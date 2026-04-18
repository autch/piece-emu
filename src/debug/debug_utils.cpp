#include "debug_utils.hpp"
#include "cpu.hpp"

#include <cstdio>

std::pair<uint32_t, uint32_t> parse_addr_size(const std::string& s)
{
    auto colon = s.find(':');
    uint32_t addr = static_cast<uint32_t>(
        std::stoul(s.substr(0, colon), nullptr, 0));
    uint32_t size = 4;
    if (colon != std::string::npos)
        size = static_cast<uint32_t>(
            std::stoul(s.substr(colon + 1), nullptr, 0));
    return {addr, size};
}

WpCallback make_wp_callback(const Bus& bus)
{
    return [&bus](const Watchpoint& /*wp*/, uint32_t addr, uint32_t val,
                  int width, bool is_write) {
        std::fprintf(stderr,
            "[WP-%s] PC=0x%06X  addr=0x%06X  val=0x%0*X  width=%d",
            is_write ? "WRITE" : "READ",
            bus.debug_pc, addr, width * 2, val, width);
        if (is_write) {
            uint32_t prev = bus.shadow_last_writer(addr);
            if (prev != 0xFFFF'FFFFu)
                std::fprintf(stderr, "  prev_writer=0x%06X", prev);
        }
        std::fprintf(stderr, "\n");
    };
}

void print_reg_snapshot(const CpuState& s)
{
    std::fprintf(stderr,
        "[SNAPSHOT] Registers:\n"
        "  R 0=%08X  R 1=%08X  R 2=%08X  R 3=%08X\n"
        "  R 4=%08X  R 5=%08X  R 6=%08X  R 7=%08X\n"
        "  R 8=%08X  R 9=%08X  R10=%08X  R11=%08X\n"
        "  R12=%08X  R13=%08X  R14=%08X  R15=%08X\n"
        "   PC=%08X   SP=%08X  PSR=%08X\n"
        "  ALR=%08X  AHR=%08X\n",
        s.r[0],  s.r[1],  s.r[2],  s.r[3],
        s.r[4],  s.r[5],  s.r[6],  s.r[7],
        s.r[8],  s.r[9],  s.r[10], s.r[11],
        s.r[12], s.r[13], s.r[14], s.r[15],
        s.pc, s.sp, s.psr.raw, s.alr, s.ahr);
}
