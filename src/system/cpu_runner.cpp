#include "cpu_runner.hpp"
#include "cli_config.hpp"
#include "piece_peripherals.hpp"

#include "bus.hpp"
#include "cpu.hpp"
#include "debug_utils.hpp"
#include "gdb_rsp.hpp"
#include "audio_output.hpp"
#include "audio_log.hpp"

#include <SDL3/SDL.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#  include <pthread.h>
#endif

#include <algorithm>
#include <cstdio>
#include <string>

void CpuRunner::run()
{
#if defined(_WIN32)
    ::SetThreadDescription(::GetCurrentThread(), L"piece-cpu");
#elif defined(__APPLE__)
    pthread_setname_np("piece-cpu");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "piece-cpu");
#endif
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
    // Also re-poll INTC so interrupts pended while PSR.IL was high get
    // delivered as soon as IL drops.  Without this, raise() would only
    // try once and drop the IRQ if IL was temporarily high (e.g. during
    // the sound ISR's SET_SIL5 phase).
    auto do_tick = [&]() {
        periph.tick(total_cycles);
        periph.intc.set_current_il(cpu.state.psr.il());
        periph.intc.poll();
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

    // Real-time pacing.
    //
    // Primary mode (audio-clock): when SDL audio is actively consuming
    // samples, pace on the audio queue depth instead of wall time.  We
    // keep the SDL stream near TARGET_LEAD_NS of buffered samples; if it
    // grows beyond that the CPU sleeps, otherwise it runs free.  This
    // naturally builds a 1–2 frame lead that absorbs transient CPU
    // stalls (muslib / make_xwave heavy paths) without starving SDL.
    //
    // Fallback mode (wall-clock): when audio is absent or not yet
    // running, pace so emu-time doesn't run ahead of wall-time.
    //
    // A per-tick cap keeps the loop responsive to button input and
    // quit requests even if the audio queue is massively overfilled.
    auto sync_realtime = [&]() {
        // Target ring-buffer fill = 50 ms @ 32 kHz = 1600 samples.
        // Must exceed SDL's single pull burst (~1365 samples every
        // 42 ms): SDL drains the ring in one go then stays silent for
        // ~42 ms, so avail must be > pull-size before each burst to
        // avoid underrun.  RING_SIZE (4096) provides the headroom
        // above TARGET_FILL so emu bursts don't hit the cap and drop
        // samples — the "ring must be ~2× max usage" rule.
        constexpr std::size_t TARGET_FILL   = 1600;
        constexpr uint64_t    MAX_SLEEP_NS  = 20'000'000ULL;
        constexpr uint64_t    NS_PER_SAMPLE = 1'000'000'000ULL
                                            / Sound::SAMPLE_RATE;
        // Consider audio "active" while pushes have occurred within the
        // last ~100 ms of emulated time (≈2.4M cycles @ 24 MHz, 4.8M @
        // 48 MHz; scale via cur_hz below).  Outside that window the app
        // is silent, so fall back to wall-clock pacing.
        uint32_t cur_hz              = periph.clk.cpu_clock_hz();
        uint64_t active_window_cyc   = cur_hz / 10ULL; // 100 ms

        auto audio_producing = [&]() -> bool {
            if (!audio_out || !audio_out->is_open()) return false;
            uint64_t last = periph.sound.last_push_cycle();
            if (last == 0) return false;
            return (total_cycles - last) < active_window_cyc;
        };

        if (audio_producing()) {
            std::size_t avail = periph.sound.available();
            uint64_t sleep_ns = 0;
            if (avail > TARGET_FILL) {
                uint64_t excess = avail - TARGET_FILL;
                sleep_ns = excess * NS_PER_SAMPLE;
                if (sleep_ns > MAX_SLEEP_NS) sleep_ns = MAX_SLEEP_NS;
                SDL_DelayNS(sleep_ns);
            }
            if (audio_log)
                audio_log->log_pace(total_cycles, 0,
                                    static_cast<int64_t>(avail),
                                    static_cast<int64_t>(sleep_ns));
            // Keep wall-clock reference fresh for clean fallback.
            pace_last_cycle = total_cycles;
            pace_last_ns    = SDL_GetTicksNS();
            return;
        }

        uint64_t expected_ns = (total_cycles - pace_last_cycle)
                               * 1'000'000'000ULL / cur_hz;
        uint64_t now_ns      = SDL_GetTicksNS();
        uint64_t wall_ns     = now_ns - pace_last_ns;
        uint64_t sleep_ns    = 0;
        if (wall_ns < expected_ns) {
            sleep_ns = expected_ns - wall_ns;
            SDL_DelayNS(sleep_ns);
        }
        if (audio_log)
            audio_log->log_pace(total_cycles, 1,
                                static_cast<int64_t>(periph.sound.available()),
                                static_cast<int64_t>(sleep_ns));
        pace_last_cycle = total_cycles;
        pace_last_ns    = SDL_GetTicksNS();
    };

    while (!quit && !cpu.state.fault) {

        // -----------------------------------------------------------------
        // Reset handshake (main thread → CPU thread).
        //
        // F5 / Shift+F5 in the SDL event loop set reset_request.  We
        // acknowledge it at this outer-loop boundary so no instruction is
        // torn mid-flight, then re-seed CPU + peripheral state and the
        // pacing/wake anchors.  total_cycles is left monotonic (only
        // deltas matter for pacing).
        // -----------------------------------------------------------------
        if (int req = reset_request.load(std::memory_order_acquire)) {
            reset_request.store(0, std::memory_order_release);
            bool cold = (req == 2);
            std::fprintf(stderr,
                "[RESET] %s start at cycle %llu\n",
                cold ? "COLD" : "HOT",
                static_cast<unsigned long long>(total_cycles));
            periph.reset(cold);
            cpu.reset(); // reads fresh reset vector from 0xC00000
            // Re-apply the button state we last received — portctrl
            // was wiped on cold start, and even on hot start we need
            // to re-drive K5D/K6D.
            periph.portctrl.set_k5(0xFF);
            periph.portctrl.set_k6(0xFF);
            uint16_t btns = shared_buttons.load(std::memory_order_relaxed);
            periph.portctrl.set_k5(static_cast<uint8_t>(btns >> 8));
            periph.portctrl.set_k6(static_cast<uint8_t>(btns));
            // Re-seed pacing anchors and wake schedules.
            pace_last_cycle = total_cycles;
            pace_last_ns    = SDL_GetTicksNS();
            next_render     = total_cycles + periph.clk.cpu_clock_hz() / 60;
            next_event_poll = total_cycles + EVENT_INTERVAL;
            update_timer_wake();
            periph.intc.set_current_il(cpu.state.psr.il());
            continue;
        }

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
                        bool sleep = (cpu.state.halt_mode
                                      == CpuState::HaltMode::Slp);
                        uint64_t wake = sleep
                            ? periph.sleep_wake_cycle()
                            : periph.next_wake_cycle();
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
            // HLT leaves all clocks running → any peripheral can wake.
            // SLP stops OSC3 → only RTC (OSC1) and KEY input can wake.
            // KEY input arrives via poll_buttons(), which we guarantee
            // to run at least every EVENT_INTERVAL cycles by clamping
            // the jump target with next_event_poll.
            bool sleep = (cpu.state.halt_mode
                          == CpuState::HaltMode::Slp);
            uint64_t wake = sleep ? periph.sleep_wake_cycle()
                                  : periph.next_wake_cycle();
            if (wake == UINT64_MAX && !sleep) {
                std::fprintf(stderr,
                    "Deadlock: CPU halted with no wakeup after %llu cycles\n",
                    (unsigned long long)total_cycles);
                break;
            }
            // In SLEEP, wake may be UINT64_MAX when RTC is idle — then
            // only a button press can wake.  Clamp to next_event_poll.
            uint64_t target = std::min({wake, next_render,
                                        next_event_poll});
            total_cycles = target;
            do_tick(); // time jump: process all timer events up to new time
            if (total_cycles >= next_event_poll) {
                poll_buttons();
                next_event_poll = total_cycles + EVENT_INTERVAL;
            }
        }

        if (total_cycles >= next_render) {
            sync_realtime();
            next_render += periph.clk.cpu_clock_hz() / 60;
        }
    }

    periph.clk.on_clock_change = nullptr; // remove dangling lambda refs
    quit_flag.store(true, std::memory_order_relaxed); // wake main thread
}
