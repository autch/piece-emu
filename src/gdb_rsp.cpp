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
// GDB Remote Serial Protocol (RSP)
//
// Register packet layout:
//   R0–R15 (16 × 4 bytes LE), SP (4), PC (4), PSR (4), ALR (4), AHR (4)
//   Total: 21 × 4 = 84 bytes = 168 hex chars
//
// Supports GDB and LLDB clients.
// ============================================================================

static std::string to_hex8(uint8_t v)  { return std::format("{:02x}", v); }

// RSP spec: register values are transmitted in the target's native byte order
// (LSB first for little-endian targets). See:
// https://sourceware.org/gdb/current/onlinedocs/gdb.html/Packets.html
static std::string to_hex32(uint32_t v) {
    return std::format("{:02x}{:02x}{:02x}{:02x}",
        v & 0xFF, (v>>8)&0xFF, (v>>16)&0xFF, (v>>24)&0xFF);
}
static uint32_t from_hex32(const std::string& s, std::size_t off) {
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

static uint8_t rsp_checksum(const std::string& s) {
    uint8_t sum = 0;
    for (char c : s) sum += static_cast<uint8_t>(c);
    return sum;
}

// Returns a framed RSP packet without a leading ack.
// The serve() loop sends '+' ack separately (unless in no-ack mode).
static std::string rsp_packet(const std::string& data) {
    return std::format("${}#{:02x}", data, rsp_checksum(data));
}

GdbRsp::GdbRsp(Bus& bus, Cpu& cpu, uint16_t port, bool debug)
    : bus_(bus), cpu_(cpu), port_(port), debug_(debug)
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

// ---------------------------------------------------------------------------
// Register access
// ---------------------------------------------------------------------------

std::string GdbRsp::reg_read_all() {
    std::string s;
    for (int i = 0; i < 16; i++) s += to_hex32(cpu_.state.r[i]);
    s += to_hex32(cpu_.state.sp);
    s += to_hex32(cpu_.state.pc);
    s += to_hex32(cpu_.state.psr.raw);
    s += to_hex32(cpu_.state.alr);
    s += to_hex32(cpu_.state.ahr);
    return s;
}

void GdbRsp::reg_write_all(const std::string& hex) {
    if (hex.size() < 21 * 8) return;
    std::size_t off = 0;
    for (int i = 0; i < 16; i++, off += 8) cpu_.state.r[i]    = from_hex32(hex, off);
    cpu_.state.sp      = from_hex32(hex, off); off += 8;
    cpu_.state.pc      = from_hex32(hex, off); off += 8;
    cpu_.state.psr.raw = from_hex32(hex, off); off += 8;
    cpu_.state.alr     = from_hex32(hex, off); off += 8;
    cpu_.state.ahr     = from_hex32(hex, off);
}

std::string GdbRsp::reg_read_n(int n) {
    if (n < 0 || n > 20) return "E00";
    if (n < 16) return to_hex32(cpu_.state.r[n]);
    switch (n) {
    case 16: return to_hex32(cpu_.state.sp);
    case 17: return to_hex32(cpu_.state.pc);
    case 18: return to_hex32(cpu_.state.psr.raw);
    case 19: return to_hex32(cpu_.state.alr);
    case 20: return to_hex32(cpu_.state.ahr);
    }
    return "E00";
}

void GdbRsp::reg_write_n(int n, uint32_t val) {
    if (n < 0 || n > 20) return;
    if (n < 16) { cpu_.state.r[n] = val; return; }
    switch (n) {
    case 16: cpu_.state.sp      = val; break;
    case 17: cpu_.state.pc      = val; break;
    case 18: cpu_.state.psr.raw = val; break;
    case 19: cpu_.state.alr     = val; break;
    case 20: cpu_.state.ahr     = val; break;
    }
}

// LLDB qRegisterInfo<n> response.
// Fields: name, bitsize, offset (in the g-packet register block), encoding,
//         format, set, dwarf register number, optional generic role.
std::string GdbRsp::reg_info(int n) {
    if (n < 0 || n > 20) return "E45";

    struct Info { const char* name; const char* generic; const char* set; };
    static constexpr Info kInfo[21] = {
        {"r0",  nullptr,  "General Purpose Registers"},
        {"r1",  nullptr,  "General Purpose Registers"},
        {"r2",  nullptr,  "General Purpose Registers"},
        {"r3",  nullptr,  "General Purpose Registers"},
        {"r4",  nullptr,  "General Purpose Registers"},
        {"r5",  nullptr,  "General Purpose Registers"},
        {"r6",  nullptr,  "General Purpose Registers"},
        {"r7",  nullptr,  "General Purpose Registers"},
        {"r8",  nullptr,  "General Purpose Registers"},
        {"r9",  nullptr,  "General Purpose Registers"},
        {"r10", nullptr,  "General Purpose Registers"},
        {"r11", nullptr,  "General Purpose Registers"},
        {"r12", nullptr,  "General Purpose Registers"},
        {"r13", nullptr,  "General Purpose Registers"},
        {"r14", nullptr,  "General Purpose Registers"},
        {"r15", nullptr,  "General Purpose Registers"},
        {"sp",  "sp",     "General Purpose Registers"},
        {"pc",  "pc",     "General Purpose Registers"},
        {"psr", "flags",  "General Purpose Registers"},
        {"alr", nullptr,  "Multiply Registers"},
        {"ahr", nullptr,  "Multiply Registers"},
    };

    std::string resp = std::format(
        "name:{};bitsize:32;offset:{};encoding:uint;format:hex;endian:little;set:{};dwarf:{};",
        kInfo[n].name, n * 4, kInfo[n].set, n);
    if (kInfo[n].generic)
        resp += std::format("generic:{};", kInfo[n].generic);
    return resp;
}

// ---------------------------------------------------------------------------
// Target XML for qXfer:features:read:target.xml
// ---------------------------------------------------------------------------

static constexpr const char* kTargetXml = R"(<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target version="1.0">
  <feature name="org.gnu.gdb.s1c33.core">
    <reg name="r0"  bitsize="32" regnum="0"  type="uint32"/>
    <reg name="r1"  bitsize="32" regnum="1"  type="uint32"/>
    <reg name="r2"  bitsize="32" regnum="2"  type="uint32"/>
    <reg name="r3"  bitsize="32" regnum="3"  type="uint32"/>
    <reg name="r4"  bitsize="32" regnum="4"  type="uint32"/>
    <reg name="r5"  bitsize="32" regnum="5"  type="uint32"/>
    <reg name="r6"  bitsize="32" regnum="6"  type="uint32"/>
    <reg name="r7"  bitsize="32" regnum="7"  type="uint32"/>
    <reg name="r8"  bitsize="32" regnum="8"  type="uint32"/>
    <reg name="r9"  bitsize="32" regnum="9"  type="uint32"/>
    <reg name="r10" bitsize="32" regnum="10" type="uint32"/>
    <reg name="r11" bitsize="32" regnum="11" type="uint32"/>
    <reg name="r12" bitsize="32" regnum="12" type="uint32"/>
    <reg name="r13" bitsize="32" regnum="13" type="uint32"/>
    <reg name="r14" bitsize="32" regnum="14" type="uint32"/>
    <reg name="r15" bitsize="32" regnum="15" type="uint32"/>
    <reg name="sp"  bitsize="32" regnum="16" type="data_ptr"/>
    <reg name="pc"  bitsize="32" regnum="17" type="code_ptr"/>
    <reg name="psr" bitsize="32" regnum="18" type="uint32"/>
    <reg name="alr" bitsize="32" regnum="19" type="uint32"/>
    <reg name="ahr" bitsize="32" regnum="20" type="uint32"/>
  </feature>
</target>)";

