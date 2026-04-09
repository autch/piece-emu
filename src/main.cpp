#include "bus.hpp"
#include "cpu.hpp"
#include "elf_loader.hpp"
#include "gdb_rsp.hpp"
#include "semihosting.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <time.h>

static void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [options] <elf-file>\n"
        "Options:\n"
        "  --gdb [port]     Wait for GDB connection on port (default 1234) before running\n"
        "  --debug-rsp      Log RSP packets to stderr (requires --gdb)\n"
        "  --max-cycles N   Stop after N cycles (default: unlimited)\n"
        "  --trace          Print disassembly for each instruction\n"
        "\n"
        "The ELF entry point overrides the boot vector unless the binary is a full\n"
        "firmware image (linked for Flash at 0x0C00000).\n",
        argv0);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    bool     use_gdb    = false;
    uint16_t gdb_port   = 1234;
    bool     debug_rsp  = false;
    uint64_t max_cycles = 0; // 0 = unlimited
    bool     trace      = false;
    std::string elf_path;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--gdb") {
            use_gdb = true;
            if (i + 1 < argc && argv[i+1][0] != '-')
                gdb_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--debug-rsp") {
            debug_rsp = true;
        } else if (arg == "--max-cycles" && i + 1 < argc) {
            max_cycles = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--trace") {
            trace = true;
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return 1;
        } else {
            elf_path = arg;
        }
    }

    if (elf_path.empty()) { usage(argv[0]); return 1; }

    try {
        // Create the bus (512 KB Flash default)
        Bus bus(0x80000);

        // Create the CPU
        Cpu cpu(bus);

        // Register semihosting handlers
        semihosting_init(bus, cpu);

        // Load ELF
        uint32_t entry = elf_load(bus, elf_path);
        std::fprintf(stderr, "Loaded %s, entry=0x%06X\n", elf_path.c_str(), entry);

        // Set PC to ELF entry point (overrides boot vector)
        cpu.state.pc = entry;

        // GDB mode: wait for connection, then serve
        if (use_gdb) {
            GdbRsp gdb(bus, cpu, gdb_port, debug_rsp);
            gdb.serve();
            return semihosting_test_result() != 0 ? 1 : 0;
        }

        // Run loop
        uint64_t total_cycles = 0;
        while (!cpu.state.in_halt) {
            if (trace) {
                std::string dis = cpu.disasm(cpu.state.pc);
                std::fprintf(stderr, "  %s\n", dis.c_str());
            }
            int cycles = cpu.step();
            total_cycles += cycles;
            if (max_cycles && total_cycles >= max_cycles) {
                std::fprintf(stderr, "Reached max-cycles limit (%llu)\n",
                    (unsigned long long)max_cycles);
                break;
            }
        }

        if (cpu.state.fault) {
            std::fprintf(stderr, "Faulted after %llu cycles\n", (unsigned long long)total_cycles);
            return 1;
        }

        std::fprintf(stderr, "Halted after %llu cycles\n", (unsigned long long)total_cycles);

        int result = semihosting_test_result();
        if (result == 0)  return 0;
        if (result == -1) return 0; // clean halt, no test result written
        return 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
