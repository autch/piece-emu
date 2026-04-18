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
    uint16_t                 gdb_port   = 0;
    bool                     gdb_debug  = false;
    bool                     no_audio   = false;
    bool                     audio_trace = false;
    std::string              audio_log_path;
    std::vector<std::string> wp_write_specs, wp_read_specs, wp_rw_specs;
    std::vector<std::string> break_specs;

    static Config parse(int argc, char** argv);
};
