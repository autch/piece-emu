// piece-emu-system — full-system frontend (SDL3 display + button input)
//
// Loads a P/ECE PFI flash image and runs the S1C33209 emulator with:
//   - SDL3 window showing the S6B0741 LCD (128×88, 4× scale)
//   - Keyboard input mapped to P/ECE buttons via PortCtrl
//   - LCD rendering driven by HSDMA Ch0 completion (matches app frame rate)
//
// Key bindings:
//   Arrow keys  → D-pad (right/left/down/up = K60/K61/K62/K63)
//   Z           → B button (K64)
//   X           → A button (K65)
//   Enter       → START (K54)
//   Backspace   → SELECT (K53)
//   Escape      → quit

#include "bus.hpp"
#include "cpu.hpp"
#include "diag.hpp"
#include "gdb_rsp.hpp"
#include "pfi_loader.hpp"
#include "peripheral_intc.hpp"
#include "peripheral_clkctl.hpp"
#include "peripheral_t8.hpp"
#include "peripheral_t16.hpp"
#include "peripheral_portctrl.hpp"
#include "peripheral_bcu_area.hpp"
#include "peripheral_wdt.hpp"
#include "peripheral_rtc.hpp"
#include "peripheral_hsdma.hpp"
#include "peripheral_sif3.hpp"
#include "s6b0741.hpp"
#include "lcd_renderer.hpp"

#include <CLI/CLI.hpp>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// CPU clock
// ---------------------------------------------------------------------------
static constexpr uint64_t CPU_HZ = 24'000'000;

// ---------------------------------------------------------------------------
// Button → K5D/K6D bit mapping
//
// K5D bits (active-low):
//   bit3 = K53 = SELECT
//   bit4 = K54 = START
//
// K6D bits (active-low):
//   bit0 = K60 = Right
//   bit1 = K61 = Left
//   bit2 = K62 = Down
//   bit3 = K63 = Up
//   bit4 = K64 = B
//   bit5 = K65 = A
// ---------------------------------------------------------------------------
struct ButtonState {
    uint8_t k5 = 0xFF; // all released (active-low)
    uint8_t k6 = 0xFF;
};

static void handle_key(bool is_down, int scancode, ButtonState& btn)
{
    auto press = [is_down](uint8_t& reg, int bit) {
        if (is_down) reg &= ~(1u << bit);
        else         reg |=  (1u << bit);
    };
    uint8_t old_k6 = btn.k6, old_k5 = btn.k5;
    switch (static_cast<SDL_Scancode>(scancode)) {
    case SDL_SCANCODE_RIGHT:      press(btn.k6, 0); break; // K60
    case SDL_SCANCODE_LEFT:       press(btn.k6, 1); break; // K61
    case SDL_SCANCODE_DOWN:       press(btn.k6, 2); break; // K62
    case SDL_SCANCODE_UP:         press(btn.k6, 3); break; // K63
    case SDL_SCANCODE_Z:          press(btn.k6, 4); break; // K64 = B
    case SDL_SCANCODE_X:          press(btn.k6, 5); break; // K65 = A
    case SDL_SCANCODE_RETURN:     press(btn.k5, 4); break; // K54 = START
    case SDL_SCANCODE_BACKSPACE:  press(btn.k5, 3); break; // K53 = SELECT
    default: break;
    }
    if (btn.k5 != old_k5 || btn.k6 != old_k6)
        std::fprintf(stderr, "[BTN] k5=0x%02X k6=0x%02X\n", btn.k5, btn.k6);
}

