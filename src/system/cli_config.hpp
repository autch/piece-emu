#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Config — parsed CLI options for piece-emu-system.
//
// Construct via `Config::parse(argc, argv)`; this internally builds a
// CLI::App, parses, and exits via CLI11's standard failure path if parsing
// fails (mirrors the previous inline behaviour).
// ---------------------------------------------------------------------------
struct Config {
    std::string              pfi_path;
    bool                     trace      = false;
    uint64_t                 max_cycles = 0;
    std::size_t              sram_size  = 0x040000; // 256 KB (default; overridden from PFI)
    std::size_t              flash_size = 0x080000; // 512 KB (default; overridden from PFI)
    bool                     sram_size_explicit  = false;
    bool                     flash_size_explicit = false;
    int                      scale      = 4;
    // Texture scaling filter for the 128x88 LCD texture.
    // "nearest"  - SDL_SCALEMODE_NEAREST (default, pixel-sharp)
    // "linear"   - SDL_SCALEMODE_LINEAR (bilinear)
    // "pixelart" - SDL_SCALEMODE_PIXELART (SDL 3.4.0+, sharp with anti-alias)
    std::string              scale_mode = "nearest";
    uint16_t                 gdb_port   = 0;
    bool                     gdb_debug  = false;
    bool                     no_audio   = false;
    // Gamepad face-button layout.  Default (false) follows Xbox labels:
    // SOUTH=A, EAST=B.  With --swap-ab (true) the mapping follows the
    // P/ECE physical layout (Nintendo style: right face button = A,
    // left = B): EAST=A, WEST/SOUTH=B.
    bool                     swap_ab    = false;
    bool                     audio_trace = false;
    std::string              audio_log_path;
    std::string              snapshot_path = "."; // directory for PNG screenshots
    std::vector<std::string> wp_write_specs, wp_read_specs, wp_rw_specs;
    std::vector<std::string> break_specs;

    static Config parse(int argc, char** argv);
};
