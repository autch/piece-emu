#pragma once
#include <cstdint>

// ============================================================================
// ITickable — interface for cycle-driven peripheral simulation
//
// tick() is called after every CPU step with the cumulative CPU cycle count.
// Implementations compute elapsed counts and fire interrupts as needed.
// ============================================================================
class ITickable {
public:
    virtual void tick(uint64_t cpu_cycles) = 0;

    // Returns the earliest CPU cycle at which this peripheral will raise an
    // interrupt. Returns UINT64_MAX if the peripheral is stopped or has no
    // pending event. Used by the main loop to fast-forward past HLT/SLEEP.
    virtual uint64_t next_wake_cycle() const = 0;

    virtual ~ITickable() = default;
};
