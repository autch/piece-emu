#include "button_input.hpp"

#include <SDL3/SDL.h>
#include <cstdio>

void handle_key(bool is_down, int scancode, ButtonState& btn)
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

void handle_gamepad_button(bool is_down, int gp_button, ButtonState& btn,
                           bool swap_ab)
{
    auto press = [is_down](uint8_t& reg, int bit) {
        if (is_down) reg &= ~(1u << bit);
        else         reg |=  (1u << bit);
    };
    // SDL3 normalises face buttons by POSITION (not label):
    //   SOUTH=bottom, EAST=right, WEST=left, NORTH=top.
    //
    // Default (swap_ab=false): Xbox-label mapping.  Xbox A(SOUTH)→P/ECE A,
    //   Xbox B(EAST)→P/ECE B.  West/North as extra A/B alternates.
    // swap_ab=true: physical-position mapping matching the P/ECE hardware
    //   layout (right face = A, left face = B).  Same as a Nintendo
    //   label-based mapping because Nintendo A is the right button.
    const int a_bit = 5; // K65 = A
    const int b_bit = 4; // K64 = B
    uint8_t old_k6 = btn.k6, old_k5 = btn.k5;
    switch (static_cast<SDL_GamepadButton>(gp_button)) {
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: press(btn.k6, 0); break; // K60
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  press(btn.k6, 1); break; // K61
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  press(btn.k6, 2); break; // K62
    case SDL_GAMEPAD_BUTTON_DPAD_UP:    press(btn.k6, 3); break; // K63
    case SDL_GAMEPAD_BUTTON_SOUTH:
        press(btn.k6, swap_ab ? b_bit : a_bit); break;
    case SDL_GAMEPAD_BUTTON_EAST:
        press(btn.k6, swap_ab ? a_bit : b_bit); break;
    case SDL_GAMEPAD_BUTTON_WEST:
        press(btn.k6, swap_ab ? b_bit : b_bit); break; // always B
    case SDL_GAMEPAD_BUTTON_NORTH:
        press(btn.k6, swap_ab ? a_bit : a_bit); break; // always A
    case SDL_GAMEPAD_BUTTON_START:      press(btn.k5, 4); break; // K54 = START
    case SDL_GAMEPAD_BUTTON_BACK:       press(btn.k5, 3); break; // K53 = SELECT
    default: break;
    }
    if (btn.k5 != old_k5 || btn.k6 != old_k6)
        std::fprintf(stderr, "[PAD] k5=0x%02X k6=0x%02X\n", btn.k5, btn.k6);
}

void handle_gamepad_axis(int axis, int value, ButtonState& btn)
{
    // Convert analog axis to D-pad via hysteresis thresholds.  Use a single
    // threshold (no hysteresis) — kernel tolerates brief chatter because
    // scan rate is 100 Hz and it debounces internally.
    constexpr int TH = 16000; // ~50% of int16 range
    uint8_t old_k6 = btn.k6;
    auto set = [&btn](int bit, bool pressed) {
        if (pressed) btn.k6 &= ~(1u << bit);
        else         btn.k6 |=  (1u << bit);
    };
    switch (static_cast<SDL_GamepadAxis>(axis)) {
    case SDL_GAMEPAD_AXIS_LEFTX:
        set(0, value >  TH); // K60 right
        set(1, value < -TH); // K61 left
        break;
    case SDL_GAMEPAD_AXIS_LEFTY:
        set(2, value >  TH); // K62 down
        set(3, value < -TH); // K63 up
        break;
    default: break;
    }
    if (btn.k6 != old_k6)
        std::fprintf(stderr, "[PAD] k5=0x%02X k6=0x%02X\n", btn.k5, btn.k6);
}
