#include "gdb_rsp.hpp"
#include "gdb_rsp_impl.hpp"
#include "bus.hpp"
#include "cpu.hpp"

#include <cassert>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// Cross-platform socket shims (Winsock2 on Windows, BSD sockets on POSIX).
//
// The rest of this translation unit uses socket_t / kInvalidSock / close_sock()
// so the platform divergence is confined to the block below.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
using ssize_t  = int;
static constexpr socket_t kInvalidSock = INVALID_SOCKET;
static inline int close_sock(socket_t s) { return ::closesocket(s); }

// Cast helper for send/recv buffer length: Winsock takes int, POSIX takes size_t.
static inline int sock_len(std::size_t n) { return static_cast<int>(n); }

// Winsock requires per-process WSAStartup before any socket call, and a
// matching WSACleanup at shutdown. A function-local static handles both with
// guaranteed single initialisation and atexit-ordered teardown.
namespace {
struct WsaInit {
    WsaInit() {
        WSADATA wsa{};
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            throw std::runtime_error("GdbRsp: WSAStartup failed");
    }
    ~WsaInit() { ::WSACleanup(); }
};
inline void ensure_winsock() { static WsaInit init; }
} // namespace
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <pthread.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSock = -1;
static inline int close_sock(socket_t s) { return ::close(s); }
static inline std::size_t sock_len(std::size_t n) { return n; }
static inline void ensure_winsock() {}
#endif

// ============================================================================
// GDB Remote Serial Protocol (RSP)
//
// Register packet layout:
//   R0–R15 (16 × 4 bytes LE), SP (4), PC (4), PSR (4), ALR (4), AHR (4)
//   Total: 21 × 4 = 84 bytes = 168 hex chars
//
// Supports GDB and LLDB clients.
// ============================================================================

GdbRsp::GdbRsp(Bus& bus, Cpu& cpu, uint16_t port, bool debug)
    : bus_(bus), cpu_(cpu), port_(port), debug_(debug)
{
    ensure_winsock();
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSock)
        throw std::runtime_error("GdbRsp: socket failed");
    int opt = 1;
    // Winsock's setsockopt takes const char*; POSIX accepts any pointer
    // through const void*. The char* cast is safe on both platforms.
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_sock(s);
        throw std::runtime_error(std::format("GdbRsp: bind failed on port {}", port_));
    }
    ::listen(s, 1);
    listen_fd_ = static_cast<intptr_t>(s);
}

GdbRsp::~GdbRsp() {
    if (listen_fd_ != -1) close_sock(static_cast<socket_t>(listen_fd_));
}

// ---------------------------------------------------------------------------
// Execution helper
// ---------------------------------------------------------------------------

// Run the CPU until a stop condition is reached.
// single=true: execute exactly one instruction.
// single=false: run until halt, fault, or a breakpoint address is hit.
// Returns the RSP stop reply string.
//
// In async mode (serve_async() was used): instead of stepping the CPU
// directly, signals the main loop via async_run_cmd_ and blocks until
// notify_async_stopped() is called by the main loop.
std::string GdbRsp::run(bool single) {
    if (async_mode_) {
        // Signal the main loop to start CPU execution.
        async_run_cmd_.store(single ? 1 : 2);

        // Block until the main loop signals that the CPU has stopped.
        std::unique_lock<std::mutex> lk(async_mutex_);
        async_stopped_ = false;
        async_stopped_cv_.wait(lk, [this] { return async_stopped_; });
        return async_stop_reply_;
    }

    // --- Synchronous (headless) mode -----------------------------------------
    cpu_.state.in_halt   = false;
    cpu_.state.halt_mode = CpuState::HaltMode::None;
    wp_hit_ = false;

    auto step_one = [&]() {
        bus_.debug_pc = cpu_.state.pc;
        cpu_.step();
    };

    if (single) {
        step_one();
    } else {
        while (!cpu_.state.in_halt) {
            step_one();
            if (wp_hit_) break;
            if (!cpu_.state.in_halt && breakpoints_.count(cpu_.state.pc))
                break;
        }
    }

    return make_async_stop_reply();
}

// ---------------------------------------------------------------------------
// Async mode helpers
// ---------------------------------------------------------------------------

