#include "lcd_renderer.hpp"
#include <SDL3/SDL.h>
#include <cstdio>

// 2bpp palette: index 0=white, 1=light gray, 2=dark gray, 3=black
// Format: ARGB8888
static constexpr uint32_t PALETTE[4] = {
    0xFFFFFFFFu,
    0xFFAAAAAAu,
    0xFF555555u,
    0xFF000000u,
};

LcdRenderer::~LcdRenderer()
{
    destroy();
}

bool LcdRenderer::init(int scale)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow("P/ECE", 128 * scale, 88 * scale, 0);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    // Let SDL3 auto-select the renderer.  Override with the SDL3 standard
    // environment variable `SDL_RENDER_DRIVER=<name>` if a particular host
    // needs a specific backend (e.g. `software` for Xrdp/VNC).
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    std::fprintf(stderr, "SDL3 renderer: %s\n", SDL_GetRendererName(renderer_));

    // Raise and focus the window so keyboard events are delivered on remote
    // desktop environments (Xrdp, VNC) where the window may not get focus
    // automatically.
    SDL_RaiseWindow(window_);

    scale_ = scale;
    texture_ = SDL_CreateTexture(renderer_,
                                  SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  128, 88);
    if (!texture_) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }

    render_buf_.resize(88 * 128);
    return true;
}

void LcdRenderer::render(const uint8_t pixels[88][128])
{
    // Convert 2bpp pixel array to ARGB8888 in the heap-allocated staging buffer.
    uint32_t* buf = render_buf_.data();
    for (int y = 0; y < 88; y++)
        for (int x = 0; x < 128; x++)
            buf[y * 128 + x] = PALETTE[pixels[y][x] & 3u];

    SDL_UpdateTexture(texture_, nullptr, buf, 128 * sizeof(uint32_t));
    has_frame_ = true;
    present_last();
}

void LcdRenderer::present_last()
{
    if (!renderer_ || !texture_) return;
    SDL_RenderClear(renderer_);
    if (has_frame_) {
        SDL_FRect dst = {0.0f, 0.0f,
                         static_cast<float>(128 * scale_),
                         static_cast<float>(88  * scale_)};
        SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
    }
    SDL_RenderPresent(renderer_);
}

bool LcdRenderer::poll_events(const KeyCb& key_cb,
                              const PadButtonCb& pad_btn_cb,
                              const PadAxisCb& pad_axis_cb)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break; // ignore OS key-repeat: kernel detects long press by held state
            [[fallthrough]];
        case SDL_EVENT_KEY_UP:
            std::fprintf(stderr, "[KEY] %s sc=%d\n",
                e.type == SDL_EVENT_KEY_DOWN ? "DN" : "UP",
                static_cast<int>(e.key.scancode));
            if (key_cb)
                key_cb(e.type == SDL_EVENT_KEY_DOWN,
                       static_cast<int>(e.key.scancode));
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            // Open the first available gamepad; ignore subsequent ones.
            if (!gamepad_) {
                gamepad_ = SDL_OpenGamepad(e.gdevice.which);
                if (gamepad_) {
                    std::fprintf(stderr, "[PAD] connected: %s (id=%u)\n",
                        SDL_GetGamepadName(gamepad_),
                        static_cast<unsigned>(e.gdevice.which));
                } else {
                    std::fprintf(stderr, "[PAD] SDL_OpenGamepad failed: %s\n",
                                 SDL_GetError());
                }
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad_ &&
                SDL_GetGamepadID(gamepad_) == e.gdevice.which) {
                std::fprintf(stderr, "[PAD] disconnected (id=%u)\n",
                    static_cast<unsigned>(e.gdevice.which));
                SDL_CloseGamepad(gamepad_);
                gamepad_ = nullptr;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (pad_btn_cb)
                pad_btn_cb(e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN,
                           static_cast<int>(e.gbutton.button));
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (pad_axis_cb)
                pad_axis_cb(static_cast<int>(e.gaxis.axis),
                            static_cast<int>(e.gaxis.value));
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_RESTORED:
            // Remote desktop (Xrdp/VNC) may not preserve the back buffer
            // when the window is re-exposed. Re-present the last frame so
            // occluded regions repaint without waiting for the next CPU
            // LCD transfer.
            present_last();
            break;
        default:
            break;
        }
    }
    return true;
}

void LcdRenderer::destroy()
{
    if (gamepad_)  { SDL_CloseGamepad(gamepad_);     gamepad_  = nullptr; }
    if (texture_)  { SDL_DestroyTexture(texture_);   texture_  = nullptr; }
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_   = nullptr; }
    SDL_Quit();
}
