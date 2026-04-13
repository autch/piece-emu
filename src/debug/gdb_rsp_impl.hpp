// gdb_rsp_impl.hpp — RSP wire-encoding utilities shared by gdb_rsp*.cpp.
// Do NOT include from public headers.
#pragma once
#include <cstdint>
#include <format>
#include <string>

inline std::string to_hex8(uint8_t v) { return std::format("{:02x}", v); }

// RSP spec: register values are transmitted in the target's native byte order
// (LSB first for little-endian targets). See:
// https://sourceware.org/gdb/current/onlinedocs/gdb.html/Packets.html
inline std::string to_hex32(uint32_t v) {
    return std::format("{:02x}{:02x}{:02x}{:02x}",
        v & 0xFF, (v>>8)&0xFF, (v>>16)&0xFF, (v>>24)&0xFF);
}

inline uint32_t from_hex32(const std::string& s, std::size_t off) {
    auto h = [&](int i) -> uint32_t {
        char c = s[off + i];
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    uint32_t b0 = (h(0)<<4)|h(1);
    uint32_t b1 = (h(2)<<4)|h(3);
    uint32_t b2 = (h(4)<<4)|h(5);
    uint32_t b3 = (h(6)<<4)|h(7);
    return b0 | (b1<<8) | (b2<<16) | (b3<<24);
}

inline uint8_t rsp_checksum(const std::string& s) {
    uint8_t sum = 0;
    for (char c : s) sum += static_cast<uint8_t>(c);
    return sum;
}

// Returns a framed RSP packet without a leading ack.
// The serve() loop sends '+' ack separately (unless in no-ack mode).
inline std::string rsp_packet(const std::string& data) {
    return std::format("${}#{:02x}", data, rsp_checksum(data));
}