std::string GdbRsp::handle_qXfer(const std::string& pkt) {
    // pkt = "qXfer:features:read:target.xml:OFF,LEN"
    auto colon = pkt.rfind(':');
    auto comma  = pkt.find(',', colon);
    unsigned off = std::stoul(pkt.substr(colon + 1, comma - colon - 1), nullptr, 16);
    unsigned len = std::stoul(pkt.substr(comma + 1), nullptr, 16);

    std::string_view xml(kTargetXml);
    if (off >= xml.size()) return "l";
    auto chunk = xml.substr(off, len);
    bool last  = (off + chunk.size() >= xml.size());
    return std::string(1, last ? 'l' : 'm') + std::string(chunk);
}

// ---------------------------------------------------------------------------
// Execution helper
// ---------------------------------------------------------------------------

// Run the CPU until a stop condition is reached.
// single=true: execute exactly one instruction.
// single=false: run until halt, fault, or a breakpoint address is hit.
// Returns the RSP stop reply string.
std::string GdbRsp::run(bool single) {
    cpu_.state.in_halt = false;
    if (single) {
        cpu_.step();
    } else {
        while (!cpu_.state.in_halt) {
            cpu_.step();
            // Check breakpoint after each instruction (before next fetch).
            if (!cpu_.state.in_halt && breakpoints_.count(cpu_.state.pc))
                break;
        }
    }
    // Distinguish fault (undefined opcode, jp.d %rb, …) from normal stop.
    if (cpu_.state.fault) return "T04thread:1;"; // SIGILL
    return "T05thread:1;";                        // SIGTRAP
}