void GdbRsp::serve_async() {
    async_mode_ = true;
    async_thread_ = std::thread([this]() {
#if defined(_WIN32)
        // SetThreadDescription is Win10 1607+. Older Windows silently no-ops
        // via the loader stub; a missing export just leaves the thread unnamed.
        ::SetThreadDescription(::GetCurrentThread(), L"piece-gdb");
#elif defined(__APPLE__)
        pthread_setname_np("piece-gdb");
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), "piece-gdb");
#endif
        serve();
    });
    async_thread_.detach();
}

// Called by the main loop: take the pending run command (if any).
// Returns true if the RSP thread has issued a run/step request.
// *single_out = true → step once; false → run until breakpoint/halt.
// Resets in_halt and wp_hit_ so the CPU starts fresh.
bool GdbRsp::take_async_run_cmd(bool* single_out) {
    int cmd = async_run_cmd_.exchange(0);
    if (cmd == 0) return false;
    *single_out = (cmd == 1);
    cpu_.state.in_halt   = false;
    cpu_.state.halt_mode = CpuState::HaltMode::None;
    wp_hit_ = false;
    return true;
}

// Called by the main loop after the CPU stops.
// Wakes the blocked RSP thread so it can send the stop reply to the client.
void GdbRsp::notify_async_stopped(std::string reply) {
    {
        std::lock_guard<std::mutex> lk(async_mutex_);
        async_stop_reply_ = std::move(reply);
        async_stopped_    = true;
    }
    async_stopped_cv_.notify_one();
}

// Build the RSP stop reply from the current CPU / watchpoint state.
// Used both by the synchronous run() (via refactored body) and by the
// main loop in async mode.
std::string GdbRsp::make_async_stop_reply() const {
    // Fault (undefined opcode, jp.d %rb, …) → SIGILL
    if (cpu_.state.fault) return "T04thread:1;";

    // Watchpoint hit → encode stop reason and triggering address
    if (wp_hit_) {
        // GDB RSP stop-reason keys for watchpoints:
        //   write access : "watch"   (Z2)
        //   read  access : "rwatch"  (Z3)
        //   any   access : "awatch"  (Z4)
        static constexpr const char* wp_key[] = {
            nullptr, nullptr, "watch", "rwatch", "awatch"
        };
        const char* key = (wp_hit_ztype_ >= 2 && wp_hit_ztype_ <= 4)
                        ? wp_key[wp_hit_ztype_] : "watch";
        return std::format("T05{}:{};thread:1;", key, to_hex32(wp_hit_addr_));
    }

    return "T05thread:1;"; // SIGTRAP — breakpoint or step complete
}

// ---------------------------------------------------------------------------
// Packet handler
// ---------------------------------------------------------------------------