// ---------------------------------------------------------------------------
// Register dump
// ---------------------------------------------------------------------------
static void print_reg_snapshot(const CpuState& s)
{
    std::fprintf(stderr,
        "[SNAPSHOT] Registers:\n"
        "  R 0=%08X  R 1=%08X  R 2=%08X  R 3=%08X\n"
        "  R 4=%08X  R 5=%08X  R 6=%08X  R 7=%08X\n"
        "  R 8=%08X  R 9=%08X  R10=%08X  R11=%08X\n"
        "  R12=%08X  R13=%08X  R14=%08X  R15=%08X\n"
        "   PC=%08X   SP=%08X  PSR=%08X\n"
        "  ALR=%08X  AHR=%08X\n",
        s.r[0],  s.r[1],  s.r[2],  s.r[3],
        s.r[4],  s.r[5],  s.r[6],  s.r[7],
        s.r[8],  s.r[9],  s.r[10], s.r[11],
        s.r[12], s.r[13], s.r[14], s.r[15],
        s.pc, s.sp, s.psr.raw, s.alr, s.ahr);
}

// ---------------------------------------------------------------------------
// Debug helpers
// ---------------------------------------------------------------------------

// Parse "0xADDR" or "0xADDR:SIZE" → {addr, size}.
static std::pair<uint32_t, uint32_t> parse_addr_size(const std::string& s)
{
    auto colon = s.find(':');
    uint32_t addr = static_cast<uint32_t>(
        std::stoul(s.substr(0, colon), nullptr, 0));
    uint32_t size = 4;
    if (colon != std::string::npos)
        size = static_cast<uint32_t>(
            std::stoul(s.substr(colon + 1), nullptr, 0));
    return {addr, size};
}

