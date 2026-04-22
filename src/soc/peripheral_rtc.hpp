#pragma once
#include <cstdint>

class Bus;
class ClockControl;
class InterruptController;

// ============================================================================
// ClockTimer — S1C33209 clock timer (RTC)
//
// Register map (byte addresses, IO base = 0x040000):
//   0x040151  rRTCSTOP   stop control (bit1=stop, bit0=run; writing 0x01
//                        starts the counter and resets sec/prescaler to 0)
//   0x040152  rRTCALMC   alarm control / mode
//   0x040153  rRTCSUB    8-bit free-running prescaler (256 Hz). The kernel
//                        reads it to derive 1/100s precision and to detect
//                        the 1 Hz edge (bit3 toggles at 32 Hz).
//   0x040154  rRTCSEC    seconds  (binary, 0..59)
//   0x040155  rRTCMIN    minutes  (binary, 0..59)
//   0x040156  rRTCHOUR   hours    (binary, 0..23)
//   0x040157  rRTCDAYL   days low byte  (16-bit total, days since 2000-01-01)
//   0x040158  rRTCDAYH   days high byte
//   0x040159  rRTCALMM   alarm minute
//   0x04015A  rRTCALMH   alarm hour
//   0x04015B  rRTCALMD   alarm day (low 5 bits)
//
// Time-source emulation:
//   Reads of SEC/MIN/HOUR/DAY return the *host PC* wall-clock time, optionally
//   offset by a per-instance delta. When the guest writes the date/time fields
//   and then asserts STOP=run (0x01), the offset is recomputed so that future
//   reads continue to advance from the just-written value.
//
//   Base epoch is 2000-01-01 00:00:00 (matches pceTime SDK convention).
//
//   The 8-bit prescaler at 0x040153 is driven from CPU cycles so that
//   GetSysClock() (which polls bit3) calibrates correctly regardless of host
//   clock skew.
//
// Alarm functionality is not implemented; alarm registers are storage-only.
// ============================================================================
class ClockTimer {
public:
    void attach(Bus& bus, InterruptController& intc, const ClockControl& clk);

    void tick(uint64_t cpu_cycles);

    // Returns the next cycle at which the prescaler advances.
    uint64_t next_wake_cycle() const { return next_prescaler_cycle_; }

    // Reset register + scheduling state; preserves attach-bound pointers.
    void reset();

    // Direct register access (for unit tests)
    uint8_t rtcsub()  const { return prescaler_; }
    uint8_t rtcstop() const { return rtcstop_; }
    int64_t offset_seconds() const { return offset_sec_; }

private:
    static constexpr uint32_t RTC_BASE = 0x040150; // halfword base (dummy lo)

    const ClockControl*  clk_  = nullptr;
    InterruptController* intc_ = nullptr;

    // Control / alarm storage
    uint8_t rtcstop_ = 0x01; // running by default
    uint8_t alarmc_  = 0;
    uint8_t alarm_mi_ = 0;
    uint8_t alarm_hh_ = 0;
    uint8_t alarm_d_  = 0;

    // Prescaler (free-running 8-bit at 256 Hz)
    uint8_t  prescaler_           = 0;
    uint64_t next_prescaler_cycle_ = 0;

    // Pending "set" values latched between STOP and RUN.
    // Initialised to "now" so reads work even before any write happens.
    uint8_t  set_mi_      = 0;
    uint8_t  set_hh_      = 0;
    uint8_t  set_day_lo_  = 0;
    uint8_t  set_day_hi_  = 0;

    // Offset in seconds: emulated_time = host_now + offset_sec_
    // (both expressed as seconds since 2000-01-01 00:00:00)
    int64_t offset_sec_ = 0;

    // Frozen time-since-2000 while STOPped (rtcstop_ bit1 set).
    bool    frozen_       = false;
    int64_t frozen_sec_   = 0;

    // ---- helpers ----------------------------------------------------------
    static int64_t host_seconds_since_2000();
    int64_t current_seconds_since_2000() const;
    void    apply_set_to_offset();

    uint64_t prescaler_period(uint32_t cpu_hz) const
    {
        // rTCD (bp[0x153]) ticks at 256 Hz — standard RTC prescaler fed
        // from OSC1 (32.768 kHz) through the chip's own divider chain.
        // bit7 toggles at 1 Hz (second mark), bit3 at 16 Hz (half-period
        // 31.25 ms — used by GetSysClock to time its T16 measurement).
        if (cpu_hz == 0) return 24'000'000u / 256u;
        return static_cast<uint64_t>(cpu_hz) / 256u;
    }
};
