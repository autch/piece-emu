#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <set>

class Bus;
class Cpu;
class GdbRsp;
class AudioOutput;
class AudioLog;

struct PiecePeripherals;
struct Config;

// ---------------------------------------------------------------------------
// CpuRunner — drives the CPU emulation loop in a dedicated thread.
//
// All SDL calls stay on the main thread; only SDL_GetTicksNS / SDL_DelayNS
// are used here (safe from any thread).
//
// The struct holds references to state owned by main(); construct it with
// aggregate initialisation and call run() on the CPU thread.
// ---------------------------------------------------------------------------
struct CpuRunner {
    Bus&                       bus;
    Cpu&                       cpu;
    PiecePeripherals&          periph;
    const Config&              cfg;
    uint64_t&                  total_cycles;
    const std::set<uint32_t>&  break_addrs;
    std::unique_ptr<GdbRsp>&   gdb_rsp;
    std::atomic<bool>&         quit_flag;
    std::atomic<uint16_t>&     shared_buttons;
    AudioOutput*               audio_out = nullptr; // optional; pacing hint
    AudioLog*                  audio_log = nullptr; // optional; diagnostic log

    // Reset request (main thread → CPU thread):
    //   0 = none, 1 = hot start, 2 = cold start.
    // Main thread stores a non-zero value; CPU thread CAS-clears it at
    // the outer-loop boundary and performs the reset.
    std::atomic<int>           reset_request{0};

    // Called from main thread to request a reset.
    void request_reset(bool cold) {
        reset_request.store(cold ? 2 : 1, std::memory_order_release);
    }

    void run();
};
