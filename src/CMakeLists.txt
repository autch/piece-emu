cmake_minimum_required(VERSION 3.20)
project(piece_emu CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# if gcc or clang, enable all warnings and pedantic mode
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
# msvc
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_compile_options(/W4 /permissive- /utf-8)
endif()

# Keep frame pointers in RelWithDebInfo so that profilers (perf, Instruments)
# can unwind call stacks through hot emulation loops without DWARF unwinding.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options($<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer>)
else()
    # MSVC doesn't omit frame pointers by default, so no need for a special flag.
endif()

# Enable LTO for Release/RelWithDebInfo builds if the toolchain supports it.
# This allows bus.fetch16(), cpu.step(), and instruction handlers to be inlined
# across translation-unit boundaries, dramatically improving emulation speed.
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
if(_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    message(STATUS "LTO enabled for Release/RelWithDebInfo builds")
else()
    message(STATUS "LTO not available: ${_ipo_msg}")
endif()

# ---------------------------------------------------------------------------
# Dependencies via vcpkg (toolchain injected by -DCMAKE_TOOLCHAIN_FILE=...)
# ---------------------------------------------------------------------------
find_package(GTest QUIET)
find_package(CLI11 CONFIG REQUIRED)
find_package(SDL3 CONFIG)   # optional; enables piece-emu-system + piece_host

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
add_subdirectory(tools)

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

    # Split into two binaries along the library boundary.  Each binary lists
    # ~60–100 tests instead of 161, which keeps `gtest_discover_tests`'
    # per-test static-init / startup cost small enough to avoid the Windows
    # stack-overflow we hit with a single monolithic binary.
    #   piece_core_tests — CPU / BCU / EXT / PSR / shift / disasm
    #   piece_soc_tests  — on-chip peripherals (INTC / T8 / T16 / PortCtrl /
    #                      RTC / SIF3 / Sound)
    add_executable(piece_core_tests
        tests/unit/test_ext_imm.cpp
        tests/unit/test_psr_flags.cpp
        tests/unit/test_shift_decode.cpp
        tests/unit/test_disasm.cpp
        tests/unit/test_bcu.cpp
        tests/unit/test_cpu_instructions.cpp
    )
    target_link_libraries(piece_core_tests PRIVATE
        piece_core
        GTest::gtest_main
    )

    add_executable(piece_soc_tests
        tests/unit/test_peripheral_intc.cpp
        tests/unit/test_peripheral_t8.cpp
        tests/unit/test_peripheral_t16.cpp
        tests/unit/test_peripheral_portctrl.cpp
        tests/unit/test_peripheral_rtc.cpp
        tests/unit/test_peripheral_sif3.cpp
        tests/unit/test_peripheral_sound.cpp
    )
    target_link_libraries(piece_soc_tests PRIVATE
        piece_soc
        piece_core
        GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(piece_core_tests)
    gtest_discover_tests(piece_soc_tests)
else()
    message(WARNING "Google Test not found — skipping unit tests (piece_unit_tests). "
                    "Run cmake with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()

# ---------------------------------------------------------------------------
# piece-emu-system — full-system frontend (SDL3 display + button input)
# Requires SDL3.  Enable by building with -DVCPKG_MANIFEST_FEATURES=system.
# ---------------------------------------------------------------------------
if(SDL3_FOUND)
    # Platform-specific joystick layout.  Set at configure time to bake in the
    # correct button-index mapping for the target device.
    # Supported values: "" (generic/gamepad only), "uconsole" (ClockworkPi uConsole).
    set(PIECE_PLATFORM "" CACHE STRING
        "Target platform for raw joystick button layout (e.g. uconsole)")

    add_executable(piece-emu-system
        system_main.cpp
        system/button_input.cpp
        system/cli_config.cpp
        system/piece_peripherals.cpp
        system/cpu_runner.cpp
    )
    target_include_directories(piece-emu-system PRIVATE system)
    target_link_libraries(piece-emu-system PRIVATE
        piece_host
        piece_board
        piece_soc
        piece_debug
        piece_core
        CLI11::CLI11
    )

    if(PIECE_PLATFORM STREQUAL "uconsole")
        target_compile_definitions(piece-emu-system PRIVATE PIECE_PLATFORM_UCONSOLE)
        message(STATUS "piece-emu-system: platform=uconsole (raw joystick input enabled)")
    elseif(NOT PIECE_PLATFORM STREQUAL "")
        message(WARNING "piece-emu-system: unknown PIECE_PLATFORM '${PIECE_PLATFORM}' — ignored")
    endif()

    message(STATUS "piece-emu-system target enabled")
else()
    message(STATUS "SDL3 not found — piece-emu-system target skipped")
endif()
