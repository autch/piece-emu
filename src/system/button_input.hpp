#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Button → K5D/K6D bit mapping (active-low)
//
// K5D bits:
//   bit3 = K53 = SELECT
//   bit4 = K54 = START
//
// K6D bits:
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

// Apply an SDL key event to the button state.  is_down=true for KEY_DOWN,
// false for KEY_UP.  `scancode` is an SDL_Scancode (passed as int to keep
// the header SDL-agnostic; the .cpp translates).
void handle_key(bool is_down, int scancode, ButtonState& btn);
