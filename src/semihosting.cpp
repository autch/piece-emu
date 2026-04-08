#include "semihosting.hpp"
#include "bus.hpp"
#include "cpu.hpp"

#include <cstdio>
#include <cstring>

// ============================================================================
// Semihosting port at 0x060000
//
// Offsets from 0x060000:
//   +0x00  CONSOLE_CHAR  write: emit low byte to stdout
//   +0x02  CONSOLE_STR   write16 of address low word; pair with +0x04 for high word
//   +0x08  TEST_RESULT   write: 0=PASS halt, non-zero=FAIL halt
// ============================================================================

static constexpr uint32_t SEMI_BASE         = 0x060000;
static constexpr uint32_t SEMI_CONSOLE_CHAR = SEMI_BASE + 0x00;
static constexpr uint32_t SEMI_CONSOLE_STR  = SEMI_BASE + 0x02; // write 32-bit addr here
static constexpr uint32_t SEMI_TEST_RESULT  = SEMI_BASE + 0x08;

// We use a small state object captured in lambdas.
struct SemiState {
    Bus* bus;
    Cpu* cpu;
    uint32_t str_addr_buf = 0; // latches low 16 bits of string address
    int test_result = -1;      // -1=not set, 0=pass, else fail code
};

static SemiState* g_semi = nullptr; // simple singleton for now

void semihosting_init(Bus& bus, Cpu& cpu) {
    static SemiState state;
    state.bus = &bus;
    state.cpu = &cpu;
    g_semi = &state;

    // CONSOLE_CHAR: write a character
    bus.register_io(SEMI_CONSOLE_CHAR, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            char c = static_cast<char>(val & 0xFF);
            std::putchar(c);
            std::fflush(stdout);
        }
    });

    // CONSOLE_STR: write 32-bit address (as two 16-bit writes) then print string
    // Simple approach: write to +0x02 sets low 16, write to +0x04 sets high 16 and triggers
    // For P0: treat a write to +0x02 as a full 16-bit address (strings in low 64KB)
    bus.register_io(SEMI_CONSOLE_STR, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            if (!g_semi) return;
            uint32_t addr = val; // low 16 bits = address (sufficient for IRAM/SRAM)
            Bus& b = *g_semi->bus;
            for (int i = 0; i < 4096; i++) {
                char c = static_cast<char>(b.read8(addr + i));
                if (c == '\0') break;
                std::putchar(c);
            }
            std::fflush(stdout);
        }
    });

    // TEST_RESULT: write 0 = PASS, non-zero = FAIL
    bus.register_io(SEMI_TEST_RESULT, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t val) {
            if (!g_semi) return;
            g_semi->test_result = val;
            g_semi->cpu->state.in_halt = true;
            if (val == 0) {
                std::fprintf(stderr, "\n[PASS]\n");
            } else {
                std::fprintf(stderr, "\n[FAIL] code=%u\n", unsigned(val));
            }
        }
    });
}

int semihosting_test_result() {
    return g_semi ? g_semi->test_result : -1;
}
