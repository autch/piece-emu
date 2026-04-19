#include "screenshot.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_MSC_SECURE_CRT
#include "stb_image_write.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace {

// Standard P/ECE LCD palette (0=white, 3=black; matches LcdRenderer).
constexpr uint8_t GRAY_PALETTE[4] = { 0xFF, 0xAA, 0x55, 0x00 };

std::string build_filename(const std::string& dir)
{
    using clock = std::chrono::system_clock;
    auto now   = clock::now();
    auto secs  = std::chrono::time_point_cast<std::chrono::seconds>(now);
    auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - secs).count();
    std::time_t t = clock::to_time_t(secs);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char name[64];
    std::snprintf(name, sizeof(name),
                  "piece_%04d%02d%02d_%02d%02d%02d_%03lld.png",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(ms));
    std::filesystem::path p = dir.empty()
        ? std::filesystem::path{name}
        : std::filesystem::path{dir} / name;
    return p.string();
}

} // namespace

std::string save_screenshot_png(const std::string& dir,
                                const uint8_t pixels[88][128])
{
    std::string path = build_filename(dir);

    uint8_t gray[88 * 128];
    for (int y = 0; y < 88; y++)
        for (int x = 0; x < 128; x++)
            gray[y * 128 + x] = GRAY_PALETTE[pixels[y][x] & 3u];

    // Speed-priority compression (see header comment for rationale).
    stbi_write_png_compression_level = 1;

    int ok = stbi_write_png(path.c_str(), 128, 88, 1, gray, 128);
    if (!ok) {
        std::fprintf(stderr, "[SNAP] stbi_write_png failed: %s\n", path.c_str());
        return {};
    }
    std::fprintf(stderr, "[SNAP] saved: %s\n", path.c_str());
    return path;
}
