#include "audio_log.hpp"

#include <SDL3/SDL.h>
#include <chrono>

AudioLog::~AudioLog() { close(); }

bool AudioLog::open(const std::string& path)
{
    close();
    f_ = std::fopen(path.c_str(), "w");
    if (!f_) return false;

    std::fprintf(f_, "# event\twall_ns\tcpu_cyc\tf1\tf2\tf3\tf4\n");
    std::fprintf(f_, "# PUSH: cnt, avail_after, dropped_total, -\n");
    std::fprintf(f_, "# PULL: want, got, avail_before, sdl_queued\n");
    std::fprintf(f_, "# PACE: mode(0=audio,1=wall), avail, sleep_ns, -\n");
    std::fflush(f_);

    head_ = 0;
    tail_ = 0;
    dropped_.store(0);
    stop_.store(false);
    flush_thr_ = std::thread([this] { flush_loop(); });
    return true;
}

void AudioLog::close()
{
    if (!f_) return;

    stop_.store(true);
    if (flush_thr_.joinable()) flush_thr_.join();

    // Final drain of anything left in the ring.
    std::lock_guard<std::mutex> g(mu_);
    drain_locked();

    std::uint64_t d = dropped_.load();
    if (d > 0) std::fprintf(f_, "# dropped %llu events\n", (unsigned long long)d);
    std::fclose(f_);
    f_ = nullptr;
}

void AudioLog::push_entry(Entry e)
{
    if (!f_) return;
    std::lock_guard<std::mutex> g(mu_);
    std::size_t next = (head_ + 1) & (N - 1);
    if (next == tail_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ring_[head_] = e;
    head_ = next;
}

void AudioLog::log_push(std::uint64_t cpu_cyc, std::int64_t cnt,
                        std::int64_t avail_after, std::int64_t dropped_total)
{
    if (!f_) return;
    Entry e{K_PUSH, SDL_GetTicksNS(), cpu_cyc,
            cnt, avail_after, dropped_total, 0};
    push_entry(e);
}

void AudioLog::log_pull(std::int64_t want, std::int64_t got,
                        std::int64_t avail_before, std::int64_t sdl_queued)
{
    if (!f_) return;
    Entry e{K_PULL, SDL_GetTicksNS(), 0,
            want, got, avail_before, sdl_queued};
    push_entry(e);
}

void AudioLog::log_pace(std::uint64_t cpu_cyc, std::int64_t mode,
                        std::int64_t avail, std::int64_t sleep_ns)
{
    if (!f_) return;
    Entry e{K_PACE, SDL_GetTicksNS(), cpu_cyc,
            mode, avail, sleep_ns, 0};
    push_entry(e);
}

void AudioLog::write_entry(FILE* f, const Entry& e)
{
    const char* name = "?";
    switch (e.kind) {
        case K_PUSH: name = "PUSH"; break;
        case K_PULL: name = "PULL"; break;
        case K_PACE: name = "PACE"; break;
    }
    std::fprintf(f, "%s\t%llu\t%llu\t%lld\t%lld\t%lld\t%lld\n",
        name,
        (unsigned long long)e.wall_ns,
        (unsigned long long)e.cpu_cyc,
        (long long)e.f1, (long long)e.f2,
        (long long)e.f3, (long long)e.f4);
}

void AudioLog::drain_locked()
{
    while (tail_ != head_) {
        write_entry(f_, ring_[tail_]);
        tail_ = (tail_ + 1) & (N - 1);
    }
}

void AudioLog::flush_loop()
{
    using namespace std::chrono_literals;
    while (!stop_.load(std::memory_order_relaxed)) {
        {
            std::lock_guard<std::mutex> g(mu_);
            drain_locked();
        }
        std::fflush(f_);
        std::this_thread::sleep_for(50ms);
    }
}
