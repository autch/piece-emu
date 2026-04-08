// test_basic.c — basic arithmetic and memory test for piece_emu.
//
// Tests: integer arithmetic, function calls, global variables, array access.
// Returns 0 on success (all assertions pass), non-zero on first failure.

#include <stdint.h>

// ---- simple arithmetic ------------------------------------------------------

static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }

// ---- global variable --------------------------------------------------------

static int g_counter = 0;
static void increment(void) { g_counter++; }

// ---- array ------------------------------------------------------------------

static int array[8];

static void fill_array(void) {
    for (int i = 0; i < 8; i++)
        array[i] = i * i;
}

// ---- main -------------------------------------------------------------------

int main(void) {
    // Arithmetic
    if (add(2, 3) != 5) return 1;
    if (add(-1, 1) != 0) return 2;
    if (mul(6, 7) != 42) return 3;
    if (mul(-3, 4) != -12) return 4;

    // Global variable
    g_counter = 0;
    increment();
    increment();
    increment();
    if (g_counter != 3) return 5;

    // Array
    fill_array();
    if (array[0] != 0) return 6;
    if (array[3] != 9) return 7;
    if (array[7] != 49) return 8;

    // Bit operations
    uint32_t v = 0xA5A5A5A5u;
    if ((v & 0xFF) != 0xA5) return 9;
    if ((v >> 16) != 0xA5A5) return 10;
    if ((v << 4) != 0x5A5A5A50u) return 11;

    return 0; // PASS
}
