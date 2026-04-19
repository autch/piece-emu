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
//   F5          → hot start (reset CPU + on-chip peripherals)
//   Shift+F5    → cold start (also resets BCU area / PortCtrl / LCD)
//   F12         → save PNG screenshot
//   Escape      → quit

#include "bus.hpp"
#include "cpu.hpp"
#include "debug_utils.hpp"
#include "diag.hpp"
#include "gdb_rsp.hpp"
#include "pfi_loader.hpp"
#include "piece_peripherals.hpp"
#include "cpu_runner.hpp"
#include "lcd_renderer.hpp"
#include "lcd_framebuf.hpp"
#include "button_input.hpp"
#include "cli_config.hpp"
#include "audio_output.hpp"
#include "audio_log.hpp"
#include "screenshot.hpp"

#include <SDL3/SDL.h>
#ifdef _WIN32
#include <SDL3/SDL_main.h>
#endif
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#  include <pthread.h>
#endif

#include <atomic>
#include <cstdio>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

// Heap-allocated wrapper for the main thread's last-rendered frame buffer.
// (Used by F12 screenshot and fed into LcdRenderer::render each take()).
// Wrapped in a struct so std::make_unique can own the 11 KB block —
// unique_ptr<uint8_t[88][128]> is not directly expressible.
namespace { struct FrameSnap { uint8_t p[88][128] = {}; }; }

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
#if defined(_WIN32)
    ::SetThreadDescription(::GetCurrentThread(), L"piece-sdl");
#elif defined(__APPLE__)
    pthread_setname_np("piece-sdl");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "piece-sdl");
