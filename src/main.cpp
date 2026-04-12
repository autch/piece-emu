#include "bus.hpp"
#include "cpu.hpp"
#include "diag.hpp"
#include "elf_loader.hpp"
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

#include <CLI/CLI.hpp>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    CLI::App app{"P/ECE bare-metal emulator (S1C33209)"};
    argv = app.ensure_utf8(argv);

    std::string  elf_path;
    bool         use_gdb   = false;
    uint16_t     gdb_port  = 1234;
    bool         debug_rsp = false;
    uint64_t     max_cycles = 0;
    bool         trace      = false;
    std::size_t  sram_size  = 0x040000; // 256 KB
    std::size_t  flash_size = 0x080000; // 512 KB

    app.add_option("elf", elf_path, "ELF binary to load and run")
        ->required()
        ->check(CLI::ExistingFile);

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

    CLI11_PARSE(app, argc, argv);

    try {
        Bus bus(sram_size, flash_size);
        Cpu cpu(bus);

        StderrDiagSink diag_sink;
        cpu.set_diag(&diag_sink);
        bus.set_diag(&diag_sink);

        // Declared early so semihosting lambdas can capture by reference.
        uint64_t total_cycles = 0;

        semihosting_init(bus, cpu, {
            .get_cycles = [&]() -> uint64_t { return total_cycles; },
            .set_trace  = [&](bool on) { trace = on; }
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

        uint32_t entry = elf_load(bus, elf_path);
        std::fprintf(stderr, "Loaded %s, entry=0x%06X\n", elf_path.c_str(), entry);

        cpu.state.pc = entry;

        if (use_gdb) {
            GdbRsp gdb(bus, cpu, gdb_port, debug_rsp);
            gdb.serve();
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

        for (;;) {
            // Inner loop: normal CPU execution until HLT/SLEEP or fault.
            while (!cpu.state.in_halt && !cpu.state.fault) {
                if (trace) {
                    std::string dis = cpu.disasm(cpu.state.pc);
                    std::fprintf(stderr, "  %s\n", dis.c_str());
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

            if (limit_hit || cpu.state.fault) break;

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
            return 1;
        }

        std::fprintf(stderr, "Halted after %llu cycles\n",
            (unsigned long long)total_cycles);

        int result = semihosting_test_result();
        if (result == 0)  return 0;
        if (result == -1) return 0; // clean halt, no test result written
        return 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
