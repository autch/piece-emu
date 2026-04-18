#pragma once
#include <cstdint>
#include <functional>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

// ============================================================================
// LcdRenderer — SDL3-based display for the S6B0741 LCD (128×88 pixels)
//
// Usage:
//   LcdRenderer renderer;
//   renderer.init(4);                   // 4× scale → 512×352 window
//   renderer.render(pixels);            // pixels[88][128], 2bpp gray
//   bool ok = renderer.poll_events(...) // returns false on quit
//   renderer.destroy();
//
// Pixel encoding: 0=white, 1=light gray, 2=dark gray, 3=black
// (matches piemu's palette and to_pixels() output from S6b0741).
// ============================================================================

class LcdRenderer {
public:
    ~LcdRenderer();

    // Create SDL3 window and renderer.  scale multiplies the 128×88 pixel
    // resolution (default 4 → 512×352 window).  Returns true on success.
    bool init(int scale = 4);

    // Blit pixels[88][128] (2bpp) to the SDL3 window.
    void render(const uint8_t pixels[88][128]);

    // Re-present the last rendered texture. Called on window expose events
    // so remote-desktop setups (Xrdp/VNC) repaint occluded regions even when
    // the CPU has not pushed a new LCD frame.
    void present_last();

    // Key event callback type: (is_down, scancode).
    using KeyCb = std::function<void(bool is_down, int scancode)>;

    // Poll SDL3 events.  Calls key_cb for key events.
    // Returns false when the user closes the window (SDL_EVENT_QUIT).
    bool poll_events(const KeyCb& key_cb);

    // Release SDL3 resources.  Safe to call even if init() was not called.
    void destroy();

private:
    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;
    int           scale_    = 4;
    bool          has_frame_ = false;
};
