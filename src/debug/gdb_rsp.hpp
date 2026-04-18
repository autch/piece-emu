#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

// Z/z packet type codes (GDB RSP spec §E.1):
//   0 = software breakpoint
//   1 = hardware breakpoint
//   2 = write watchpoint
//   3 = read watchpoint
//   4 = access (read/write) watchpoint
static constexpr int Z_SW_BREAK = 0;
static constexpr int Z_HW_BREAK = 1;
static constexpr int Z_WP_WRITE = 2;
static constexpr int Z_WP_READ  = 3;
static constexpr int Z_WP_RW    = 4;

class Bus;
class Cpu;

// GDB Remote Serial Protocol server.
// Listens on TCP port, speaks RSP, allows GDB/LLDB to connect and debug.
//
// Register packet order:
//   R0–R15 (16 × 4 bytes LE), SP (4), PC (4), PSR (4), ALR (4), AHR (4)
//   Total: 21 × 4 = 84 bytes = 168 hex chars
//
// Supports both GDB and LLDB clients.
// LLDB-specific extensions: QStartNoAckMode, qHostInfo, qProcessInfo,
//   qRegisterInfo<n>, QThreadSuffixSupported, QListThreadsInStopReply,
//   vCont, p<n>, P<n>=<val>
class GdbRsp {
public:
    GdbRsp(Bus& bus, Cpu& cpu, uint16_t port = 1234, bool debug = false);
    ~GdbRsp();

    // Accept one connection and serve it until the client disconnects.
    // Blocking; call from a separate thread or after the emulator halts.
    void serve();

    // ---- Async server mode (piece-emu-system / SDL frontend) ----------------
    //
    // serve_async() starts serve() in a background thread and returns
    // immediately.  CPU stepping is performed by the SDL main loop.
    //
    // Main-loop integration:
    //
    //   bool take_async_run_cmd(bool* single_out)
    //     Returns true when the GDB/LLDB client issued 'c'/'s'.
    //     *single_out = true → step one instruction; false → run until stop.
    //     Resets in_halt and watchpoint hit state as a side-effect.
    //
    //   void notify_async_stopped(std::string reply)
    //     Call after the CPU stops (breakpoint, halt, fault, watchpoint).
    //     Passes the RSP stop reply to the waiting RSP thread.
    //
    //   std::string make_async_stop_reply()
    //     Builds the RSP stop reply from the current CPU / watchpoint state.
    //     Call this to produce the argument for notify_async_stopped().
    //
    //   bool has_breakpoint(uint32_t addr)
    //     Returns true if a software/hardware breakpoint is set at addr.
    //     Needed by the main loop's continue-until-breakpoint check.
    //
    //   bool has_async_client()
    //     True while a GDB/LLDB client is connected in async mode.

    void serve_async();

    bool        has_async_client()    const { return async_client_active_.load(); }
    bool        take_async_run_cmd(bool* single_out);
    void        notify_async_stopped(std::string reply);
    std::string make_async_stop_reply() const;
    bool        has_breakpoint(uint32_t addr) const { return breakpoints_.count(addr) > 0; }

private:
    Bus& bus_;
    Cpu& cpu_;
    uint16_t port_;
    // Stored as intptr_t so that either a POSIX int fd or a Windows SOCKET
    // (UINT_PTR; may exceed int range) fits without truncation. INVALID_SOCKET
    // on Windows is all-bits-set, which round-trips through intptr_t as -1,
    // so the `< 0` sentinel check works on both platforms.
    intptr_t listen_fd_ = -1;
    bool no_ack_mode_ = false;
    bool debug_       = false;
    std::unordered_set<uint32_t> breakpoints_;

    // Watchpoint hit state: set by the Bus callback inside cpu_.step(),
    // read by run() after the step returns.
    bool     wp_hit_       = false; // true iff a watchpoint fired this step
    uint32_t wp_hit_addr_  = 0;     // physical address that triggered it
    int      wp_hit_ztype_ = 2;     // Z-packet type: 2=write, 3=read, 4=rw

    // fd is an intptr_t for the same portability reason as listen_fd_ —
    // callers cast to/from the platform socket type in gdb_rsp.cpp.
    void handle_packet(intptr_t fd, const std::string& packet);

    // Run the CPU: single=true → one instruction, false → until halt/fault/breakpoint.
    // Returns the RSP stop reply string (e.g. "T05thread:1;").
    std::string run(bool single);

    // ---- async state --------------------------------------------------------
    bool              async_mode_         = false;
    std::atomic<bool> async_client_active_{false};
    // Run-command handshake (protected by async_mutex_ except async_run_cmd_):
    //   RSP thread sets async_run_cmd_ (1=step, 2=continue), then blocks on
    //   async_stopped_cv_ until main loop calls notify_async_stopped().
    std::atomic<int>        async_run_cmd_{0};     // 0=none 1=step 2=continue
    std::mutex              async_mutex_;
    std::condition_variable async_stopped_cv_;
    bool        async_stopped_   = false;
    std::string async_stop_reply_;
    std::thread async_thread_;

    // Register access helpers
    std::string reg_read_all();
    void        reg_write_all(const std::string& hex);
    std::string reg_read_n(int n);
    void        reg_write_n(int n, uint32_t val);
    std::string reg_info(int n);   // qRegisterInfo<n> response

    std::string handle_qXfer(const std::string& pkt);
};
