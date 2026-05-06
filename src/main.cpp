#include "bus.hpp"
#include "cpu.hpp"
#include "debug_utils.hpp"
#include "diag.hpp"
#include "elf_loader.hpp"
#include "flash_sst39vf.hpp"
#include "pfi_loader.hpp"
#include "pfi_writeback.hpp"
#include "gdb_rsp.hpp"
#include "semihosting.hpp"
#include "peripheral_intc.hpp"
#include "peripheral_clkctl.hpp"
#include "peripheral_t8.hpp"
#include "peripheral_t16.hpp"
#include "peripheral_portctrl.hpp"
#include "peripheral_bcu_area.hpp"
#include "peripheral_wdt.hpp"
#include "peripheral_rtc.hpp"
#include "peripheral_hsdma.hpp"
#include "peripheral_sif3.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Signal-driven quit flag for SIGINT / SIGTERM (file scope so the handler
// can reach it).  Used in the headless main loop alongside semihosting's
// stop_requested.
namespace {
    std::atomic<bool>* g_quit_flag = nullptr;

    extern "C" void on_terminate_signal(int /*sig*/) {
        if (g_quit_flag) g_quit_flag->store(true, std::memory_order_relaxed);
    }

    uint64_t steady_now_us() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<microseconds>(
                steady_clock::now().time_since_epoch()).count());
    }
}

