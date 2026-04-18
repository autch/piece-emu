#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

// ============================================================================
// AudioLog — lightweight event recorder for diagnosing audio pacing.
//
// Three event kinds are recorded with a shared timestamp (wall-clock ns):
//
//   PUSH  — Sound::handle_ch1_start fired (CPU thread).
//           f1=cnt, f2=avail_after, f3=dropped_total
//   PULL  — AudioOutput::audio_cb delivered samples to SDL (SDL thread).
//           f1=want, f2=got, f3=avail_before, f4=sdl_queued
//   PACE  — sync_realtime() decision point (CPU thread).
//           f1=mode (0=audio, 1=wall), f2=avail, f3=sleep_ns
//
// Writers push into a lock-guarded ring (few hundred Hz, mutex critical
// section < 1 µs).  A background thread drains the ring to the log file
// so the real-time threads never call fprintf.
// ============================================================================

class AudioLog {
public:
    ~AudioLog();

    // Open the log file and start the flush thread.  Returns true on success.
    // If already open, closes the previous file first.
    bool open(const std::string& path);

    // Stop and flush.  Safe to call multiple times.
    void close();

    bool is_open() const { return f_ != nullptr; }

    // Producers.  No-op when closed.  Thread-safe.
    void log_push(std::uint64_t cpu_cyc, std::int64_t cnt,
                  std::int64_t avail_after, std::int64_t dropped_total);
    void log_pull(std::int64_t want, std::int64_t got,
                  std::int64_t avail_before, std::int64_t sdl_queued);
    void log_pace(std::uint64_t cpu_cyc, std::int64_t mode,
                  std::int64_t avail, std::int64_t sleep_ns);

private:
    enum : std::uint8_t { K_PUSH = 0, K_PULL = 1, K_PACE = 2 };

    struct Entry {
        std::uint8_t  kind;
        std::uint64_t wall_ns;
        std::uint64_t cpu_cyc;
        std::int64_t  f1, f2, f3, f4;
    };

    static constexpr std::size_t N = 16384; // ~640 KB ring
    static_assert((N & (N - 1)) == 0, "N must be power of 2");

    FILE*                      f_    = nullptr;
    std::mutex                 mu_;
    Entry                      ring_[N]{};
    std::size_t                head_ = 0; // written by producers under mu_
    std::size_t                tail_ = 0; // written by flush thread only
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool>          stop_{false};
    std::thread                flush_thr_;

    void push_entry(Entry e);
    void flush_loop();
    void drain_locked();
    static void write_entry(FILE* f, const Entry& e);
};
