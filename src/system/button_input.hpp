#pragma once
#include <cstdint>
#include <span>

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

// ---------------------------------------------------------------------------
// Generic data-driven joystick button handler
// ---------------------------------------------------------------------------

// One entry in a joystick button map: which P/ECE register and bit to toggle.
// KReg::NONE means the button is unmapped (silently ignored).
enum class KReg : uint8_t { NONE, K5, K6 };
struct JoyBtnEntry {
    KReg    reg = KReg::NONE;
    uint8_t bit = 0;
};

// Apply a raw SDL joystick button event using the supplied mapping table.
// `js_button` is the SDL joystick button index; indices beyond map.size()
// are silently ignored.
void handle_joystick_button(bool is_down, int js_button, ButtonState& btn,
                            std::span<const JoyBtnEntry> map);

// ---------------------------------------------------------------------------
// Per-platform mapping tables
// ---------------------------------------------------------------------------

// ClockworkPi uConsole — build with -DPIECE_PLATFORM=uconsole.
// The uConsole device declares all BTN codes in 0x120…0x129 (including
// phantom ones), so SDL assigns sequential indices:
//   0 BTN_TRIGGER (X)                  → A (K65)
//   1 BTN_THUMB   (A)                  → A (K65)
//   2 BTN_THUMB2  (B)                  → B (K64)
//   3 BTN_TOP     (Y)                  → B (K64)
//   4–7 BTN_TOP2/PINKIE/BASE/BASE2     (phantom, unmapped)
//   8 BTN_BASE3   (SELECT)             → K53
//   9 BTN_BASE4   (START)              → K54
extern const JoyBtnEntry UCONSOLE_JOYBTN_MAP[10];