// ---------------------------------------------------------------------------
// Packet handler
// ---------------------------------------------------------------------------

void GdbRsp::handle_packet(int fd, const std::string& pkt) {
    auto send_rsp = [&](const std::string& data) {
        std::string msg = rsp_packet(data);
        if (debug_) std::fprintf(stderr, "GDB RSP: >> %s\n", data.c_str());
        ::send(fd, msg.c_str(), msg.size(), 0);
    };

    if (pkt.empty()) { send_rsp(""); return; }

    char cmd = pkt[0];

    switch (cmd) {

    // -----------------------------------------------------------------------
    // Standard packets
    // -----------------------------------------------------------------------

    case '?':
        send_rsp("T05thread:1;");
        break;

    case 'g':
        send_rsp(reg_read_all());
        break;

    case 'G':
        reg_write_all(pkt.substr(1));
        send_rsp("OK");
        break;

    case 'p': {
        // p<n> — read single register (LLDB primary register access path)
        int n = std::stoi(pkt.substr(1), nullptr, 16);
        send_rsp(reg_read_n(n));
        break;
    }

    case 'P': {
        // P<n>=<hexval> — write single register
        auto eq = pkt.find('=');
        if (eq == std::string::npos) { send_rsp("E00"); break; }
        int n = std::stoi(pkt.substr(1, eq - 1), nullptr, 16);
        uint32_t val = from_hex32(pkt, eq + 1);
        reg_write_n(n, val);
        send_rsp("OK");
        break;
    }

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
        auto colon = pkt.find(':');
        std::sscanf(pkt.c_str() + 1, "%x,%x", &addr, &len);
        if (colon != std::string::npos) {
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
    case 's':
        send_rsp(run(cmd == 's'));
        break;

    // -----------------------------------------------------------------------
    // H — set thread for subsequent operations (bare-metal: always thread 1)
    // -----------------------------------------------------------------------

    case 'H':
        send_rsp("OK");
        break;

    // -----------------------------------------------------------------------
    // T — is thread alive?
    // -----------------------------------------------------------------------

    case 'T':
        send_rsp("OK");
        break;

    // -----------------------------------------------------------------------
    // Z / z — set / remove breakpoint
    // Only Z0/z0 (software breakpoints) are supported.
    // Virtual implementation: no instruction patching; the run() loop checks
    // the PC against the breakpoint set after every instruction.
    // -----------------------------------------------------------------------

    case 'Z': {
        if (pkt[1] != '0') { send_rsp(""); break; } // only Z0
        uint32_t addr = 0; unsigned kind = 0;
        std::sscanf(pkt.c_str() + 3, "%x,%x", &addr, &kind);
        breakpoints_.insert(addr);
        send_rsp("OK");
        break;
    }

    case 'z': {
        if (pkt[1] != '0') { send_rsp(""); break; } // only z0
        uint32_t addr = 0; unsigned kind = 0;
        std::sscanf(pkt.c_str() + 3, "%x,%x", &addr, &kind);
        breakpoints_.erase(addr);
        send_rsp("OK");
        break;
    }

    // -----------------------------------------------------------------------
    // Q — set / mode packets
    // -----------------------------------------------------------------------

    case 'Q':
        if (pkt == "QStartNoAckMode") {
            send_rsp("OK");
            no_ack_mode_ = true;
        } else if (pkt.starts_with("QThreadSuffixSupported") ||
                   pkt.starts_with("QListThreadsInStopReply") ||
                   pkt.starts_with("QEnableErrorStrings")) {
            send_rsp("OK");
        } else {
            send_rsp("");
        }
        break;

    // -----------------------------------------------------------------------
    // v — multi-letter packets
    // -----------------------------------------------------------------------

    case 'v':
        if (pkt == "vCont?") {
            send_rsp("vCont;c;s;");
        } else if (pkt.starts_with("vCont;")) {
            // vCont;c or vCont;c:1 — continue
            // vCont;s or vCont;s:1 — step
            char action = pkt[6]; // 'c' or 's'
            send_rsp(run(action == 's'));
        } else {
            send_rsp("");
        }
        break;

    // -----------------------------------------------------------------------
    // q — query packets
    // -----------------------------------------------------------------------

    case 'q':
        if (pkt.starts_with("qSupported:")) {
            send_rsp("PacketSize=4000;qXfer:features:read+;qRegisterInfo+;vContSupported+");

        } else if (pkt == "qAttached") {
            send_rsp("1");

        } else if (pkt == "qC") {
            // Current thread ID
            send_rsp("QC1");

        } else if (pkt == "qfThreadInfo") {
            send_rsp("m1");

        } else if (pkt == "qsThreadInfo") {
            send_rsp("l");

        } else if (pkt == "qHostInfo") {
            // LLDB uses this to determine the remote architecture.
            // s1c33-unknown-none-elf may not be in LLDB's arch list,
            // but returning a proper triple lets LLDB fall back gracefully
            // to qRegisterInfo for register layout.
            send_rsp("triple:s1c33-unknown-none-elf;endian:little;ptrsize:4;");

        } else if (pkt == "qProcessInfo") {
            send_rsp("pid:1;triple:s1c33-unknown-none-elf;endian:little;ptrsize:4;");

        } else if (pkt.starts_with("qRegisterInfo")) {
            // LLDB queries qRegisterInfo0, qRegisterInfo1, ... until E45
            int n = std::stoi(pkt.substr(13), nullptr, 16);
            send_rsp(reg_info(n));

        } else if (pkt.starts_with("qXfer:features:read:target.xml:")) {
            send_rsp(handle_qXfer(pkt));

        } else if (pkt == "qVAttachOrWaitSupported" ||
                   pkt == "qStructuredDataPlugins"  ||
                   pkt == "qShlibInfoAddr") {
            send_rsp("");

        } else {
            send_rsp("");
        }
        break;

    // -----------------------------------------------------------------------
    // j — LLDB JSON extension packets (respond empty — not required)
    // -----------------------------------------------------------------------

    case 'j':
        send_rsp("");
        break;

    default:
        send_rsp("");
        break;
    }
}

// ---------------------------------------------------------------------------
// Server loop
// ---------------------------------------------------------------------------

void GdbRsp::serve() {
    std::fprintf(stderr, "GDB RSP: waiting for connection on port %u...\n", port_);
    int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) return;
    std::fprintf(stderr, "GDB RSP: client connected\n");
    no_ack_mode_ = false;
    breakpoints_.clear();

    std::string buf;
    char tmp[256];
    while (true) {
        ssize_t n = ::recv(client, tmp, sizeof(tmp) - 1, 0);
        if (n <= 0) break;
        tmp[n] = '\0';
        buf += tmp;

        while (!buf.empty()) {
            if (buf[0] == '+' || buf[0] == '-') {
                buf.erase(0, 1);
                continue;
            }
            if (buf[0] == '\x03') { // Ctrl-C: interrupt
                cpu_.state.in_halt = true;
                buf.erase(0, 1);
                std::string stop = rsp_packet("T02thread:1;");
                ::send(client, stop.c_str(), stop.size(), 0);
                continue;
            }
            if (buf[0] == '$') {
                std::size_t hash = buf.find('#', 1);
                if (hash == std::string::npos || hash + 2 >= buf.size()) break; // incomplete
                std::string pkt_data = buf.substr(1, hash - 1);
                if (!no_ack_mode_)
                    ::send(client, "+", 1, 0);
                if (debug_) std::fprintf(stderr, "GDB RSP: << %s\n", pkt_data.c_str());
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