void GdbRsp::handle_packet(intptr_t fd, const std::string& pkt) {
    socket_t sock = static_cast<socket_t>(fd);
    auto send_rsp = [&](const std::string& data) {
        std::string msg = rsp_packet(data);
        if (debug_) std::fprintf(stderr, "GDB RSP: >> %s\n", data.c_str());
        ::send(sock, msg.c_str(), sock_len(msg.size()), 0);
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
    // Z / z — set / remove breakpoint or watchpoint
    //
    //   Z0/z0  Software breakpoint  — virtual: checked after each step
    //   Z1/z1  Hardware breakpoint  — same implementation as Z0
    //   Z2/z2  Write watchpoint     — Bus::add/remove_watchpoint WRITE
    //   Z3/z3  Read  watchpoint     — Bus::add/remove_watchpoint READ
    //   Z4/z4  Access watchpoint    — Bus::add/remove_watchpoint RW
    //
    // Watchpoint kind field = byte count to watch (1/2/4; default 4).
    // -----------------------------------------------------------------------

    case 'Z': {
        int      ztype = pkt[1] - '0';
        uint32_t addr  = 0;
        unsigned kind  = 0;
        std::sscanf(pkt.c_str() + 3, "%x,%x", &addr, &kind);
        if (kind == 0) kind = 4; // default to 4-byte watch

        if (ztype == Z_SW_BREAK || ztype == Z_HW_BREAK) {
            breakpoints_.insert(addr);
            send_rsp("OK");
        } else if (ztype == Z_WP_WRITE) {
            bus_.add_watchpoint(addr, kind, WpType::WRITE);
            send_rsp("OK");
        } else if (ztype == Z_WP_READ) {
            bus_.add_watchpoint(addr, kind, WpType::READ);
            send_rsp("OK");
        } else if (ztype == Z_WP_RW) {
            bus_.add_watchpoint(addr, kind, WpType::RW);
            send_rsp("OK");
        } else {
            send_rsp(""); // unsupported type
        }
        break;
    }

    case 'z': {
        int      ztype = pkt[1] - '0';
        uint32_t addr  = 0;
        unsigned kind  = 0;
        std::sscanf(pkt.c_str() + 3, "%x,%x", &addr, &kind);
        if (kind == 0) kind = 4;

        if (ztype == Z_SW_BREAK || ztype == Z_HW_BREAK) {
            breakpoints_.erase(addr);
            send_rsp("OK");
        } else if (ztype == Z_WP_WRITE) {
            bus_.remove_watchpoint(addr, kind, WpType::WRITE);
            send_rsp("OK");
        } else if (ztype == Z_WP_READ) {
            bus_.remove_watchpoint(addr, kind, WpType::READ);
            send_rsp("OK");
        } else if (ztype == Z_WP_RW) {
            bus_.remove_watchpoint(addr, kind, WpType::RW);
            send_rsp("OK");
        } else {
            send_rsp("");
        }
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
            send_rsp("PacketSize=4000"
                     ";qXfer:features:read+"
                     ";qRegisterInfo+"
                     ";vContSupported+"
                     ";hwbreak+"   // Z1/z1 hardware breakpoints
                     ";swbreak+"   // Z0/z0 software breakpoints (stop reason)
                     );

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
    socket_t client = ::accept(static_cast<socket_t>(listen_fd_), nullptr, nullptr);
    if (client == kInvalidSock) return;
    std::fprintf(stderr, "GDB RSP: client connected\n");
    if (async_mode_) async_client_active_.store(true);
    no_ack_mode_ = false;
    breakpoints_.clear();
    bus_.clear_watchpoints();
    wp_hit_ = false;
    cpu_.set_strict(true);   // escalate soft violations to faults while debugger is attached

    // Install watchpoint callback: record first hit, then halt the CPU so
    // run() can inspect the state and return the appropriate stop reply.
    bus_.set_wp_callback([this](const Watchpoint& wp, uint32_t addr,
                                uint32_t /*val*/, int /*width*/, bool /*is_write*/) {
        if (!wp_hit_) {
            wp_hit_      = true;
            wp_hit_addr_ = addr;
            switch (wp.type) {
            case WpType::WRITE: wp_hit_ztype_ = Z_WP_WRITE; break;
            case WpType::READ:  wp_hit_ztype_ = Z_WP_READ;  break;
            case WpType::RW:    wp_hit_ztype_ = Z_WP_RW;    break;
            }
            cpu_.state.in_halt = true; // stop the run() loop after this step
        }
    });

    std::string buf;
    char tmp[256];
    while (true) {
        ssize_t n = ::recv(client, tmp, sock_len(sizeof(tmp) - 1), 0);
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
                ::send(client, stop.c_str(), sock_len(stop.size()), 0);
                continue;
            }
            if (buf[0] == '$') {
                std::size_t hash = buf.find('#', 1);
                if (hash == std::string::npos || hash + 2 >= buf.size()) break; // incomplete
                std::string pkt_data = buf.substr(1, hash - 1);
                if (!no_ack_mode_)
                    ::send(client, "+", 1, 0);
                if (debug_) std::fprintf(stderr, "GDB RSP: << %s\n", pkt_data.c_str());
                handle_packet(static_cast<intptr_t>(client), pkt_data);
                buf.erase(0, hash + 3);
                continue;
            }
            buf.erase(0, 1); // discard unknown
        }
    }
    close_sock(client);
    if (async_mode_) {
        async_client_active_.store(false);
        // If the RSP thread is blocked in run() waiting for a stop notification,
        // unblock it so it can observe the disconnection.
        notify_async_stopped("T05thread:1;");
    }
    cpu_.set_strict(false);      // back to warning mode after debugger disconnects
    bus_.clear_watchpoints();    // remove any watchpoints left by the client
    bus_.set_wp_callback({});    // restore no-op callback
    std::fprintf(stderr, "GDB RSP: client disconnected\n");
}
