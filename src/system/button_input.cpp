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
