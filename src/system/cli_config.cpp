#include "cli_config.hpp"

#include <CLI/CLI.hpp>
#include <cstdlib>

Config Config::parse(int argc, char** argv)
{
    Config cfg;
    CLI::App app{"P/ECE system emulator (SDL3 display)"};
    argv = app.ensure_utf8(argv);

    app.add_option("--pfi", cfg.pfi_path, "P/ECE Flash Image (.pfi) to load")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("--max-cycles", cfg.max_cycles,
        "Stop after N cycles (default: unlimited)");
    app.add_flag("--trace", cfg.trace,
        "Print disassembly for each instruction");
    app.add_option("--scale", cfg.scale,
        "Display scale factor (default: 4 → 512×352)")
        ->check(CLI::Range(1, 8));
    auto* opt_sram  = app.add_option("--sram-size",  cfg.sram_size,
        "SRAM size in bytes (default: from PFI SYSTEMINFO)");
    auto* opt_flash = app.add_option("--flash-size", cfg.flash_size,
        "Flash size in bytes (default: from PFI SYSTEMINFO)");
    app.add_option("--gdb-port", cfg.gdb_port,
        "Start GDB RSP server on this TCP port (e.g. 1234); 0 = disabled");
    app.add_flag("--gdb-debug", cfg.gdb_debug,
        "Print GDB RSP packet traffic to stderr");
    app.add_flag("--no-audio", cfg.no_audio,
        "Disable SDL3 audio output (default: enabled)");
    app.add_flag("--swap-ab", cfg.swap_ab,
        "Swap gamepad A/B to match P/ECE physical layout (right=A, left=B)");
    app.add_flag("--audio-trace", cfg.audio_trace,
        "Print sound subsystem trace events to stderr");
    app.add_option("--audio-log", cfg.audio_log_path,
        "Write audio PUSH/PULL/PACE events to FILE (TSV)");
    app.add_option("--snapshot-path", cfg.snapshot_path,
        "Directory to save F12 screenshots into (default: current dir)");
    app.add_option("--wp-write", cfg.wp_write_specs,
        "Write watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
    app.add_option("--wp-read",  cfg.wp_read_specs,
        "Read watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
    app.add_option("--wp-rw",    cfg.wp_rw_specs,
        "Read/write watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
    app.add_option("--break-at", cfg.break_specs,
        "Dump registers when PC == ADDR (hex, repeatable)");

    try { app.parse(argc, argv); }
    catch (const CLI::ParseError& e) { std::exit(app.exit(e)); }

    cfg.sram_size_explicit  = opt_sram->count()  > 0;
    cfg.flash_size_explicit = opt_flash->count() > 0;
    return cfg;
}
