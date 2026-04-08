#pragma once
#include <cstdint>
#include <string>

class Bus;
class Cpu;

// GDB Remote Serial Protocol server (skeleton).
// Listens on TCP port, speaks RSP, allows GDB to connect and debug.
//
// Register packet order (matches MAME state_add order):
//   R0–R15 (16 × 4 bytes), PC (4), SP (4), PSR (4), ALR (4), AHR (4)
//
// P0 stub: opens port but always sends "S05" (SIGTRAP) to indicate halt.
// Implement fully in a later pass.
class GdbRsp {
public:
    GdbRsp(Bus& bus, Cpu& cpu, uint16_t port = 1234);
    ~GdbRsp();

    // Accept one connection and serve it until the client disconnects.
    // Blocking; call from a separate thread or after the emulator halts.
    void serve();

private:
    Bus& bus_;
    Cpu& cpu_;
    uint16_t port_;
    int listen_fd_ = -1;

    void handle_packet(int fd, const std::string& packet);
    std::string reg_read_all();
    void        reg_write_all(const std::string& hex);
};
