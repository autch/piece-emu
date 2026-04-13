#include "semihosting.hpp"
#include "bus.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>

// ============================================================================
// Semihosting debug port at 0x060000
// ============================================================================

static constexpr uint32_t SEMI_BASE = 0x060000;

// Internal state captured by I/O lambdas.
struct SemiState {
    Bus*  bus = nullptr;
    std::function<uint64_t()>     get_cycles;
    std::function<void(bool)>     set_trace;
    std::function<void()>         halt;
    std::function<void()>         snapshot_regs;
    std::function<void(uint32_t)> set_breakpoint;
    std::function<void(uint32_t)> clear_breakpoint;

    uint16_t str_addr_lo  = 0;   // latched low 16 bits of CONSOLE_STR address
    uint16_t bkpt_set_lo  = 0;   // latched low 16 bits of BKPT_SET address
    uint16_t bkpt_clr_lo  = 0;   // latched low 16 bits of BKPT_CLR address
    uint64_t latched_cycles = 0; // snapshot taken at first 16-bit half of CYCLE_COUNT read
    uint64_t latched_time   = 0; // snapshot taken at first 16-bit half of HOST_TIME_MS read
    int  test_result = -1;       // -1 = not set, 0 = PASS, else FAIL code
};

static SemiState* g_semi = nullptr; // singleton for lambda capture