#endif

    Config cfg = Config::parse(argc, argv);

    try {
        // Read PFI header first to determine hardware memory sizes.
        // Override defaults unless the user explicitly passed --sram-size /
        // --flash-size on the command line.
        {
            SYSTEMINFO si = pfi_read_sysinfo(cfg.pfi_path);
            if (!cfg.sram_size_explicit && si.sram_end > si.sram_top) {
                cfg.sram_size = si.sram_end - si.sram_top;
                std::fprintf(stderr, "PFI SYSTEMINFO: sram_size=0x%X bytes\n",
                             static_cast<unsigned>(cfg.sram_size));
            }
            if (!cfg.flash_size_explicit && si.pffs_end > Bus::FLASH_BASE) {
                cfg.flash_size = si.pffs_end - Bus::FLASH_BASE;
                std::fprintf(stderr, "PFI SYSTEMINFO: flash_size=0x%X bytes\n",
                             static_cast<unsigned>(cfg.flash_size));
            }
        }

        // All large/composite state lives on the heap.  The main thread's
        // stack on Windows is only 1 MB, and the aggregate footprint of
        // PiecePeripherals + LcdFrameBuf + CPU frame snapshot alone is
        // ~50 KB — well within budget, but unique_ptr ownership keeps the
        // intent explicit and guarantees pointer-stability for the CPU
        // thread which borrows references to these objects.
        auto bus = std::make_unique<Bus>(cfg.sram_size, cfg.flash_size);
        auto cpu = std::make_unique<Cpu>(*bus);

        StderrDiagSink diag_sink;
        cpu->set_diag(&diag_sink);
        bus->set_diag(&diag_sink);

        // Watchpoints
        for (auto& s : cfg.wp_write_specs) { auto [a,sz]=parse_addr_size(s); bus->add_watchpoint(a,sz,WpType::WRITE); }
        for (auto& s : cfg.wp_read_specs)  { auto [a,sz]=parse_addr_size(s); bus->add_watchpoint(a,sz,WpType::READ);  }
        for (auto& s : cfg.wp_rw_specs)    { auto [a,sz]=parse_addr_size(s); bus->add_watchpoint(a,sz,WpType::RW);    }
        bus->set_wp_callback(make_wp_callback(*bus));

        std::set<uint32_t> break_addrs;
        for (auto& s : cfg.break_specs)
            break_addrs.insert(static_cast<uint32_t>(std::stoul(s, nullptr, 0)));

        uint64_t total_cycles = 0;

        // Peripherals
        auto periph = std::make_unique<PiecePeripherals>();
        periph->attach(*bus, *cpu);

        // Sound: wire HSDMA Ch1 → PWM sample ring buffer.  Completion is
        // throttled by SDL audio consumption (see peripheral_sound.cpp), so
        // no cycle provider is needed.
        periph->sound.attach(*bus, periph->hsdma, periph->intc, periph->clk,
                             [&total_cycles]() { return total_cycles; },
                             &periph->t16_ch[1]);
        periph->sound.set_trace(cfg.audio_trace);

        // Snapshot LCD pixels to shared buffer on each HSDMA Ch0 completion.
        uint64_t hsdma_frame_no = 0;
        auto     frame_buf      = std::make_unique<LcdFrameBuf>();
        periph->hsdma.on_ch0_complete = [&]() {
            ++hsdma_frame_no;
            if (false && hsdma_frame_no % 100 == 0) {
                std::fprintf(stderr,
                    "[FRAME] #%llu pc=0x%06X halt=%d nmi=%llu total=%llu\n",
                    (unsigned long long)hsdma_frame_no,
                    cpu->state.pc, (int)cpu->state.in_halt,
                    (unsigned long long)periph->nmi_count,
                    (unsigned long long)total_cycles);
            }
            frame_buf->push(periph->lcd);
        };

        // Load PFI flash image
        PfiInfo pfi_info = pfi_load(*bus, cfg.pfi_path);
        (void)pfi_info;
        uint32_t entry = bus->read32(Bus::FLASH_BASE);
        std::fprintf(stderr, "PFI loaded, reset vector=0x%06X\n", entry);
        cpu->state.pc = entry;

        // SDL3 renderer (must stay on this thread for SDL_RenderPresent)
        auto renderer = std::make_unique<LcdRenderer>();
        if (!renderer->init(cfg.scale)) {
            std::fprintf(stderr, "Failed to initialise SDL3 renderer\n");
            return 1;
        }

        // SDL3 audio (optional).  Callback pulls samples from periph->sound
        // on the SDL audio thread; the SPSC ring buffer is lock-free.
        auto audio = std::make_unique<AudioOutput>();
        if (!cfg.no_audio) {
            audio->set_trace(cfg.audio_trace);
            if (!audio->open(periph->sound))
                std::fprintf(stderr, "Audio disabled (open failed)\n");
        }

        // Optional audio event log (diagnostic).  Writer is lock-guarded
        // with a background flush thread, so real-time paths never call
        // fprintf directly.
        auto audio_log = std::make_unique<AudioLog>();
        if (!cfg.audio_log_path.empty()) {
            if (audio_log->open(cfg.audio_log_path)) {
                audio->set_log(audio_log.get());
                periph->sound.on_push =
                    [log = audio_log.get()](uint64_t cyc, std::size_t cnt,
                                            std::size_t avail, uint64_t dropped) {
                        log->log_push(cyc,
                            static_cast<int64_t>(cnt),
                            static_cast<int64_t>(avail),
                            static_cast<int64_t>(dropped));
                    };
                std::fprintf(stderr, "Audio log: %s\n",
                             cfg.audio_log_path.c_str());
            } else {
                std::fprintf(stderr, "Audio log open failed: %s\n",
                             cfg.audio_log_path.c_str());
            }
        }

        // GDB RSP server (async, background thread)
        std::unique_ptr<GdbRsp> gdb_rsp;
        if (cfg.gdb_port > 0) {
            gdb_rsp = std::make_unique<GdbRsp>(*bus, *cpu, cfg.gdb_port, cfg.gdb_debug);
            gdb_rsp->serve_async();
        }

        // Shared state: buttons and quit signal
        std::atomic<bool>     quit_flag{false};
        std::atomic<uint16_t> shared_buttons{0xFFFFu};

        // CPU thread.  CpuRunner is an aggregate with reference members +
        // a default-initialised std::atomic<int> reset_request{0} at the
        // end, which rules out std::make_unique (pre-P0960 parenthesised
        // aggregate init is flaky across compilers).  Brace-new + unique_ptr
        // is the portable path.
        auto runner = std::unique_ptr<CpuRunner>(new CpuRunner{
            *bus, *cpu, *periph, cfg, total_cycles,
            break_addrs, gdb_rsp, quit_flag, shared_buttons,
            audio.get(), audio_log.get()
        });
        std::thread cpu_thread([&runner]{ runner->run(); });

        // Main thread: SDL event loop + rendering.
        // `px_owner` owns the 11 KB last-rendered frame (heap); `px` is
        // a reference to its pixel array so existing functions that take
        // `uint8_t[88][128]` keep working via array-to-pointer decay.
        ButtonState btn;
        auto  px_owner = std::make_unique<FrameSnap>();
        auto& px       = px_owner->p;

        while (!quit_flag.load(std::memory_order_relaxed)) {
            if (!renderer->poll_events([&](bool is_down, int sc) {
                    if (is_down) {
                        if (sc == SDL_SCANCODE_ESCAPE) {
                            quit_flag.store(true, std::memory_order_relaxed);
                            return;
                        }
                        if (sc == SDL_SCANCODE_F12) {
                            save_screenshot_png(cfg.snapshot_path, px);
                            return;
                        }
                        if (sc == SDL_SCANCODE_F5) {
                            // Shift+F5 = cold start, F5 = hot start.
                            bool cold = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                            runner->request_reset(cold);
                            return;
                        }
                    }
                    handle_key(is_down, sc, btn);
                    shared_buttons.store(
                        (static_cast<uint16_t>(btn.k5) << 8) | btn.k6,
                        std::memory_order_relaxed);
                })) {
                quit_flag.store(true, std::memory_order_relaxed);
            }

            if (frame_buf->take(px))
                renderer->render(px);

            SDL_DelayNS(16'666'667ULL); // ~60 fps polling cadence
        }

        cpu_thread.join();
        audio->close();
        audio_log->close();
        renderer->destroy();

        if (cpu->state.fault)
            std::fprintf(stderr, "Faulted after %llu cycles\n",
                (unsigned long long)total_cycles);
        else
            std::fprintf(stderr, "Stopped after %llu cycles\n",
                (unsigned long long)total_cycles);

        print_reg_snapshot(cpu->state);
        return cpu->state.fault ? 1 : 0;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
