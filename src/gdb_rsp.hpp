#pragma once
#include <cstdint>
#include <string>

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

private:
    Bus& bus_;
    Cpu& cpu_;
    uint16_t port_;
    int listen_fd_ = -1;
    bool no_ack_mode_ = false;
    bool debug_       = false;

    void handle_packet(int fd, const std::string& packet);

    // Register access helpers
    std::string reg_read_all();
    void        reg_write_all(const std::string& hex);
    std::string reg_read_n(int n);
    void        reg_write_n(int n, uint32_t val);
    std::string reg_info(int n);   // qRegisterInfo<n> response

    std::string handle_qXfer(const std::string& pkt);
};
