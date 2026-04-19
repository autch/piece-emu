#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

class Bus;
class Hsdma;
class InterruptController;
class ClockControl;
class Timer16bit;

// ============================================================================
// Sound — P/ECE PWM audio emulation
//
// Model:
//   * On HSDMA Ch1 EN 0→1, read all `cnt` PWM samples from SADR in one
//     shot and push them into an SPSC ring buffer (PWM → int16 PCM).
//     This gives SDL a stable supply that matches its async pull pattern.
//   * Schedule the HSDMA Ch1 completion IRQ at +cnt*(cpu_hz/32000) CPU
//     cycles later — i.e. 4 ms of emulated time for a 128-sample frame,
//     which translates to 4 ms of wall time via the CPU loop's
//     sync_realtime pacing.
//   * Deliver the completion at IL=1, not PHSD1L=7 — the kernel drops
//     PSR.IL to 5 during make_xwave() (SET_SIL5); at PHSD1L=7 the ISR
//     re-enters itself under muslib/ADPCM workloads.  IL=1 prevents
//     re-entry.  INTC's poll()/level_override mechanism ensures the
//     pending IRQ still fires as soon as the kernel's reti drops IL
//     back below 1.
//
// Why read all samples up front (rather than drip-feed per cmp.B)?
//   The T16 Ch.1 is configured in SELFM=1 PWM mode; our T16 emulation
//   doesn't precisely model that mode's compare events.  Reading samples
//   synchronously with EN=1 sidesteps the T16 dependency while still
//   respecting the kernel's expected 4 ms completion cadence.  The
//   trade-off is that the kernel's make_xwave() overlaps with DMA
//   "playback" differently than on real HW, but since the buffers are
//   double-buffered (ping-pong) and make_xwave always writes to the
//   inactive half, the read content is still the previous iteration's
//   make_xwave output — which is correct.
// ============================================================================

class Sound {
public:
    // Attach to peripherals.  hsdma.on_ch1_start is hooked; must be called
    // after hsdma.attach(bus).  t16_ch1 is optional — used only to read
    // the current CRA value for sample-rate scaling (defaults to 750).
    // get_cycles returns the CPU's current cycle counter; used to
    // schedule the HSDMA Ch1 completion IRQ at "now + 4 ms".
    void attach(Bus& bus, Hsdma& hsdma, InterruptController& intc,
                ClockControl& clk,
                std::function<std::uint64_t()> get_cycles,
                const Timer16bit* t16_ch1 = nullptr);

    // Called once per CPU tick.  Delivers the scheduled HSDMA Ch1
    // completion IRQ when `cycles` catches up with complete_cycle_.
    void tick(uint64_t cycles);

    // Reset internal DMA/IRQ scheduling and ring buffer.  Preserves
    // attach-time pointers and user-assigned on_push callback.
    // Safe to call only when the SDL audio callback is paused or
    // guaranteed not to run (handshake in CpuRunner guarantees this).
    void reset();

    // Earliest CPU cycle at which tick() needs to fire an IRQ.
    uint64_t next_wake_cycle() const {
        return pending_ ? complete_cycle_ : UINT64_MAX;
    }

    // Pull up to `n` samples into `dst`.  Returns the number actually
    // popped.  Thread-safe (called from the SDL audio callback).
    std::size_t pop(std::int16_t* dst, std::size_t n);

    // Number of samples currently buffered.
    std::size_t available() const;

    // CPU cycle counter at the most recent successful ring push.
    // Returns 0 if the app has never produced audio.  Used by the audio-
    // clock pacing path to tell "app is producing audio" from "app is
    // silent but SDL is still pulling zeros".
    std::uint64_t last_push_cycle() const {
        return last_push_cycle_.load(std::memory_order_relaxed);
    }

    // Count of samples dropped because the ring buffer was full.
    std::uint64_t drop_count() const {
        return drop_count_.load(std::memory_order_relaxed);
    }

    // Native output rate of the emulator (32 kHz; matches PWM carrier).
    static constexpr int SAMPLE_RATE = 32000;

    // Enable --audio-trace stderr event log.
    void set_trace(bool v) { trace_ = v; }

    // Optional hook fired after every successful ch1_start burst.  Used
    // by the frontend to feed the audio-event log without introducing
    // a soc → host dependency.  Called from the CPU thread.
    //   cpu_cyc       — current CPU cycle counter
    //   cnt           — samples in this burst
    //   avail_after   — ring fill level after push
    //   dropped_total — cumulative drop count
    std::function<void(std::uint64_t cpu_cyc,
                       std::size_t   cnt,
                       std::size_t   avail_after,
                       std::uint64_t dropped_total)> on_push;

private:
    // Default T16 Ch.1 CRA value from snd.c (32 kHz @ 24 MHz).
    static constexpr std::uint16_t PWMC_DEFAULT = 750;

    // Ring buffer — 4096 samples = 128 ms @ 32 kHz.  Sized to absorb:
    //   * SDL's bursty pull pattern (~1365 samples every 42 ms, drawn
    //     as two back-to-back callbacks 15 µs apart)
    //   * emu-side production bursts between sync_realtime() ticks
    //   * a couple of frames of pacing slack without hitting the cap
    // Hitting the cap causes try_push() to drop samples — audible as
    // clicks/pops — so the ring must stay above TARGET_FILL + headroom.
    static constexpr std::size_t RING_SIZE = 4096;
    static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be power of 2");

    std::int16_t               ring_[RING_SIZE]{};
    std::atomic<std::size_t>   ring_head_{0}; // producer (CPU thread)
    std::atomic<std::size_t>   ring_tail_{0}; // consumer (SDL thread)
    std::atomic<std::uint64_t> drop_count_{0};
    std::atomic<std::uint64_t> last_push_cycle_{0};

    Bus*                 bus_    = nullptr;
    Hsdma*               hsdma_  = nullptr;
    InterruptController* intc_   = nullptr;
    ClockControl*        clk_    = nullptr;
    const Timer16bit*    t16ch1_ = nullptr;
    std::function<std::uint64_t()> get_cycles_;

    bool     pending_        = false;
    uint64_t complete_cycle_ = 0;

    // --audio-trace
    bool                       trace_     = false;
    std::uint64_t              irq_count_ = 0;
    std::atomic<std::uint64_t> pop_count_{0};

    void handle_ch1_start(uint32_t sadr, uint32_t cnt);
    bool try_push(std::int16_t sample);
    std::uint16_t current_pwmc() const;

};
