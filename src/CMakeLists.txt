cmake_minimum_required(VERSION 3.20)
project(piece_emu CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Dependencies via vcpkg (toolchain injected by -DCMAKE_TOOLCHAIN_FILE=...)
# ---------------------------------------------------------------------------
find_package(GTest QUIET)
find_package(CLI11 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Library layers
#
#   piece_core   — S1C33000 CPU + BCU + Diag (no POSIX, no SDL)
#   piece_soc    — S1C33209 on-chip peripherals (→ piece_core)
#   piece_board  — P/ECE board-level external devices (→ piece_soc, INTERFACE only for now)
#   piece_debug  — ELF loader, semihosting, GDB RSP (POSIX) (→ piece_core)
#   piece_host   — SDL3 LCD/audio/input frontend (see host/CMakeLists.txt; stub when SDL3 absent)
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# piece_pfi_format — shared PFI/PFFS on-disk format definitions (pfi_format.h).
# Pure C header; safe to include from C and C++.
# Both src/tools (C) and src/debug (C++) link against this INTERFACE target.
# ---------------------------------------------------------------------------
add_library(piece_pfi_format INTERFACE)
target_include_directories(piece_pfi_format INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(core)
add_subdirectory(soc)
add_subdirectory(board)
add_subdirectory(debug)
add_subdirectory(host)

# ---------------------------------------------------------------------------
# piece-emu — bare-metal / headless CLI frontend (POSIX only, no SDL)
# ---------------------------------------------------------------------------
add_executable(piece-emu main.cpp)
target_link_libraries(piece-emu PRIVATE
    piece_debug
    piece_board
    piece_soc
    piece_core
    CLI11::CLI11
)

# ---------------------------------------------------------------------------
# Unit tests (Google Test via vcpkg)
# ---------------------------------------------------------------------------
if(GTest_FOUND)
    enable_testing()

    add_executable(piece_unit_tests
        tests/unit/test_ext_imm.cpp
        tests/unit/test_psr_flags.cpp
        tests/unit/test_shift_decode.cpp
        tests/unit/test_disasm.cpp
        tests/unit/test_bcu.cpp
        tests/unit/test_cpu_instructions.cpp
        tests/unit/test_peripheral_intc.cpp
        tests/unit/test_peripheral_t8.cpp
        tests/unit/test_peripheral_t16.cpp
        tests/unit/test_peripheral_portctrl.cpp
        tests/unit/test_peripheral_rtc.cpp
        tests/unit/test_peripheral_sif3.cpp
    )

    # Tests only need soc + core (no POSIX debug layer required).
    target_link_libraries(piece_unit_tests PRIVATE
        piece_soc
        piece_core
        GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(piece_unit_tests)
else()
    message(WARNING "Google Test not found — skipping unit tests (piece_unit_tests). "
                    "Run cmake with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()

# ---------------------------------------------------------------------------
# piece-emu-system — full-system frontend (future; requires SDL3)
# To enable: cmake with -DVCPKG_MANIFEST_FEATURES=system and uncomment below.
# ---------------------------------------------------------------------------
# add_executable(piece-emu-system system_main.cpp)
# target_link_libraries(piece-emu-system PRIVATE
#     piece_host
#     piece_board
#     piece_soc
#     piece_debug
#     piece_core
# )
