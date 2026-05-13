#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// HeadlessConfig — CLI options for piece-emu-headless-system.
//
// Intentionally separate from src/system/cli_config.hpp (which is SDL-aware
// and tied to piece-emu-system).  This struct drops the display/audio
// fields the headless frontend cannot use and adds the script / hash /
// determinism options the regression workflow needs.
// ---------------------------------------------------------------------------
struct HeadlessConfig {
    std::string              pfi_path;
    std::string              script_path;

    std::size_t              sram_size  = 0x040000; // 256 KB (default; overridden from PFI)
    std::size_t              flash_size = 0x080000; // 512 KB (default; overridden from PFI)
    bool                     sram_size_explicit  = false;
    bool                     flash_size_explicit = false;

    // Stop conditions
    uint64_t                 max_frames = 7200;     // ~2 minutes at 60 Hz
    uint64_t                 max_cycles = 0;        // 0 = unlimited
    // Wall-clock guard: bound the real-time runtime of this process so a
    // forgotten 'quit' or an app that stops refreshing the LCD never
    // produces a runaway CPU-burning process.  0 = no guard.
    int                      wall_timeout_sec = 60;

    // Determinism
    std::string              rtc_fixed  = "2026-01-01T00:00:00";

    // Output
    uint64_t                 hash_every    = 1;
    std::string              snapshot_dir  = ".";

    // Optional GDB RSP (disables determinism guarantee)
    uint16_t                 gdb_port   = 0;
    bool                     gdb_debug  = false;

    // Debug-trace passthrough (same semantics as piece-emu / piece-emu-system)
    bool                     trace      = false;
    std::vector<std::string> wp_write_specs, wp_read_specs, wp_rw_specs;
    std::vector<std::string> break_specs;

    static HeadlessConfig parse(int argc, char** argv);
};
