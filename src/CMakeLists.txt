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
# libpiece_core — shared emulation core (no external dependencies)
# ---------------------------------------------------------------------------
add_library(piece_core STATIC
    diag.cpp
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
    bus.cpp
    elf_loader.cpp
    semihosting.cpp
    gdb_rsp.cpp
    peripheral_intc.cpp
    peripheral_clkctl.cpp
    peripheral_t8.cpp
    peripheral_t16.cpp
    peripheral_portctrl.cpp
    peripheral_bcu_area.cpp
    peripheral_wdt.cpp
)

target_include_directories(piece_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# piece-emu — bare-metal / headless CLI frontend (POSIX only, no SDL)
# ---------------------------------------------------------------------------
add_executable(piece-emu
    main.cpp
)

target_link_libraries(piece-emu PRIVATE piece_core CLI11::CLI11)

# ---------------------------------------------------------------------------
# 第1層・第2層: C++ unit tests (Google Test via vcpkg)
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
    )

    target_link_libraries(piece_unit_tests PRIVATE piece_core GTest::gtest_main)
    target_include_directories(piece_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    include(GoogleTest)
    gtest_discover_tests(piece_unit_tests)
else()
    message(WARNING "Google Test not found — skipping unit tests (piece_unit_tests). "
                    "Run cmake with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()

# ---------------------------------------------------------------------------
# piece-emu-system — full-system / game frontend (future; requires SDL2 + imgui)
# To enable: cmake with -DVCPKG_MANIFEST_FEATURES=system and uncomment below.
# ---------------------------------------------------------------------------
# find_package(SDL2 CONFIG REQUIRED)
# find_package(imgui CONFIG REQUIRED)
#
# add_executable(piece-emu-system
#     system_main.cpp
#     frontend/lcd.cpp
#     frontend/sound.cpp
#     frontend/input.cpp
# )
# target_link_libraries(piece-emu-system PRIVATE
#     piece_core
#     SDL2::SDL2
#     imgui::imgui
# )
