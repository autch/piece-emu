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

// Apply an SDL gamepad button event.  `gp_button` is an SDL_GamepadButton.
// If `swap_ab` is true, map to P/ECE physical layout (right=A, left=B)
// instead of the default Xbox-label mapping.
void handle_gamepad_button(bool is_down, int gp_button, ButtonState& btn,
                           bool swap_ab = false);

// Apply an SDL gamepad axis event (left stick / triggers → D-pad via
// threshold).  `axis` is an SDL_GamepadAxis; `value` is the raw int16 axis
// value from SDL_EVENT_GAMEPAD_AXIS_MOTION.
void handle_gamepad_axis(int axis, int value, ButtonState& btn);
