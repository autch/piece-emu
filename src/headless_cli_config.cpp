#include "headless_cli_config.hpp"

#include <CLI/CLI.hpp>
#include <cstdlib>

HeadlessConfig HeadlessConfig::parse(int argc, char** argv)
{
    HeadlessConfig cfg;
    CLI::App app{"P/ECE headless full-system emulator "
                 "(script-driven, deterministic VRAM hashing)"};
    argv = app.ensure_utf8(argv);

    app.add_option("pfi", cfg.pfi_path,
        "P/ECE Flash Image (.pfi) to load (read-only)")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("--script", cfg.script_path,
        "Input / action script file")
        ->required()
        ->check(CLI::ExistingFile);

    auto* opt_sram  = app.add_option("--sram-size",  cfg.sram_size,
        "SRAM size in bytes (default: from PFI SYSTEMINFO)");
    auto* opt_flash = app.add_option("--flash-size", cfg.flash_size,
        "Flash size in bytes (default: from PFI SYSTEMINFO)");

    app.add_option("--max-frames", cfg.max_frames,
        "Hard stop after N LCD frames (default: 7200 ≈ 2 min @ 60 Hz)");
    app.add_option("--max-cycles", cfg.max_cycles,
        "Hard stop after N CPU cycles (default: unlimited)");
    app.add_option("--wall-timeout", cfg.wall_timeout_sec,
        "Hard stop after N real-time seconds — guards against a forgotten "
        "'quit' or an app that stops refreshing the LCD.  0 = no guard.  "
        "Default: 60.")
        ->check(CLI::NonNegativeNumber);

    app.add_option("--rtc-fixed", cfg.rtc_fixed,
        "Pin RTC peripheral to a fixed ISO-8601 timestamp "
        "(default: 2026-01-01T00:00:00)");

    app.add_option("--hash-every", cfg.hash_every,
        "Emit a hash line every N frames (default: 1)")
        ->check(CLI::PositiveNumber);
    app.add_option("--snapshot-dir", cfg.snapshot_dir,
        "Directory where 'snapshot' script commands write PNGs (default: .)");

    app.add_option("--gdb-port", cfg.gdb_port,
        "Start GDB RSP server on this TCP port (0 = disabled). "
        "Note: enabling GDB voids the determinism guarantee.");
    app.add_flag("--gdb-debug", cfg.gdb_debug,
        "Print GDB RSP packet traffic to stderr");

    app.add_flag("--trace", cfg.trace,
        "Print disassembly for each instruction");
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
