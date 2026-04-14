// piece-emu-system — full-system frontend (SDL3 display + button input)
//
// Loads a P/ECE PFI flash image and runs the S1C33209 emulator with:
//   - SDL3 window showing the S6B0741 LCD (128×88, 4× scale)
//   - Keyboard input mapped to P/ECE buttons via PortCtrl
//   - Frame rate ~60 fps (400 000 cycles per frame at 24 MHz)
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
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// CPU clock and frame timing
// ---------------------------------------------------------------------------
static constexpr uint64_t CPU_HZ          = 24'000'000;
static constexpr uint64_t CYCLES_PER_FRAME = CPU_HZ / 60; // ~400 000

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
// Register dump (same as in main.cpp)
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
// Debug helpers shared between main.cpp and system_main.cpp
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
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    CLI::App app{"P/ECE system emulator (SDL3 display)"};
    argv = app.ensure_utf8(argv);

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

    app.add_option("--pfi", pfi_path, "P/ECE Flash Image (.pfi) to load")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("--max-cycles", max_cycles,
        "Stop after N cycles (default: unlimited)");
    app.add_flag("--trace", trace,
        "Print disassembly for each instruction");
    app.add_option("--scale", scale,
        "Display scale factor (default: 4 → 512×352)")
        ->check(CLI::Range(1, 8));
    app.add_option("--sram-size",  sram_size,  "SRAM size in bytes");
    app.add_option("--flash-size", flash_size, "Flash size in bytes");
    app.add_option("--gdb-port", gdb_port,
        "Start GDB RSP server on this TCP port (e.g. 1234); 0 = disabled");
    app.add_flag("--gdb-debug", gdb_debug,
        "Print GDB RSP packet traffic to stderr");
    app.add_option("--wp-write", wp_write_specs,
        "Write watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
    app.add_option("--wp-read",  wp_read_specs,
        "Read watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
    app.add_option("--wp-rw",    wp_rw_specs,
        "Read/write watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
    app.add_option("--break-at", break_specs,
        "Dump registers when PC == ADDR (hex, repeatable)");

    CLI11_PARSE(app, argc, argv);

    try {
        Bus bus(sram_size, flash_size);
        Cpu cpu(bus);

        StderrDiagSink diag_sink;
        cpu.set_diag(&diag_sink);
        bus.set_diag(&diag_sink);

        // ---- Debug: watchpoints & break-at ----------------------------------
        for (auto& s : wp_write_specs) {
            auto [a, sz] = parse_addr_size(s);
            bus.add_watchpoint(a, sz, WpType::WRITE);
        }
        for (auto& s : wp_read_specs) {
            auto [a, sz] = parse_addr_size(s);
            bus.add_watchpoint(a, sz, WpType::READ);
        }
        for (auto& s : wp_rw_specs) {
            auto [a, sz] = parse_addr_size(s);
            bus.add_watchpoint(a, sz, WpType::RW);
        }
        // GDB RSP watchpoints will override this callback when a client connects.
        // Install the stderr logger as the default (active when no RSP client).
        bus.set_wp_callback(make_wp_callback(bus));

        std::set<uint32_t> break_addrs;
        for (auto& s : break_specs)
            break_addrs.insert(static_cast<uint32_t>(std::stoul(s, nullptr, 0)));
        // ---------------------------------------------------------------------

        uint64_t total_cycles = 0;

        // Peripheral initialisation (identical to main.cpp)
        InterruptController intc;
        intc.attach(bus, [&cpu](int trap_no, int level) {
            cpu.assert_trap(trap_no, level);
        });

        ClockControl clk;
        clk.attach(bus);

        Timer8bit  t8_ch[4]  = {Timer8bit(0),  Timer8bit(1),  Timer8bit(2),  Timer8bit(3)};
        Timer16bit t16_ch[6] = {Timer16bit(0), Timer16bit(1), Timer16bit(2),
                                Timer16bit(3), Timer16bit(4), Timer16bit(5)};
        for (int i = 0; i < 4; i++) t8_ch[i].attach(bus, intc, clk);
        for (int i = 0; i < 6; i++) t16_ch[i].attach(bus, intc, clk);

        PortCtrl portctrl;
        portctrl.attach(bus, intc, &clk);

        BcuAreaCtrl bcu_area;
        bcu_area.attach(bus, cpu);

        uint64_t nmi_count = 0;
        WatchdogTimer wdt;
        wdt.attach(bus, clk,
            [&cpu, &nmi_count](int no, int lvl) {
                ++nmi_count;
                cpu.assert_trap(no, lvl);
            });

        ClockTimer rtc;
        rtc.attach(bus, intc, clk);

        Hsdma hsdma;
        hsdma.attach(bus);

        Sif3 sif3;
        sif3.attach(bus, intc, hsdma);

        // LCD controller — receives bytes from SIF3
        S6b0741 lcd;
        lcd.attach(portctrl);
        sif3.set_txd_callback([&lcd](uint8_t b) { lcd.write(b); });

        // Load PFI flash image
        PfiInfo pfi_info = pfi_load(bus, pfi_path);
        (void)pfi_info;
        uint32_t entry = bus.read32(Bus::FLASH_BASE);
        std::fprintf(stderr, "PFI loaded, reset vector=0x%06X\n", entry);
        cpu.state.pc = entry;

        // SDL3 renderer
        LcdRenderer renderer;
        if (!renderer.init(scale)) {
            std::fprintf(stderr, "Failed to initialise SDL3 renderer\n");
            return 1;
        }

        // ---- GDB RSP server (async, background thread) ----------------------
        std::unique_ptr<GdbRsp> gdb_rsp;
        if (gdb_port > 0) {
            gdb_rsp = std::make_unique<GdbRsp>(bus, cpu, gdb_port, gdb_debug);
            gdb_rsp->serve_async();
        }

        auto tick_all = [&](uint64_t cycles) {
            for (int i = 0; i < 4; i++) t8_ch[i].tick(cycles);
            for (int i = 0; i < 6; i++) t16_ch[i].tick(cycles);
            wdt.tick(cycles);
            rtc.tick(cycles);
        };

        auto find_next_wake = [&]() -> uint64_t {
            uint64_t wake = UINT64_MAX;
            for (int i = 0; i < 4; i++) wake = std::min(wake, t8_ch[i].next_wake_cycle());
            for (int i = 0; i < 6; i++) wake = std::min(wake, t16_ch[i].next_wake_cycle());
            wake = std::min(wake, rtc.next_wake_cycle());
            wake = std::min(wake, wdt.next_wake_cycle());
            return wake;
        };

        ButtonState btn;
        // Initialise PortCtrl input state explicitly (all buttons released).
        portctrl.set_k5(btn.k5);
        portctrl.set_k6(btn.k6);

        uint64_t next_frame = total_cycles + CYCLES_PER_FRAME;
        bool quit = false;

        // Wall-clock frame pacing: target ~16 ms per frame (60 fps).
        static constexpr uint64_t FRAME_NS = 1'000'000'000ULL / 60;
        uint64_t frame_start_ns = SDL_GetTicksNS();

        // Intra-frame event polling interval (cycles).
        // Events are drained at this interval so that key presses shorter than
        // one 400K-cycle frame are not missed by pcePadGetProc.
        // ~10 000 cycles ≈ 2 400 polls/s at 24 MHz.
        static constexpr uint64_t EVENT_INTERVAL = 10'000;
        uint64_t next_event_poll = total_cycles + EVENT_INTERVAL;

        auto poll_events = [&]() {
            if (!renderer.poll_events([&](bool is_down, int sc) {
                if (is_down && sc == SDL_SCANCODE_ESCAPE) { quit = true; return; }
                handle_key(is_down, sc, btn);
                portctrl.set_k5(btn.k5);
                portctrl.set_k6(btn.k6);
            })) {
                quit = true;
            }
        };

        // Helper: render current LCD VRAM to the SDL window.
        auto render_frame = [&]() {
            static uint64_t frame_no = 0;
            ++frame_no;
            if (frame_no % 60 == 0) {
                std::fprintf(stderr,
                    "[FRAME] #%llu pc=0x%06X halt=%d nmi=%llu total=%llu\n",
                    (unsigned long long)frame_no,
                    cpu.state.pc,
                    (int)cpu.state.in_halt,
                    (unsigned long long)nmi_count,
                    (unsigned long long)total_cycles);
            }
            uint8_t px[88][128];
            lcd.to_pixels(px);
            renderer.render(px);
        };

        // Helper: real-time pacing — sleep to stay at ~60 fps.
        auto pace_frame = [&]() {
            uint64_t now_ns    = SDL_GetTicksNS();
            uint64_t elapsed   = now_ns - frame_start_ns;
            if (elapsed < FRAME_NS)
                SDL_DelayNS(FRAME_NS - elapsed);
            frame_start_ns = SDL_GetTicksNS();
            next_frame += CYCLES_PER_FRAME;
        };

        while (!quit && !cpu.state.fault) {

            // =================================================================
            // GDB RSP async mode: RSP client is connected.
            // CPU stepping is driven by RSP run commands ('c'/'s').
            // SDL rendering still happens at frame rate.
            // =================================================================
            if (gdb_rsp && gdb_rsp->has_async_client()) {
                bool single_step = false;
                if (gdb_rsp->take_async_run_cmd(&single_step)) {
                    if (single_step) {
                        // --- Step one instruction ----------------------------
                        if (trace) {
                            std::string dis = cpu.disasm(cpu.state.pc);
                            std::fprintf(stderr, "  %s\n", dis.c_str());
                        }
                        bus.debug_pc = cpu.state.pc;
                        total_cycles += cpu.step();
                        tick_all(total_cycles);
                        gdb_rsp->notify_async_stopped(
                            gdb_rsp->make_async_stop_reply());
                    } else {
                        // --- Continue until breakpoint/halt/fault/watchpoint -
                        while (!cpu.state.in_halt && !cpu.state.fault) {
                            if (trace) {
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
                            tick_all(total_cycles);

                            // RSP breakpoints take priority over in_halt check
                            if (!cpu.state.in_halt
                                    && gdb_rsp->has_breakpoint(cpu.state.pc))
                                break;

                            // Render + handle SDL events at each frame boundary
                            // so the window stays responsive during long runs.
                            if (total_cycles >= next_event_poll) {
                                poll_events();
                                next_event_poll = total_cycles + EVENT_INTERVAL;
                            }
                            if (total_cycles >= next_frame) {
                                render_frame();
                                poll_events();
                                pace_frame();
                                if (quit) {
                                    cpu.state.in_halt = true;
                                    break;
                                }
                            }
                        }

                        // Handle HLT: fast-forward to nearest wakeup
                        if (cpu.state.in_halt && !cpu.state.fault) {
                            uint64_t wake = find_next_wake();
                            if (wake != UINT64_MAX) {
                                total_cycles = wake;
                                tick_all(total_cycles);
                            }
                        }

                        gdb_rsp->notify_async_stopped(
                            gdb_rsp->make_async_stop_reply());
                    }
                } else {
                    // RSP client connected but CPU paused (waiting for command).
                    // Keep SDL window alive and handle events.
                    if (total_cycles >= next_frame) {
                        render_frame();
                        poll_events();
                        pace_frame();
                    } else {
                        poll_events();
                        SDL_DelayNS(1'000'000); // 1 ms idle sleep
                    }
                }
                continue; // back to top of outer while
            }

            // =================================================================
            // Normal (no RSP client) mode: run CPU freely at 60 fps.
            // =================================================================

            // --- Run CPU for one frame ---
            while (!cpu.state.in_halt && !cpu.state.fault
                   && total_cycles < next_frame) {
                if (trace) {
                    std::string dis = cpu.disasm(cpu.state.pc);
                    std::fprintf(stderr, "  %s\n", dis.c_str());
                }
                bus.debug_pc = cpu.state.pc;
                if (!break_addrs.empty() && break_addrs.count(cpu.state.pc)) {
                    std::fprintf(stderr, "[BREAK] PC=0x%06X\n", cpu.state.pc);
                    print_reg_snapshot(cpu.state);
                }
                total_cycles += cpu.step();
                tick_all(total_cycles);

                // Drain SDL events at intra-frame intervals so short presses
                // (press+release within one 400K-cycle frame) are not missed.
                if (total_cycles >= next_event_poll) {
                    poll_events();
                    next_event_poll = total_cycles + EVENT_INTERVAL;
                }
            }

            if (cpu.state.fault) break;
            if (max_cycles && total_cycles >= max_cycles) {
                std::fprintf(stderr, "Reached max-cycles limit (%llu)\n",
                    (unsigned long long)total_cycles);
                break;
            }

            // Handle HLT/SLEEP: fast-forward to nearest wakeup within frame.
            if (cpu.state.in_halt) {
                uint64_t wake = find_next_wake();
                if (wake == UINT64_MAX) {
                    std::fprintf(stderr,
                        "Deadlock: CPU halted with no wakeup after %llu cycles\n",
                        (unsigned long long)total_cycles);
                    break;
                }
                uint64_t target = std::min(wake, next_frame);
                uint64_t delta = target - total_cycles;
                if (delta > CPU_HZ) { // more than 1 second fast-forward
                    std::fprintf(stderr,
                        "[HALT-JUMP] delta=%llu wake=%llu wdt=%llu"
                        " t16[0]=%llu t16[1]=%llu\n",
                        (unsigned long long)delta,
                        (unsigned long long)wake,
                        (unsigned long long)wdt.next_wake_cycle(),
                        (unsigned long long)t16_ch[0].next_wake_cycle(),
                        (unsigned long long)t16_ch[1].next_wake_cycle());
                }
                total_cycles = target;
                tick_all(total_cycles);
            }

            // --- Frame boundary: render + events ---
            if (total_cycles >= next_frame) {
                render_frame();
                poll_events();
                pace_frame();
            }
        }

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
