#include "peripheral_sound.hpp"
#include "bus.hpp"
#include "peripheral_hsdma.hpp"
#include "peripheral_intc.hpp"
#include "peripheral_clkctl.hpp"
#include "peripheral_t16.hpp"

#include <algorithm>
#include <cstdio>

void Sound::attach(Bus& bus, Hsdma& hsdma, InterruptController& intc,
                   ClockControl& clk,
                   std::function<std::uint64_t()> get_cycles,
                   const Timer16bit* t16_ch1)
{
    bus_        = &bus;
    hsdma_      = &hsdma;
    intc_       = &intc;
    clk_        = &clk;
    get_cycles_ = std::move(get_cycles);
    t16ch1_     = t16_ch1;

    hsdma.on_ch1_start = [this](Bus& /*b*/, uint32_t sadr, uint32_t cnt) {
        handle_ch1_start(sadr, cnt);
    };
}

std::uint16_t Sound::current_pwmc() const
{
    if (t16ch1_) {
        uint16_t v = t16ch1_->cra();
        if (v != 0) return v;
    }
    return PWMC_DEFAULT;
}

void Sound::handle_ch1_start(uint32_t sadr, uint32_t cnt)
{
    if (!bus_ || cnt == 0) return;

    // Sample format (matches piemu/iomem.c):
    //   Input  PWM duty, range 0 .. PWMC .. 2*PWMC (centre = PWMC).
    //   Output signed 16-bit PCM, full int16 span.
    const int32_t pwmc = static_cast<int32_t>(current_pwmc());
    if (pwmc <= 0) return;

    uint64_t now_cyc = get_cycles_ ? get_cycles_() : 0;
    last_push_cycle_.store(now_cyc, std::memory_order_relaxed);

    for (uint32_t i = 0; i < cnt; i++) {
        int32_t v = static_cast<int16_t>(bus_->read16(sadr + i * 2u));
        v -= pwmc;
        int64_t s = (static_cast<int64_t>(v) << 16) / pwmc;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        if (!try_push(static_cast<int16_t>(s)))
            drop_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Schedule completion IRQ at +cnt * cycles_per_sample.  At 24 MHz with
    // pwmc=750 this is 128*750 = 96000 cycles = 4 ms emulated = 4 ms wall
    // (sync_realtime enforces emu-time == wall-time on average).
    uint32_t clk_hz = clk_ ? clk_->cpu_clock_hz() : 24'000'000u;
    uint64_t delta = static_cast<uint64_t>(cnt)
                   * static_cast<uint64_t>(clk_hz)
                   / static_cast<uint64_t>(SAMPLE_RATE);
    uint64_t now = now_cyc;
    complete_cycle_ = now + delta;
    pending_        = true;

    if (trace_ && irq_count_ < 8) {
        std::fprintf(stderr, "[SND] ch1_start sadr=0x%08x cnt=%u now=%llu done=%llu\n",
            sadr, cnt, (unsigned long long)now, (unsigned long long)complete_cycle_);
    }

    if (on_push) {
        on_push(now_cyc, static_cast<std::size_t>(cnt), available(),
                drop_count_.load(std::memory_order_relaxed));
    }
}

void Sound::tick(uint64_t cycles)
{
    if (!pending_)            return;
    if (cycles < complete_cycle_) return;

    pending_ = false;
    if (hsdma_) hsdma_->finish_ch1();
    if (intc_)  intc_->raise(InterruptController::IrqSource::HSDMA1, 1);

    if (trace_ && ++irq_count_ <= 16) {
        std::fprintf(stderr, "[SND] fire-irq #%llu cycles=%llu\n",
            (unsigned long long)irq_count_,
            (unsigned long long)cycles);
    }
}

bool Sound::try_push(std::int16_t sample)
{
    const std::size_t head = ring_head_.load(std::memory_order_relaxed);
    const std::size_t tail = ring_tail_.load(std::memory_order_acquire);
    const std::size_t next = (head + 1) & (RING_SIZE - 1);
    if (next == tail) return false;
    ring_[head] = sample;
    ring_head_.store(next, std::memory_order_release);
    return true;
}

std::size_t Sound::pop(std::int16_t* dst, std::size_t n)
{
    std::size_t head = ring_head_.load(std::memory_order_acquire);
    std::size_t tail = ring_tail_.load(std::memory_order_relaxed);
    std::size_t popped = 0;
    while (popped < n && tail != head) {
        dst[popped++] = ring_[tail];
        tail = (tail + 1) & (RING_SIZE - 1);
    }
    ring_tail_.store(tail, std::memory_order_release);

    if (trace_) {
        uint64_t c = pop_count_.fetch_add(1) + 1;
        if (c <= 8 || (c % 128) == 0) {
            std::fprintf(stderr, "[SND] pop #%llu want=%zu got=%zu avail=%zu\n",
                (unsigned long long)c, n, popped, available());
        }
    }
    return popped;
}

std::size_t Sound::available() const
{
    const std::size_t head = ring_head_.load(std::memory_order_acquire);
    const std::size_t tail = ring_tail_.load(std::memory_order_acquire);
    return (head - tail) & (RING_SIZE - 1);
}
