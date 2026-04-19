#include "peripheral_rtc.hpp"
#include "peripheral_clkctl.hpp"
#include "bus.hpp"

#include <chrono>
#include <ctime>

// ---------------------------------------------------------------------------
// Date helpers — base epoch is 2000-01-01 00:00:00 (local time).
// ---------------------------------------------------------------------------

namespace {

// Days from 2000-01-01 to start of given (yy>=2000, mm in [1..12], dd in [1..]).
// Uses the same calendar that pceTime SDK assumes (Gregorian leap rule).
int days_from_2000(int yy, int mm, int dd)
{
    static const int mtbl[12]  = {0,31,59,90,120,151,181,212,243,273,304,334};
    static const int mtblu[12] = {0,31,60,91,121,152,182,213,244,274,305,335};

    int y = yy - 2000;
    // Leap days completed strictly before year (2000+y).  Year 2000 is itself
    // leap, so for y>=1 we always have at least one completed leap day; the
    // closed-form (y+3)/4 gives the right count throughout 2000..2099.
    int day = y * 365 + (y + 3) / 4;
    bool leap = (y & 3) == 0;
    day += (leap ? mtblu[mm-1] : mtbl[mm-1]);
    day += (dd - 1);
    return day;
}

// Decompose a 16-bit "days since 2000-01-01" into (yy, mm, dd) like the kernel.
void days_to_ymd(int days, int& yy, int& mm, int& dd)
{
    static const int ytbl[4] = {366, 365, 365, 365}; // 2000=leap
    static const int mtbl[12]  = {0,31,59,90,120,151,181,212,243,273,304,334};
    static const int mtblu[12] = {0,31,60,91,121,152,182,213,244,274,305,335};

    int y = 2000;
    int n = days;
    int len = 0;
    for (; y < 2100; ++y) {
        len = ytbl[(y - 2000) & 3];
        if (n < len) break;
        n -= len;
    }
    yy = y;
    const int* mt = ((y - 2000) & 3) ? mtbl : mtblu;
    int m = 1;
    for (int i = 1; i < 12; ++i) {
        if (n < mt[i]) { m = i; break; }
        m = i + 1;
    }
    mm = m;
    dd = n - mt[m-1] + 1;
}

} // namespace

int64_t ClockTimer::host_seconds_since_2000()
{
    // Use local time so the on-screen RTC matches the user's wall clock.
    std::time_t now = std::time(nullptr);
    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &now);
#else
    localtime_r(&now, &tm_local);
#endif

    int yy = tm_local.tm_year + 1900;
    int mm = tm_local.tm_mon  + 1;
    int dd = tm_local.tm_mday;
    if (yy < 2000) yy = 2000; // clamp; pre-2000 isn't representable
    if (yy > 2099) yy = 2099;

    int64_t days = days_from_2000(yy, mm, dd);
    return days * 86400
         + tm_local.tm_hour * 3600
         + tm_local.tm_min  * 60
         + tm_local.tm_sec;
}

int64_t ClockTimer::current_seconds_since_2000() const
{
    if (frozen_) return frozen_sec_;
    return host_seconds_since_2000() + offset_sec_;
}

void ClockTimer::apply_set_to_offset()
{
    int64_t day = (static_cast<int64_t>(set_day_hi_) << 8) | set_day_lo_;
    int64_t set_sec = day * 86400
                    + static_cast<int64_t>(set_hh_) * 3600
                    + static_cast<int64_t>(set_mi_) * 60;
    // SEC is hardware-reset to 0 at the moment of RUN.
    offset_sec_ = set_sec - host_seconds_since_2000();
    prescaler_  = 0;
    frozen_     = false;
}

// ---------------------------------------------------------------------------
// Tick — drives the 8-bit prescaler at 256 Hz from the CPU clock.
// ---------------------------------------------------------------------------
void ClockTimer::tick(uint64_t cpu_cycles)
{
    if (cpu_cycles < next_prescaler_cycle_) return;
    if (rtcstop_ & 0x02) {
        // Stopped: don't advance prescaler, but keep wake schedule sane.
        next_prescaler_cycle_ = cpu_cycles + 1;
        return;
    }

    uint32_t cpu_hz = clk_ ? clk_->cpu_clock_hz() : 48'000'000u;
    uint64_t per    = prescaler_period(cpu_hz);
    if (per == 0) per = 1;

    while (cpu_cycles >= next_prescaler_cycle_) {
        next_prescaler_cycle_ += per;
        ++prescaler_; // 8-bit wrap is intentional (free-running counter)
    }
}

void ClockTimer::reset()
{
    rtcstop_  = 0x01; // running
    alarmc_   = 0;
    alarm_mi_ = alarm_hh_ = alarm_d_ = 0;
    prescaler_ = 0;
    set_mi_ = set_hh_ = set_day_lo_ = set_day_hi_ = 0;
    offset_sec_ = 0;
    frozen_ = false; frozen_sec_ = 0;
    next_prescaler_cycle_ = clk_ ? prescaler_period(clk_->cpu_clock_hz())
                                 : 24'000'000u / 256u;
}

