#pragma once
#include "s6b0741.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------------------
// LcdFrameBuf — shared pixel buffer between CPU thread and main (SDL) thread.
//
// CPU thread calls push() on HSDMA Ch0 completion.
// Main thread calls take() at its own ~60 Hz cadence and passes pixels to
// LcdRenderer::render(), which must only be called from the main thread.
//
// If multiple frames arrive before take(), the latest wins (frame drop).
// ---------------------------------------------------------------------------
struct LcdFrameBuf {
    std::mutex mtx;
    uint8_t    pixels[88][128] = {};
    bool       pending = false;

    // Called from CPU thread: convert LCD VRAM directly into the shared
    // buffer under the mutex.  Taking the LCD by reference avoids putting
    // an 11 KB intermediate pixel array on the CPU thread's stack (Windows
    // default thread stack is 1 MB).
    void push(const S6b0741& lcd) {
        std::lock_guard<std::mutex> lk(mtx);
        lcd.to_pixels(pixels);
        pending = true;
    }

    // Called from main thread: returns true and copies out if a new frame is
    // available.
    bool take(uint8_t dst[88][128]) {
        std::lock_guard<std::mutex> lk(mtx);
        if (!pending) return false;
        std::memcpy(dst, pixels, sizeof(pixels));
        pending = false;
        return true;
    }
};
