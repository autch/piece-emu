#include "gdb_rsp.hpp"
#include "bus.hpp"
#include "cpu.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <format>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

// ============================================================================
// GDB Remote Serial Protocol (RSP) — P0 skeleton
//
// Register packet layout (matches MAME state_add order):
//   R0–R15 (16 × 4 bytes LE), PC (4), SP (4), PSR (4), ALR (4), AHR (4)
//   Total: 21 × 4 = 84 bytes = 168 hex chars
// ============================================================================

static std::string to_hex8(uint8_t v)  { return std::format("{:02x}", v); }
static std::string to_hex32_le(uint32_t v) {
    // Little-endian byte order in RSP register packets
    return std::format("{:02x}{:02x}{:02x}{:02x}",
        v & 0xFF, (v>>8)&0xFF, (v>>16)&0xFF, (v>>24)&0xFF);
}
static uint32_t from_hex32_le(const std::string& s, std::size_t off) {
    auto h = [&](int i) -> uint32_t {
        char c = s[off + i];
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    // 8 hex digits, LE: first byte = bits[7:0]
    uint32_t b0 = (h(0)<<4)|h(1);
    uint32_t b1 = (h(2)<<4)|h(3);
    uint32_t b2 = (h(4)<<4)|h(5);
    uint32_t b3 = (h(6)<<4)|h(7);
    return b0 | (b1<<8) | (b2<<16) | (b3<<24);
}

static uint8_t rsp_checksum(const std::string& s) {
    uint8_t sum = 0;
    for (char c : s) sum += static_cast<uint8_t>(c);
    return sum;
}

static std::string rsp_packet(const std::string& data) {
    return std::format("+${}#{:02x}", data, rsp_checksum(data));
}

GdbRsp::GdbRsp(Bus& bus, Cpu& cpu, uint16_t port)
    : bus_(bus), cpu_(cpu), port_(port)
{
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error("GdbRsp: socket failed");
    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::format("GdbRsp: bind failed on port {}", port_));
    ::listen(listen_fd_, 1);
}

GdbRsp::~GdbRsp() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

std::string GdbRsp::reg_read_all() {
    std::string s;
    for (int i = 0; i < 16; i++) s += to_hex32_le(cpu_.state.r[i]);
    s += to_hex32_le(cpu_.state.pc);
    s += to_hex32_le(cpu_.state.sp);
    s += to_hex32_le(cpu_.state.psr.raw);
    s += to_hex32_le(cpu_.state.alr);
    s += to_hex32_le(cpu_.state.ahr);
    return s;
}

void GdbRsp::reg_write_all(const std::string& hex) {
    if (hex.size() < 21 * 8) return; // too short
    std::size_t off = 0;
    for (int i = 0; i < 16; i++, off += 8) cpu_.state.r[i]    = from_hex32_le(hex, off);
    cpu_.state.pc      = from_hex32_le(hex, off); off += 8;
    cpu_.state.sp      = from_hex32_le(hex, off); off += 8;
    cpu_.state.psr.raw = from_hex32_le(hex, off); off += 8;
    cpu_.state.alr     = from_hex32_le(hex, off); off += 8;
    cpu_.state.ahr     = from_hex32_le(hex, off);
}

void GdbRsp::handle_packet(int fd, const std::string& pkt) {
    auto send_rsp = [&](const std::string& data) {
        std::string msg = rsp_packet(data);
        ::send(fd, msg.c_str(), msg.size(), 0);
    };

    if (pkt.empty()) { send_rsp(""); return; }

    char cmd = pkt[0];

    switch (cmd) {
    case '?':
        send_rsp("S05"); // SIGTRAP — stopped
        break;
    case 'g':
        send_rsp(reg_read_all());
        break;
    case 'G':
        reg_write_all(pkt.substr(1));
        send_rsp("OK");
        break;
    case 'm': {
        // m addr,length
        uint32_t addr = 0, len = 0;
        std::sscanf(pkt.c_str() + 1, "%x,%x", &addr, &len);
        std::string mem;
        for (uint32_t i = 0; i < len; i++)
            mem += to_hex8(bus_.read8(addr + i));
        send_rsp(mem);
        break;
    }
    case 'M': {
        // M addr,length:data
        uint32_t addr = 0, len = 0;
        int colon = static_cast<int>(pkt.find(':'));
        std::sscanf(pkt.c_str() + 1, "%x,%x", &addr, &len);
        if (colon > 0) {
            const std::string& hexdata = pkt.substr(colon + 1);
            for (uint32_t i = 0; i < len && i * 2 + 1 < hexdata.size(); i++) {
                uint8_t b = 0;
                std::sscanf(hexdata.c_str() + i * 2, "%02hhx", &b);
                bus_.write8(addr + i, b);
            }
        }
        send_rsp("OK");
        break;
    }
    case 'c':
    case 's': {
        // Step or continue — run until halt / breakpoint
        // P0: just run one instruction for 's', loop for 'c'
        bool single = (cmd == 's');
        cpu_.state.in_halt = false;
        if (single) {
            cpu_.step();
        } else {
            while (!cpu_.state.in_halt)
                cpu_.step();
        }
        send_rsp("S05");
        break;
    }
    case 'q':
        if (pkt.substr(0, 12) == "qSupported:") {
            send_rsp("PacketSize=4000");
        } else if (pkt == "qAttached") {
            send_rsp("1");
        } else {
            send_rsp(""); // not supported
        }
        break;
    default:
        send_rsp(""); // not supported
        break;
    }
}

void GdbRsp::serve() {
    std::fprintf(stderr, "GDB RSP: waiting for connection on port %u...\n", port_);
    int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) return;
    std::fprintf(stderr, "GDB RSP: client connected\n");

    std::string buf;
    char tmp[256];
    while (true) {
        ssize_t n = ::recv(client, tmp, sizeof(tmp) - 1, 0);
        if (n <= 0) break;
        tmp[n] = '\0';
        buf += tmp;

        // Process packets: +, -, $...#xx
        while (!buf.empty()) {
            if (buf[0] == '+' || buf[0] == '-') {
                buf.erase(0, 1);
                continue;
            }
            if (buf[0] == '\x03') { // Ctrl-C: interrupt
                cpu_.state.in_halt = true;
                buf.erase(0, 1);
                std::string stop = rsp_packet("S02");
                ::send(client, stop.c_str(), stop.size(), 0);
                continue;
            }
            if (buf[0] == '$') {
                std::size_t hash = buf.find('#', 1);
                if (hash == std::string::npos || hash + 2 >= buf.size()) break; // incomplete
                std::string pkt_data = buf.substr(1, hash - 1);
                // Send ack
                ::send(client, "+", 1, 0);
                handle_packet(client, pkt_data);
                buf.erase(0, hash + 3);
                continue;
            }
            buf.erase(0, 1); // discard unknown
        }
    }
    ::close(client);
    std::fprintf(stderr, "GDB RSP: client disconnected\n");
}