void semihosting_init(Bus& bus, SemiConfig cfg) {
    static SemiState state;
    state                  = SemiState{};
    state.bus              = &bus;
    state.get_cycles       = std::move(cfg.get_cycles);
    state.set_trace        = std::move(cfg.set_trace);
    state.halt             = std::move(cfg.halt);
    state.snapshot_regs    = std::move(cfg.snapshot_regs);
    state.set_breakpoint   = std::move(cfg.set_breakpoint);
    state.clear_breakpoint = std::move(cfg.clear_breakpoint);
    g_semi = &state;

    // -----------------------------------------------------------------------
    // +0x00  CONSOLE_CHAR (W): low byte → stdout
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x00, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            std::putchar(static_cast<char>(val & 0xFF));
            std::fflush(stdout);
        }
    });

    // -----------------------------------------------------------------------
    // +0x02  CONSOLE_STR lo (W): latch low 16 bits of string address
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x02, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            if (g_semi) g_semi->str_addr_lo = val;
        }
    });

    // -----------------------------------------------------------------------
    // +0x04  CONSOLE_STR hi (W): high 16 bits; combine with lo and print string
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x04, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            if (!g_semi) return;
            uint32_t addr = (static_cast<uint32_t>(val) << 16) | g_semi->str_addr_lo;
            Bus& b = *g_semi->bus;
            for (int i = 0; i < 4096; i++) {
                char c = static_cast<char>(b.read8(addr + i));
                if (c == '\0') break;
                std::putchar(c);
            }
            std::fflush(stdout);
        }
    });

    // -----------------------------------------------------------------------
    // +0x06  (unused pad — register as no-op to absorb stray writes)
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x06, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t) {}
    });

    // -----------------------------------------------------------------------
    // +0x08  TEST_RESULT lo (W): 0=PASS, non-zero=FAIL; halts the emulator
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x08, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            if (!g_semi) return;
            g_semi->test_result = static_cast<int>(val);
            if (g_semi->halt) g_semi->halt();
            if (val == 0)
                std::fprintf(stderr, "\n[PASS]\n");
            else
                std::fprintf(stderr, "\n[FAIL] code=%u\n", unsigned(val));
        }
    });

    // +0x0A  TEST_RESULT hi (W): high word of 32-bit TEST_RESULT write; ignored
    bus.register_io(SEMI_BASE + 0x0A, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t) {}
    });

    // -----------------------------------------------------------------------
    // +0x0C  CYCLE_COUNT_LO (R): lower 32 bits of total cycle count
    //         Two 16-bit reads: +0x0C=bits[15:0], +0x0E=bits[31:16]
    //         The first read latches the full 64-bit value for consistency.
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x0C, {
        [](uint32_t) -> uint16_t {
            if (!g_semi) return 0;
            // Latch cycle count on low-word read for a consistent 64-bit snapshot.
            g_semi->latched_cycles = g_semi->get_cycles
                ? g_semi->get_cycles() : 0;
            return static_cast<uint16_t>(g_semi->latched_cycles & 0xFFFF);
        },
        [](uint32_t, uint16_t) {}
    });
    bus.register_io(SEMI_BASE + 0x0E, {
        [](uint32_t) -> uint16_t {
            if (!g_semi) return 0;
            return static_cast<uint16_t>((g_semi->latched_cycles >> 16) & 0xFFFF);
        },
        [](uint32_t, uint16_t) {}
    });

    // -----------------------------------------------------------------------
    // +0x10  CYCLE_COUNT_HI (R): upper 32 bits of total cycle count
    //         +0x10=bits[47:32], +0x12=bits[63:48]
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x10, {
        [](uint32_t) -> uint16_t {
            if (!g_semi) return 0;
            return static_cast<uint16_t>((g_semi->latched_cycles >> 32) & 0xFFFF);
        },
        [](uint32_t, uint16_t) {}
    });
    bus.register_io(SEMI_BASE + 0x12, {
        [](uint32_t) -> uint16_t {
            if (!g_semi) return 0;
            return static_cast<uint16_t>((g_semi->latched_cycles >> 48) & 0xFFFF);
        },
        [](uint32_t, uint16_t) {}
    });

    // -----------------------------------------------------------------------
    // +0x14  REG_SNAPSHOT (W): dump all CPU registers to stderr
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x14, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t) {
            if (g_semi && g_semi->snapshot_regs) g_semi->snapshot_regs();
        }
    });
    // +0x16 high word pad
    bus.register_io(SEMI_BASE + 0x16, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t) {}
    });

    // -----------------------------------------------------------------------
    // +0x18  TRACE_CTL (W): 1=enable instruction trace, 0=disable
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x18, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            if (g_semi && g_semi->set_trace)
                g_semi->set_trace(val != 0);
        }
    });
    // +0x1A high word pad
    bus.register_io(SEMI_BASE + 0x1A, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t) {}
    });

    // -----------------------------------------------------------------------
    // +0x1C  BKPT_SET (W): 32-bit address; lo-word latched at +0x1C, hi-word triggers at +0x1E
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x1C, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            if (g_semi) g_semi->bkpt_set_lo = val;
        }
    });
    bus.register_io(SEMI_BASE + 0x1E, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            if (!g_semi) return;
            uint32_t addr = (static_cast<uint32_t>(val) << 16) | g_semi->bkpt_set_lo;
            if (g_semi->set_breakpoint) g_semi->set_breakpoint(addr);
            std::fprintf(stderr, "[BKPT] set breakpoint at 0x%06X\n", addr);
        }
    });

    // -----------------------------------------------------------------------
    // +0x20  BKPT_CLR (W): 32-bit address; lo-word latched at +0x20, hi-word triggers at +0x22
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x20, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            if (g_semi) g_semi->bkpt_clr_lo = val;
        }
    });
    bus.register_io(SEMI_BASE + 0x22, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            if (!g_semi) return;
            uint32_t addr = (static_cast<uint32_t>(val) << 16) | g_semi->bkpt_clr_lo;
            if (g_semi->clear_breakpoint) g_semi->clear_breakpoint(addr);
            std::fprintf(stderr, "[BKPT] cleared breakpoint at 0x%06X\n", addr);
        }
    });

    // -----------------------------------------------------------------------
    // +0x24  HOST_TIME_MS (R): host wall-clock milliseconds since some epoch
    //         Two 16-bit reads: +0x24=bits[15:0], +0x26=bits[31:16]
    //         The first read latches the value for a consistent pair.
    // -----------------------------------------------------------------------
    bus.register_io(SEMI_BASE + 0x24, {
        [](uint32_t) -> uint16_t {
            if (!g_semi) return 0;
            using namespace std::chrono;
            auto now_ms = static_cast<uint64_t>(
                duration_cast<milliseconds>(
                    steady_clock::now().time_since_epoch()).count());
            g_semi->latched_time = now_ms;
            return static_cast<uint16_t>(now_ms & 0xFFFF);
        },
        [](uint32_t, uint16_t) {}
    });
    bus.register_io(SEMI_BASE + 0x26, {
        [](uint32_t) -> uint16_t {
            if (!g_semi) return 0;
            return static_cast<uint16_t>((g_semi->latched_time >> 16) & 0xFFFF);
        },
        [](uint32_t, uint16_t) {}
    });
}

int semihosting_test_result() {
    return g_semi ? g_semi->test_result : -1;
}
