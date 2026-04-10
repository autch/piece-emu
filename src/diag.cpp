#include "diag.hpp"
#include <cstdio>

void StderrDiagSink::report(const DiagEvent& ev) {
    const char* level_str = (ev.level == DiagLevel::Fault) ? "FAULT" : "WARNING";
    std::fprintf(stderr, "\n--- %s [%s] at 0x%06X: %s ---\n",
                 level_str, ev.category, ev.pc, ev.detail.c_str());
    if (!ev.context.empty())
        std::fprintf(stderr, "%s", ev.context.c_str());
    std::fprintf(stderr, "---\n\n");
}