int main(int argc, char** argv) {
    CLI::App app{"P/ECE emulator (S1C33209)"};
    argv = app.ensure_utf8(argv);

    std::string              elf_path;
    std::string              pfi_path;
    bool                     use_gdb    = false;
    uint16_t                 gdb_port   = 1234;
    bool                     debug_rsp  = false;
    uint64_t                 max_cycles = 0;
    bool                     trace      = false;
    std::size_t              sram_size  = 0x040000; // 256 KB
    std::size_t              flash_size = 0x080000; // 512 KB
    std::vector<std::string> wp_write_specs, wp_read_specs, wp_rw_specs;
    std::vector<std::string> break_specs;
    bool                     enable_flash_writeback = false;
    bool                     read_only              = false;
    int                      writeback_debounce_ms  = 2000;

    // Exactly one of --pfi or elf must be provided.
    auto* elf_opt = app.add_option("elf", elf_path, "ELF binary to load and run")
        ->check(CLI::ExistingFile);
    auto* pfi_opt = app.add_option("--pfi", pfi_path, "P/ECE Flash Image (.pfi) to load")
        ->check(CLI::ExistingFile);
    elf_opt->excludes(pfi_opt);
    pfi_opt->excludes(elf_opt);

    auto* gdb_opt = app.add_flag("--gdb", use_gdb,
        "Wait for GDB/LLDB connection before running");
    app.add_option("--gdb-port", gdb_port,
        "GDB RSP listen port (default: 1234)")
        ->needs(gdb_opt);
    app.add_flag("--debug-rsp", debug_rsp,
        "Log RSP packets to stderr (requires --gdb)")
        ->needs(gdb_opt);

    app.add_option("--max-cycles", max_cycles,
        "Stop after N cycles (default: unlimited, 0 = unlimited)");
    app.add_flag("--trace", trace,
        "Print disassembly for each instruction executed");

    app.add_option("--sram-size", sram_size,
        "External SRAM size in bytes (default: 262144 = 256 KB)");
    app.add_option("--flash-size", flash_size,
        "External Flash size in bytes (default: 524288 = 512 KB; "
        "use 1048576 for 1 MB or 2097152 for 2 MB Flash-modded P/ECE)");
    app.add_option("--wp-write", wp_write_specs,
        "Write watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
    app.add_option("--wp-read",  wp_read_specs,
        "Read watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
    app.add_option("--wp-rw",    wp_rw_specs,
        "Read/write watchpoint: ADDR or ADDR:SIZE (hex, repeatable)");
    app.add_option("--break-at", break_specs,
        "Dump registers when PC == ADDR (hex, repeatable)");
    app.add_flag("--enable-flash-writeback", enable_flash_writeback,
        "Push kernel-driven flash mutations back to the host PFI file "
        "(headless default: OFF — preserves test idempotency)");
    app.add_flag("--read-only", read_only,
        "Force read-only flash even if --enable-flash-writeback is set");
    app.add_option("--writeback-debounce-ms", writeback_debounce_ms,
        "Idle interval (ms) before dirty flash sectors are flushed "
        "(default: 2000)")
        ->check(CLI::Range(0, 60000));

    CLI11_PARSE(app, argc, argv);

    try {
        Bus bus(sram_size, flash_size);
        Cpu cpu(bus);

        StderrDiagSink diag_sink;
        cpu.set_diag(&diag_sink);
        bus.set_diag(&diag_sink);

        // ---- Debug: watchpoints & break-at ----------------------------------
        for (auto& s : wp_write_specs) {
            auto [a, sz] = parse_addr_size(s);
            bus.add_watchpoint(a, sz, WpType::WRITE);
        }
        for (auto& s : wp_read_specs) {
            auto [a, sz] = parse_addr_size(s);
            bus.add_watchpoint(a, sz, WpType::READ);
        }
        for (auto& s : wp_rw_specs) {
            auto [a, sz] = parse_addr_size(s);
            bus.add_watchpoint(a, sz, WpType::RW);
        }
        bus.set_wp_callback(make_wp_callback(bus));

        std::set<uint32_t> break_addrs;
        for (auto& s : break_specs)
            break_addrs.insert(static_cast<uint32_t>(std::stoul(s, nullptr, 0)));
        // ---------------------------------------------------------------------

        // Declared early so semihosting lambdas can capture by reference.
        uint64_t total_cycles   = 0;
        // Set by TEST_RESULT when no debugger is attached: unconditional
        // emulator termination.  Bypasses the HLT→peripheral-wake path so
        // pending timers/RTC events cannot silently resume execution.
        bool     stop_requested = false;

        semihosting_init(bus, {
            .get_cycles       = [&]() -> uint64_t { return total_cycles; },
            .set_trace        = [&](bool on) { trace = on; },
            .halt             = [&]() {
                cpu.state.in_halt = true;
                // When a debugger is attached we only halt (so the user can
                // inspect state and resume); otherwise request termination.
                if (!use_gdb) stop_requested = true;
            },
            .snapshot_regs    = [&]() { print_reg_snapshot(cpu.state); },
            .set_breakpoint   = [&](uint32_t addr) { cpu.breakpoints.insert(addr); },
            .clear_breakpoint = [&](uint32_t addr) { cpu.breakpoints.erase(addr); },
        });

        // Peripheral initialisation
        InterruptController intc;
        intc.attach(bus, [&cpu](int trap_no, int level) {
            cpu.assert_trap(trap_no, level);
        });

        ClockControl clk;
        clk.attach(bus);

        Timer8bit  t8_ch[4]  = {Timer8bit(0),  Timer8bit(1),  Timer8bit(2),  Timer8bit(3)};
        Timer16bit t16_ch[6] = {Timer16bit(0), Timer16bit(1), Timer16bit(2),
                                Timer16bit(3), Timer16bit(4), Timer16bit(5)};
        for (int i = 0; i < 4; i++) t8_ch[i].attach(bus, intc, clk);
        for (int i = 0; i < 6; i++) t16_ch[i].attach(bus, intc, clk);

        PortCtrl portctrl;
        portctrl.attach(bus, intc, &clk);

        BcuAreaCtrl bcu_area;
        bcu_area.attach(bus, cpu);

        WatchdogTimer wdt;
        wdt.attach(bus, clk,
            [&cpu](int no, int lvl) { cpu.assert_trap(no, lvl); });

        ClockTimer rtc;
        rtc.attach(bus, intc, clk);

        Hsdma hsdma;
        hsdma.attach(bus);

        Sif3 sif3;
        sif3.attach(bus, intc, hsdma);

        // Load firmware: either a bare-metal ELF or a full PFI flash image.
        uint32_t entry;
        auto writeback = std::make_unique<PfiWriteback>();
        if (!pfi_path.empty()) {
            // Install Sst39vf BEFORE pfi_load so writes go through the
            // CFI state machine and dirty tracking is wired from boot.
            bus.install_flash_device(
                std::make_unique<Sst39vf>(
                    Sst39vf::for_min_bytes(flash_size)));

            // Full-system mode: load PFI flash image, boot from reset vector.
            // The reset vector at 0xC00000 contains a 32-bit jump target.
            PfiInfo pfi_info = pfi_load(bus, pfi_path);
            // Read the reset vector (first 4 bytes of flash = word at 0xC00000)
            entry = bus.read32(Bus::FLASH_BASE);
            std::fprintf(stderr, "PFI loaded, reset vector=0x%06X\n", entry);

            // Headless writeback is opt-in to preserve test idempotency.
            // --read-only takes precedence over --enable-flash-writeback.
            const auto wb_mode = (enable_flash_writeback && !read_only)
                ? PfiWriteback::Mode::WriteBack
                : PfiWriteback::Mode::ReadOnly;
            writeback->attach(bus.flash_device(), wb_mode, pfi_path,
                              pfi_info.flash_offset_in_pfi,
                              writeback_debounce_ms);
        } else if (!elf_path.empty()) {
            entry = elf_load(bus, elf_path);
            std::fprintf(stderr, "Loaded %s, entry=0x%06X\n", elf_path.c_str(), entry);
        } else {
            std::fprintf(stderr, "Error: provide either an ELF file or --pfi <file>\n");
            return 1;
        }

        cpu.state.pc = entry;

        // Install SIGINT / SIGTERM handlers so a Ctrl-C still walks the
        // PfiWriteback shutdown_flush path on the way out.
        std::atomic<bool> signal_quit{false};
        g_quit_flag = &signal_quit;
        std::signal(SIGINT,  on_terminate_signal);
        std::signal(SIGTERM, on_terminate_signal);

        auto on_exit = [&]() {
            writeback->shutdown_flush();
            g_quit_flag = nullptr;
        };

        if (use_gdb) {
            GdbRsp gdb(bus, cpu, gdb_port, debug_rsp);
            gdb.serve();
            on_exit();
            return semihosting_test_result() != 0 ? 1 : 0;
        }

        // Tick all cycle-driven peripherals to a given cycle count.
        auto tick_all = [&](uint64_t cycles) {
            for (int i = 0; i < 4; i++) t8_ch[i].tick(cycles);
            for (int i = 0; i < 6; i++) t16_ch[i].tick(cycles);
            wdt.tick(cycles);
            rtc.tick(cycles);
        };

        bool limit_hit = false;

        auto sig_quit = [&]() {
            return signal_quit.load(std::memory_order_relaxed);
        };

        for (;;) {
            if (stop_requested || sig_quit()) break;
            // Inner loop: normal CPU execution until HLT/SLEEP or fault.
            while (!cpu.state.in_halt && !cpu.state.fault && !stop_requested
                   && !sig_quit()) {
                if (trace) {
                    std::string dis = cpu.disasm(cpu.state.pc);
                    std::fprintf(stderr, "  %s\n", dis.c_str());
                }
                bus.debug_pc = cpu.state.pc;
                if (!break_addrs.empty() && break_addrs.count(cpu.state.pc)) {
                    std::fprintf(stderr, "[BREAK] PC=0x%06X\n", cpu.state.pc);
                    print_reg_snapshot(cpu.state);
                }
                total_cycles += cpu.step();
                tick_all(total_cycles);
                if (max_cycles && total_cycles >= max_cycles) {
                    std::fprintf(stderr, "Reached max-cycles limit (%llu)\n",
                        (unsigned long long)max_cycles);
                    limit_hit = true;
                    break;
                }
            }

            // Writeback debounce check at every HLT round-trip — cheap,
            // and keeps the host file roughly in sync during long runs
            // without paying the cost on every CPU instruction.
            writeback->poll(steady_now_us());

            if (limit_hit || cpu.state.fault || stop_requested || sig_quit()) break;

            // HLT/SLEEP: fast-forward to the earliest pending peripheral event.
            uint64_t wake = UINT64_MAX;
            for (int i = 0; i < 4; i++)
                wake = std::min(wake, t8_ch[i].next_wake_cycle());
            for (int i = 0; i < 6; i++)
                wake = std::min(wake, t16_ch[i].next_wake_cycle());
            wake = std::min(wake, rtc.next_wake_cycle());

            if (wake == UINT64_MAX) {
                std::fprintf(stderr,
                    "Deadlock: CPU halted with no pending peripheral wakeup "
                    "after %llu cycles\n", (unsigned long long)total_cycles);
                break;
            }
            if (max_cycles && wake > max_cycles) {
                std::fprintf(stderr, "Reached max-cycles limit (%llu)\n",
                    (unsigned long long)max_cycles);
                limit_hit = true;
                break;
            }

            // Jump to the wake cycle and fire peripherals.
            // One of them will call assert_trap(), which clears in_halt.
            total_cycles = wake;
            tick_all(total_cycles);
        }

        if (cpu.state.fault) {
            std::fprintf(stderr, "Faulted after %llu cycles\n",
                (unsigned long long)total_cycles);
            on_exit();
            return 1;
        }

        std::fprintf(stderr, "Halted after %llu cycles\n",
            (unsigned long long)total_cycles);

        on_exit();

        int result = semihosting_test_result();
        if (result == 0)  return 0;
        if (result == -1) return 0; // clean halt, no test result written
        return 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
