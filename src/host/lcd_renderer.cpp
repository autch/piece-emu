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
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow("P/ECE", 128 * scale, 88 * scale, 0);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    // Prefer software renderer for broad compatibility (e.g. Xrdp, VNC).
    // SDL3 may pick OpenGL/Vulkan by default which doesn't work on all remote
    // desktop setups.  The software renderer is always available.
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");

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

    return true;
}

void LcdRenderer::render(const uint8_t pixels[88][128])
{
    // Convert 2bpp pixel array to ARGB8888.
    uint32_t buf[88 * 128];
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

bool LcdRenderer::poll_events(const KeyCb& key_cb)
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
    if (texture_)  { SDL_DestroyTexture(texture_);   texture_  = nullptr; }
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_   = nullptr; }
    SDL_Quit();
}