// ---------------------------------------------------------------------------
// Bus attachment.  Layout (halfword pairs):
//
//   0x040150 : dummy (lo)              | rRTCSTOP  (hi @ 0x040151)
//   0x040152 : rRTCALMC (lo @ 0x152)   | rRTCSUB   (hi @ 0x040153)
//   0x040154 : rRTCSEC  (lo @ 0x154)   | rRTCMIN   (hi @ 0x040155)
//   0x040156 : rRTCHOUR (lo @ 0x156)   | rRTCDAYL  (hi @ 0x040157)
//   0x040158 : rRTCDAYH (lo @ 0x158)   | rRTCALMM  (hi @ 0x040159)
//   0x04015A : rRTCALMH (lo @ 0x15A)   | rRTCALMD  (hi @ 0x04015B)
// ---------------------------------------------------------------------------
void ClockTimer::attach(Bus& bus, InterruptController& /*intc*/,
                         const ClockControl& clk)
{
    clk_ = &clk;
    next_prescaler_cycle_ = prescaler_period(clk.cpu_clock_hz());

    // 0x040150: dummy (lo) + rRTCSTOP (hi)
    bus.register_io(RTC_BASE, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(rtcstop_) << 8;
        },
        [this](uint32_t addr, uint16_t v) {
            uint8_t written;
            if (addr & 1)
                written = static_cast<uint8_t>(v);
            else
                written = static_cast<uint8_t>(v >> 8);

            uint8_t prev = rtcstop_;
            rtcstop_ = written;

            if (written & 0x02) {
                // STOP transition: freeze the displayed time.
                if (!(prev & 0x02)) {
                    frozen_     = true;
                    frozen_sec_ = host_seconds_since_2000() + offset_sec_;
                }
                // Capture currently displayed fields so partial overwrites
                // (the kernel only writes mi/hh/day, not sec) keep the rest.
                int64_t s = frozen_sec_;
                int64_t day_total = s / 86400;
                int64_t tod       = s - day_total * 86400;
                set_hh_ = static_cast<uint8_t>(tod / 3600);
                set_mi_ = static_cast<uint8_t>((tod % 3600) / 60);
                set_day_lo_ = static_cast<uint8_t>(day_total & 0xFF);
                set_day_hi_ = static_cast<uint8_t>((day_total >> 8) & 0xFF);
            }
            if ((written & 0x01) && (prev & 0x02)) {
                // RUN after STOP: commit latched fields, sec resets to 0.
                apply_set_to_offset();
            }
        }
    });

    // 0x040152: rRTCALMC (lo) + rRTCSUB (hi, free-running prescaler)
    bus.register_io(RTC_BASE + 2, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(alarmc_) |
                   (static_cast<uint16_t>(prescaler_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            // rRTCSUB (hi) is read-only from software's perspective.
            if (addr & 1) {
                // Byte write to 0x040153: ignore (software can't set prescaler).
            } else {
                alarmc_ = static_cast<uint8_t>(v);
                // Halfword write upper byte ignored.
            }
        }
    });

    // 0x040154: rRTCSEC (lo) + rRTCMIN (hi)
    bus.register_io(RTC_BASE + 4, {
        [this](uint32_t) -> uint16_t {
            int64_t s = current_seconds_since_2000();
            int64_t tod = s - (s / 86400) * 86400;
            uint8_t sec = static_cast<uint8_t>(tod % 60);
            uint8_t min = static_cast<uint8_t>((tod / 60) % 60);
            return static_cast<uint16_t>(sec) |
                   (static_cast<uint16_t>(min) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            // SEC is hardware-reset on RUN; software writes are ignored.
            if (addr & 1)
                set_mi_ = static_cast<uint8_t>(v);
            else
                set_mi_ = static_cast<uint8_t>(v >> 8);
        }
    });

    // 0x040156: rRTCHOUR (lo) + rRTCDAYL (hi)
    bus.register_io(RTC_BASE + 6, {
        [this](uint32_t) -> uint16_t {
            int64_t s = current_seconds_since_2000();
            int64_t day_total = s / 86400;
            int64_t tod = s - day_total * 86400;
            uint8_t hour = static_cast<uint8_t>(tod / 3600);
            uint8_t day_lo = static_cast<uint8_t>(day_total & 0xFF);
            return static_cast<uint16_t>(hour) |
                   (static_cast<uint16_t>(day_lo) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) {
                set_day_lo_ = static_cast<uint8_t>(v);
            } else {
                set_hh_     = static_cast<uint8_t>(v);
                set_day_lo_ = static_cast<uint8_t>(v >> 8);
            }
        }
    });

    // 0x040158: rRTCDAYH (lo) + rRTCALMM (hi)
    bus.register_io(RTC_BASE + 8, {
        [this](uint32_t) -> uint16_t {
            int64_t s = current_seconds_since_2000();
            int64_t day_total = s / 86400;
            uint8_t day_hi = static_cast<uint8_t>((day_total >> 8) & 0xFF);
            return static_cast<uint16_t>(day_hi) |
                   (static_cast<uint16_t>(alarm_mi_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) {
                alarm_mi_ = static_cast<uint8_t>(v);
            } else {
                set_day_hi_ = static_cast<uint8_t>(v);
                alarm_mi_   = static_cast<uint8_t>(v >> 8);
            }
        }
    });

    // 0x04015A: rRTCALMH (lo) + rRTCALMD (hi)
    bus.register_io(RTC_BASE + 10, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(alarm_hh_) |
                   (static_cast<uint16_t>(alarm_d_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) {
                alarm_d_  = static_cast<uint8_t>(v);
            } else {
                alarm_hh_ = static_cast<uint8_t>(v);
                alarm_d_  = static_cast<uint8_t>(v >> 8);
            }
        }
    });
}

// Force-link helper for ymd helpers when they are otherwise unused (silences
// strict warnings under some configurations). Marked unused to avoid -Werror.
[[maybe_unused]] static void rtc_link_helpers()
{
    int yy=0, mm=0, dd=0;
    days_to_ymd(0, yy, mm, dd);
    (void)days_from_2000(2000,1,1);
}
