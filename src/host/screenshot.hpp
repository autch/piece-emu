#pragma once
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// Screenshot — save S6B0741 LCD framebuffer as PNG.
//
// save_png_grayscale():
//   Writes an 8-bit grayscale PNG.  The 2bpp pixel array is expanded with the
//   standard LCD palette (0=white, 1=light gray, 2=dark gray, 3=black) before
//   compression.  Uses stb_image_write with compression level 1 (speed over
//   size): the 128×88 image is ~11 KB raw and compresses in well under a
//   millisecond, so the choice is largely cosmetic.
//
// File name format: piece_YYYYMMDD_HHMMSS_mmm.png
// Returns the saved path on success, empty string on failure.
// ---------------------------------------------------------------------------
std::string save_screenshot_png(const std::string& dir,
                                const uint8_t pixels[88][128]);