// Default watchpoint hit handler: prints addr/val/width, preceding writer.
static WpCallback make_wp_callback(const Bus& bus)
{
    return [&bus](const Watchpoint& /*wp*/, uint32_t addr, uint32_t val,
                  int width, bool is_write) {
        std::fprintf(stderr,
            "[WP-%s] PC=0x%06X  addr=0x%06X  val=0x%0*X  width=%d",
            is_write ? "WRITE" : "READ",
            bus.debug_pc, addr, width * 2, val, width);
        if (is_write) {
            uint32_t prev = bus.shadow_last_writer(addr);
            if (prev != 0xFFFF'FFFFu)
                std::fprintf(stderr, "  prev_writer=0x%06X", prev);
        }
        std::fprintf(stderr, "\n");
    };
}

// ---------------------------------------------------------------------------
// LcdFrameBuf — shared pixel buffer between CPU thread and main (SDL) thread.
//
// CPU thread calls push() on HSDMA Ch0 completion.
// Main thread calls take() at its own ~60 Hz cadence and passes pixels to
// LcdRenderer::render(), which must only be called from the main thread.
//
// If multiple frames arrive before take(), the latest wins (frame drop).
// ---------------------------------------------------------------------------
struct LcdFrameBuf {
    std::mutex mtx;
    uint8_t    pixels[88][128] = {};
    bool       pending = false;

    // Called from CPU thread: snapshot LCD pixels.
    void push(const uint8_t src[88][128]) {
        std::lock_guard<std::mutex> lk(mtx);
        std::memcpy(pixels, src, sizeof(pixels));
        pending = true;
    }

    // Called from main thread: returns true and copies out if a new frame is
    // available.
    bool take(uint8_t dst[88][128]) {
        std::lock_guard<std::mutex> lk(mtx);
        if (!pending) return false;
        std::memcpy(dst, pixels, sizeof(pixels));
        pending = false;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Config — parsed CLI options
// ---------------------------------------------------------------------------
struct Config {
    std::string              pfi_path;
    bool                     trace      = false;
    uint64_t                 max_cycles = 0;
    std::size_t              sram_size  = 0x040000; // 256 KB
    std::size_t              flash_size = 0x080000; // 512 KB
    int                      scale      = 4;
    uint16_t                 gdb_port   = 0;
    bool                     gdb_debug  = false;
    std::vector<std::string> wp_write_specs, wp_read_specs, wp_rw_specs;
    std::vector<std::string> break_specs;

    static Config parse(int argc, char** argv)
    {
        Config cfg;
        CLI::App app{"P/ECE system emulator (SDL3 display)"};
        argv = app.ensure_utf8(argv);

        app.add_option("--pfi", cfg.pfi_path, "P/ECE Flash Image (.pfi) to load")
            ->required()
            ->check(CLI::ExistingFile);
        app.add_option("--max-cycles", cfg.max_cycles,
            "Stop after N cycles (default: unlimited)");
        app.add_flag("--trace", cfg.trace,
            "Print disassembly for each instruction");
        app.add_option("--scale", cfg.scale,
            "Display scale factor (default: 4 → 512×352)")
            ->check(CLI::Range(1, 8));
        app.add_option("--sram-size",  cfg.sram_size,  "SRAM size in bytes");
        app.add_option("--flash-size", cfg.flash_size, "Flash size in bytes");
        app.add_option("--gdb-port", cfg.gdb_port,
            "Start GDB RSP server on this TCP port (e.g. 1234); 0 = disabled");
        app.add_flag("--gdb-debug", cfg.gdb_debug,
            "Print GDB RSP packet traffic to stderr");
        app.add_option("--wp-write", cfg.wp_write_specs,
            "Write watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
        app.add_option("--wp-read",  cfg.wp_read_specs,
            "Read watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
        app.add_option("--wp-rw",    cfg.wp_rw_specs,
            "Read/write watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
        app.add_option("--break-at", cfg.break_specs,
            "Dump registers when PC == ADDR (hex, repeatable)");

        try { app.parse(argc, argv); }
        catch (const CLI::ParseError& e) { std::exit(app.exit(e)); }
        return cfg;
    }
};

// ---------------------------------------------------------------------------
// PiecePeripherals — all S1C33209 on-chip peripherals wired together.
// ---------------------------------------------------------------------------
struct PiecePeripherals {
    InterruptController intc;
    ClockControl        clk;
    Timer8bit           t8_ch[4]  = {Timer8bit(0),  Timer8bit(1),
                                     Timer8bit(2),  Timer8bit(3)};
    Timer16bit          t16_ch[6] = {Timer16bit(0), Timer16bit(1), Timer16bit(2),
                                     Timer16bit(3), Timer16bit(4), Timer16bit(5)};
    PortCtrl            portctrl;
    BcuAreaCtrl         bcu_area;
    WatchdogTimer       wdt;
    ClockTimer          rtc;
    Hsdma               hsdma;
    Sif3                sif3;
    S6b0741             lcd;

    uint64_t            nmi_count = 0;

    void attach(Bus& bus, Cpu& cpu)
    {
        intc.attach(bus, [&cpu](int trap_no, int level) {
            cpu.assert_trap(trap_no, level);
        });
        clk.attach(bus);
        for (int i = 0; i < 4; i++) t8_ch[i].attach(bus, intc, clk);
        for (int i = 0; i < 6; i++) t16_ch[i].attach(bus, intc, clk);
        portctrl.attach(bus, intc, &clk);
        bcu_area.attach(bus, cpu);
        wdt.attach(bus, clk, [&cpu, this](int no, int lvl) {
            ++nmi_count;
            cpu.assert_trap(no, lvl);
        });
        rtc.attach(bus, intc, clk);
        hsdma.attach(bus);
        sif3.attach(bus, intc, hsdma);
        lcd.attach(portctrl);
        sif3.set_txd_callback([this](uint8_t b) { lcd.write(b); });
    }

    void tick(uint64_t cycles)
    {
        for (int i = 0; i < 4; i++) t8_ch[i].tick(cycles);
        for (int i = 0; i < 6; i++) t16_ch[i].tick(cycles);
        wdt.tick(cycles);
        rtc.tick(cycles);
    }

    uint64_t next_wake_cycle()
    {
        uint64_t wake = UINT64_MAX;
        for (int i = 0; i < 4; i++) wake = std::min(wake, t8_ch[i].next_wake_cycle());
        for (int i = 0; i < 6; i++) wake = std::min(wake, t16_ch[i].next_wake_cycle());
        wake = std::min(wake, rtc.next_wake_cycle());
        wake = std::min(wake, wdt.next_wake_cycle());
        return wake;
    }
};

// ---------------------------------------------------------------------------
// CpuRunner — drives the CPU emulation loop in a dedicated thread.
//
// All SDL calls stay on the main thread; only SDL_GetTicksNS / SDL_DelayNS
// are used here (safe from any thread).
// ---------------------------------------------------------------------------
struct CpuRunner {
    Bus&                       bus;
    Cpu&                       cpu;
    PiecePeripherals&          periph;
    const Config&              cfg;
    uint64_t&                  total_cycles;
    const std::set<uint32_t>&  break_addrs;
    std::unique_ptr<GdbRsp>&   gdb_rsp;
    std::atomic<bool>&         quit_flag;
    std::atomic<uint16_t>&     shared_buttons;

    void run()
    {
        pthread_setname_np(pthread_self(), "piece-cpu");

        periph.portctrl.set_k5(0xFF);
        periph.portctrl.set_k6(0xFF);

        bool quit = false;

        static constexpr uint64_t EVENT_INTERVAL   = 10'000;
        // Minimum CPU cycles between do_tick() calls.  High-frequency timers
        // (e.g. audio DMA at ~44 kHz ≈ 544 cycles/interrupt) would otherwise
        // fragment the fast-path burst into tiny slices.  Each tick fires at
        // most MIN_TICK_BURST cycles late in emulated time (at 24 MHz:
        // 2000 cycles ≈ 83 µs — well within one SIF3 audio frame).
        static constexpr uint64_t MIN_TICK_BURST  =  2'000;
        uint64_t next_render     = total_cycles + periph.clk.cpu_clock_hz() / 60;
        uint64_t next_event_poll = total_cycles + EVENT_INTERVAL;
        uint64_t pace_last_cycle = total_cycles;
        uint64_t pace_last_ns    = SDL_GetTicksNS();

        // Timer-tick throttling: only call periph.tick() when the next timer
        // event is due.  next_timer_wake is capped at EVENT_INTERVAL so that
        // IO-write-induced timer config changes are picked up promptly.
        uint64_t next_timer_wake = 0;

        // Recompute the next timer wake point.
        // Must be called after every periph.tick() and after clock changes.
        // next_wake_cycle() is O(1) (cached in each timer after tick()).
        // Enforce MIN_TICK_BURST so high-frequency timers don't shatter bursts.
        auto update_timer_wake = [&]() {
            uint64_t w = periph.next_wake_cycle();
            if (w == UINT64_MAX) {
                next_timer_wake = total_cycles + EVENT_INTERVAL;
            } else {
                // Clamp: never sooner than MIN_TICK_BURST, never later than EVENT_INTERVAL.
                uint64_t earliest = total_cycles + MIN_TICK_BURST;
                uint64_t latest   = total_cycles + EVENT_INTERVAL;
                next_timer_wake   = std::clamp(w, earliest, latest);
            }
        };
        update_timer_wake();

        // Tick all peripherals and refresh the wake point.
        auto do_tick = [&]() {
            periph.tick(total_cycles);
            update_timer_wake();
        };

        // On CPU clock change: reset pace reference, render interval, and
        // timer wake point (clock change affects all timer frequencies).
        periph.clk.on_clock_change = [&](uint32_t new_hz) {
            std::fprintf(stderr, "[CLK] CPU clock: %u MHz\n",
                         new_hz / 1'000'000);
            pace_last_cycle = total_cycles;
            pace_last_ns    = SDL_GetTicksNS();
            next_render     = total_cycles + new_hz / 60;
            update_timer_wake();
        };

        // Read button state from main thread and check quit flag.
        auto poll_buttons = [&]() {
            uint16_t v = shared_buttons.load(std::memory_order_relaxed);
            periph.portctrl.set_k5(static_cast<uint8_t>(v >> 8));
            periph.portctrl.set_k6(static_cast<uint8_t>(v));
            quit = quit_flag.load(std::memory_order_relaxed);
        };

        // Real-time pacing: sleep until wall time matches simulated time.
        // Uses the current CPU clock so 24/48 MHz switches are handled correctly.
        auto sync_realtime = [&]() {
            uint32_t cur_hz      = periph.clk.cpu_clock_hz();
            uint64_t expected_ns = (total_cycles - pace_last_cycle)
                                   * 1'000'000'000ULL / cur_hz;
            uint64_t now_ns      = SDL_GetTicksNS();
            uint64_t wall_ns     = now_ns - pace_last_ns;
            if (wall_ns < expected_ns)
                SDL_DelayNS(expected_ns - wall_ns);
            pace_last_cycle = total_cycles;
            pace_last_ns    = SDL_GetTicksNS();
        };

        while (!quit && !cpu.state.fault) {

            // =================================================================
            // GDB RSP async mode
            // =================================================================
            if (gdb_rsp && gdb_rsp->has_async_client()) {
                bool single_step = false;
                if (gdb_rsp->take_async_run_cmd(&single_step)) {
                    if (single_step) {
                        if (cfg.trace) {
                            std::string dis = cpu.disasm(cpu.state.pc);
                            std::fprintf(stderr, "  %s\n", dis.c_str());
                        }
                        bus.debug_pc = cpu.state.pc;
                        total_cycles += cpu.step();
                        do_tick(); // single-step: always tick for accurate state
                        gdb_rsp->notify_async_stopped(
                            gdb_rsp->make_async_stop_reply());
                    } else {
                        // Continue until breakpoint/halt/fault/watchpoint
                        while (!cpu.state.in_halt && !cpu.state.fault) {
                            if (cfg.trace) {
                                std::string dis = cpu.disasm(cpu.state.pc);
                                std::fprintf(stderr, "  %s\n", dis.c_str());
                            }
                            bus.debug_pc = cpu.state.pc;
                            if (!break_addrs.empty()
                                    && break_addrs.count(cpu.state.pc)) {
                                std::fprintf(stderr,
                                    "[BREAK] PC=0x%06X\n", cpu.state.pc);
                                print_reg_snapshot(cpu.state);
                                break;
                            }
                            total_cycles += cpu.step();
                            if (total_cycles >= next_timer_wake)
                                do_tick();

                            if (!cpu.state.in_halt
                                    && gdb_rsp->has_breakpoint(cpu.state.pc))
                                break;

                            if (total_cycles >= next_event_poll) {
                                poll_buttons();
                                next_event_poll = total_cycles + EVENT_INTERVAL;
                                if (quit) { cpu.state.in_halt = true; break; }
                            }
                        }

                        if (cpu.state.in_halt && !cpu.state.fault) {
                            uint64_t wake = periph.next_wake_cycle();
                            if (wake != UINT64_MAX) {
                                total_cycles = wake;
                                do_tick();
                            }
                        }
                        gdb_rsp->notify_async_stopped(
                            gdb_rsp->make_async_stop_reply());
                    }
                } else {
                    // RSP connected but CPU paused — just idle
                    SDL_DelayNS(1'000'000);
                    poll_buttons();
                    pace_last_cycle = total_cycles;
                    pace_last_ns    = SDL_GetTicksNS();
                }
                continue;
            }

            // =================================================================
            // Normal mode: run CPU; pace at RENDER_INTERVAL boundary.
            //
            // Fast path: when trace/break_addrs/max_cycles/watchpoints are all
            // disabled, run a tight inner loop with only 3 conditions per
            // instruction (in_halt, fault, counter) instead of 7+.  The loop
            // is bounded by the nearest upcoming event (timer/event-poll/render)
            // so that post-loop handlers fire on time without per-step checks.
            // =================================================================
            const bool fast_path = !cfg.trace && break_addrs.empty()
                                   && !cfg.max_cycles
                                   && !bus.has_watchpoints();

            while (!cpu.state.in_halt && !cpu.state.fault && !quit
                   && total_cycles < next_render) {
                // Run to the nearest boundary in one burst.
                uint64_t stop = std::min({next_timer_wake,
                                          next_event_poll,
                                          next_render});
                if (fast_path) {
                    while (!cpu.state.in_halt && !cpu.state.fault
                           && total_cycles < stop) {
                        cpu.step();
                        ++total_cycles;
                    }
                } else {
                    while (!cpu.state.in_halt && !cpu.state.fault
                           && total_cycles < stop) {
                        if (cfg.trace) {
                            std::string dis = cpu.disasm(cpu.state.pc);
                            std::fprintf(stderr, "  %s\n", dis.c_str());
                        }
                        bus.debug_pc = cpu.state.pc;
                        if (!break_addrs.empty()
                                && break_addrs.count(cpu.state.pc)) {
                            std::fprintf(stderr,
                                "[BREAK] PC=0x%06X\n", cpu.state.pc);
                            print_reg_snapshot(cpu.state);
                        }
                        cpu.step();
                        ++total_cycles;
                        if (cfg.max_cycles
                                && total_cycles >= cfg.max_cycles) {
                            std::fprintf(stderr,
                                "Reached max-cycles limit (%llu)\n",
                                (unsigned long long)total_cycles);
                            quit = true;
                            break;
                        }
                    }
                }

                if (cpu.state.fault || quit) break;

                if (total_cycles >= next_timer_wake) do_tick();
                if (total_cycles >= next_event_poll) {
                    poll_buttons();
                    next_event_poll = total_cycles + EVENT_INTERVAL;
                }
            }

            if (cpu.state.fault || quit) break;

            if (cpu.state.in_halt) {
                uint64_t wake = periph.next_wake_cycle();
                if (wake == UINT64_MAX) {
                    std::fprintf(stderr,
                        "Deadlock: CPU halted with no wakeup after %llu cycles\n",
                        (unsigned long long)total_cycles);
                    break;
                }
                uint64_t target = std::min(wake, next_render);
                uint64_t delta  = target - total_cycles;
                if (delta > periph.clk.cpu_clock_hz()) {
                    std::fprintf(stderr,
                        "[HALT-JUMP] delta=%llu wake=%llu wdt=%llu"
                        " t16[0]=%llu t16[1]=%llu\n",
                        (unsigned long long)delta,
                        (unsigned long long)wake,
                        (unsigned long long)periph.wdt.next_wake_cycle(),
                        (unsigned long long)periph.t16_ch[0].next_wake_cycle(),
                        (unsigned long long)periph.t16_ch[1].next_wake_cycle());
                }
                total_cycles = target;
                do_tick(); // time jump: process all timer events up to new time
            }

            if (total_cycles >= next_render) {
                sync_realtime();
                next_render += periph.clk.cpu_clock_hz() / 60;
            }
        }

        periph.clk.on_clock_change = nullptr; // remove dangling lambda refs
        quit_flag.store(true, std::memory_order_relaxed); // wake main thread
    }
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    pthread_setname_np(pthread_self(), "piece-sdl");

    Config cfg = Config::parse(argc, argv);

    try {
        Bus bus(cfg.sram_size, cfg.flash_size);
        Cpu cpu(bus);

        StderrDiagSink diag_sink;
        cpu.set_diag(&diag_sink);
        bus.set_diag(&diag_sink);

        // Watchpoints
        for (auto& s : cfg.wp_write_specs) { auto [a,sz]=parse_addr_size(s); bus.add_watchpoint(a,sz,WpType::WRITE); }
        for (auto& s : cfg.wp_read_specs)  { auto [a,sz]=parse_addr_size(s); bus.add_watchpoint(a,sz,WpType::READ);  }
        for (auto& s : cfg.wp_rw_specs)    { auto [a,sz]=parse_addr_size(s); bus.add_watchpoint(a,sz,WpType::RW);    }
        bus.set_wp_callback(make_wp_callback(bus));

        std::set<uint32_t> break_addrs;
        for (auto& s : cfg.break_specs)
            break_addrs.insert(static_cast<uint32_t>(std::stoul(s, nullptr, 0)));

        uint64_t total_cycles = 0;

        // Peripherals
        PiecePeripherals periph;
        periph.attach(bus, cpu);

        // Snapshot LCD pixels to shared buffer on each HSDMA Ch0 completion.
        uint64_t    hsdma_frame_no = 0;
        LcdFrameBuf frame_buf;
        periph.hsdma.on_ch0_complete = [&]() {
            ++hsdma_frame_no;
            if (false && hsdma_frame_no % 100 == 0) {
                std::fprintf(stderr,
                    "[FRAME] #%llu pc=0x%06X halt=%d nmi=%llu total=%llu\n",
                    (unsigned long long)hsdma_frame_no,
                    cpu.state.pc, (int)cpu.state.in_halt,
                    (unsigned long long)periph.nmi_count,
                    (unsigned long long)total_cycles);
            }
            uint8_t px[88][128];
            periph.lcd.to_pixels(px);
            frame_buf.push(px);
        };

        // Load PFI flash image
        PfiInfo pfi_info = pfi_load(bus, cfg.pfi_path);
        (void)pfi_info;
        uint32_t entry = bus.read32(Bus::FLASH_BASE);
        std::fprintf(stderr, "PFI loaded, reset vector=0x%06X\n", entry);
        cpu.state.pc = entry;

        // SDL3 renderer (must stay on this thread for SDL_RenderPresent)
        LcdRenderer renderer;
        if (!renderer.init(cfg.scale)) {
            std::fprintf(stderr, "Failed to initialise SDL3 renderer\n");
            return 1;
        }

        // GDB RSP server (async, background thread)
        std::unique_ptr<GdbRsp> gdb_rsp;
        if (cfg.gdb_port > 0) {
            gdb_rsp = std::make_unique<GdbRsp>(bus, cpu, cfg.gdb_port, cfg.gdb_debug);
            gdb_rsp->serve_async();
        }

        // Shared state: buttons and quit signal
        std::atomic<bool>     quit_flag{false};
        std::atomic<uint16_t> shared_buttons{0xFFFFu};

        // CPU thread
        CpuRunner runner{bus, cpu, periph, cfg, total_cycles,
                         break_addrs, gdb_rsp, quit_flag, shared_buttons};
        std::thread cpu_thread([&runner]{ runner.run(); });

        // Main thread: SDL event loop + rendering
        ButtonState btn;
        uint8_t     px[88][128];

        while (!quit_flag.load(std::memory_order_relaxed)) {
            if (!renderer.poll_events([&](bool is_down, int sc) {
                    if (is_down && sc == SDL_SCANCODE_ESCAPE) {
                        quit_flag.store(true, std::memory_order_relaxed);
                        return;
                    }
                    handle_key(is_down, sc, btn);
                    shared_buttons.store(
                        (static_cast<uint16_t>(btn.k5) << 8) | btn.k6,
                        std::memory_order_relaxed);
                })) {
                quit_flag.store(true, std::memory_order_relaxed);
            }

            if (frame_buf.take(px))
                renderer.render(px);

            SDL_DelayNS(16'666'667ULL); // ~60 fps polling cadence
        }

        cpu_thread.join();
        renderer.destroy();

        if (cpu.state.fault)
            std::fprintf(stderr, "Faulted after %llu cycles\n",
                (unsigned long long)total_cycles);
        else
            std::fprintf(stderr, "Stopped after %llu cycles\n",
                (unsigned long long)total_cycles);

        print_reg_snapshot(cpu.state);
        return cpu.state.fault ? 1 : 0;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
