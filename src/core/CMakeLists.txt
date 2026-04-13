add_library(piece_core STATIC
    diag.cpp
    bus.cpp
    cpu_core.cpp
    cpu_class0.cpp
    cpu_class1.cpp
    cpu_class2.cpp
    cpu_class3.cpp
    cpu_class4.cpp
    cpu_class5.cpp
    cpu_class6.cpp
    cpu_dispatch.cpp
    cpu_disasm.cpp
)

# Expose src/core/ as the include root so consumers use #include "cpu.hpp" etc.
target_include_directories(piece_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
