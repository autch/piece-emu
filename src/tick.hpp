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
    virtual ~ITickable() = default;
};
