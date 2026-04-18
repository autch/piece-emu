add_library(piece_debug STATIC
    elf_loader.cpp
    pfi_loader.cpp
    semihosting.cpp
    gdb_rsp.cpp
    gdb_rsp_regs.cpp
    debug_utils.cpp
)

target_include_directories(piece_debug PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# PUBLIC: consumers inherit piece_core include dirs (bus.hpp, cpu.hpp, etc.)
target_link_libraries(piece_debug PUBLIC piece_core piece_pfi_format)

# Winsock2 for gdb_rsp.cpp. MSVC/clang-cl also pick this up via
# `#pragma comment(lib, "ws2_32.lib")`, but MinGW ignores that pragma, so the
# explicit link keeps the debug layer buildable under every Windows toolchain.
if(WIN32)
    target_link_libraries(piece_debug PUBLIC ws2_32)
endif()
