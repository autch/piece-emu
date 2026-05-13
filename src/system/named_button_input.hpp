#pragma once
#include "button_input.hpp"

#include <string_view>

// ---------------------------------------------------------------------------
// apply_named_button — SDL-free button helper for script-driven frontends.
//
// Accepts the P/ECE button names UP / DOWN / LEFT / RIGHT / A / B / START /
// SELECT (case-insensitive) and toggles the matching bit in ButtonState.
// Returns false for unknown names; the caller is expected to reject the
// script up front in that case.
//
// Lives in its own TU because button_input.cpp pulls in <SDL3/SDL.h> for the
// SDL_Scancode / SDL_GamepadButton branches.  Sharing only the ButtonState
// definition keeps this helper SDL-independent.
// ---------------------------------------------------------------------------
bool apply_named_button(std::string_view name, bool is_down, ButtonState& btn);
