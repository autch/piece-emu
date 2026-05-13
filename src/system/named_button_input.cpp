#include "named_button_input.hpp"

#include <cctype>
#include <string>

namespace {

// Case-insensitive ASCII match.  Button names are pure ASCII.
bool ieq(std::string_view a, const char* b)
{
    std::size_t n = 0;
    while (b[n] != '\0') ++n;
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; i++) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::toupper(ca) != std::toupper(cb)) return false;
    }
    return true;
}

void press(uint8_t& reg, int bit, bool is_down)
{
    if (is_down) reg &= ~(1u << bit);
    else         reg |=  (1u << bit);
}

} // namespace

bool apply_named_button(std::string_view name, bool is_down, ButtonState& btn)
{
    // Same bit assignments as button_input.cpp:handle_key.
    if (ieq(name, "RIGHT"))  { press(btn.k6, 0, is_down); return true; }
    if (ieq(name, "LEFT"))   { press(btn.k6, 1, is_down); return true; }
    if (ieq(name, "DOWN"))   { press(btn.k6, 2, is_down); return true; }
    if (ieq(name, "UP"))     { press(btn.k6, 3, is_down); return true; }
    if (ieq(name, "B"))      { press(btn.k6, 4, is_down); return true; }
    if (ieq(name, "A"))      { press(btn.k6, 5, is_down); return true; }
    if (ieq(name, "START"))  { press(btn.k5, 4, is_down); return true; }
    if (ieq(name, "SELECT")) { press(btn.k5, 3, is_down); return true; }
    return false;
}
