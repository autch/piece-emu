#pragma once
#include <cstdint>
#include <string>

// ============================================================================
// Diagnostic event system
//
// DiagSink is a pure interface; implement it to redirect diagnostics to GDB,
// a log file, a test harness, etc.  The default (StderrDiagSink) prints to
// stderr and is always available — Cpu and Bus point to it unless overridden.
//
// Events come from three sources today; the design is open to future devices:
//   CPU  — undefined opcode, jp.d %rb (hardware bug), ext before shift
//   Bus  — misaligned read/write (treated as Fault; CPU checks take_fault())
//   (future) LCD / audio / flash controllers
// ============================================================================

enum class DiagLevel { Warning, Fault };

struct DiagEvent {
    DiagLevel   level;
    const char* category;  // machine-readable tag, e.g. "undef", "jp_d_rb",
                           // "ext_shift", "misalign_read16"
    uint32_t    pc;        // address of the faulting instruction (0 if unknown)
    std::string detail;    // human-readable one-line description
    std::string context;   // optional multi-line dump (registers, etc.)
};

class DiagSink {
public:
    virtual ~DiagSink() = default;
    virtual void report(const DiagEvent& ev) = 0;
};

// Default implementation: prints to stderr.
// Warnings print one line; Faults print the context block too.
class StderrDiagSink : public DiagSink {
public:
    void report(const DiagEvent& ev) override;
};
