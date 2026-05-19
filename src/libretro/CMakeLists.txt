# piece_libretro — libretro core for piece-emu.
#
# Native builds produce a SHARED library (.so / .dll / .dylib) loaded by
# RetroArch at runtime.  Emscripten builds produce a STATIC archive (.a)
# that RetroArch Web links into its single retroarch.wasm; -fPIC and
# visibility=hidden are irrelevant in that mode, and SHARED is not how
# RetroArch's web port consumes cores.
#
# Source files from src/core, src/soc, src/board, src/debug, src/system are
# duplicate-compiled here with -fPIC (native only).  We deliberately do NOT
# reuse the existing piece_core / piece_soc / piece_board STATIC targets,
# because flipping POSITION_INDEPENDENT_CODE on those would propagate -fPIC
# to every other consumer (piece-emu, piece-emu-system, gtest binaries) for
# no benefit.  Duplicate-compile is justified for libretro: the .so is the
# only consumer that needs PIC and the build cost is small.

set(PIECE_LIBRETRO_SOURCES
    # libretro entry points (this directory)
    piece_libretro.cpp

    # core
    ${CMAKE_SOURCE_DIR}/core/bus.cpp
    ${CMAKE_SOURCE_DIR}/core/cpu_core.cpp
    ${CMAKE_SOURCE_DIR}/core/cpu_class0.cpp
    ${CMAKE_SOURCE_DIR}/core/cpu_class1.cpp
    ${CMAKE_SOURCE_DIR}/core/cpu_class2.cpp
    ${CMAKE_SOURCE_DIR}/core/cpu_class3.cpp
    ${CMAKE_SOURCE_DIR}/core/cpu_class4.cpp
    ${CMAKE_SOURCE_DIR}/core/cpu_class5.cpp
    ${CMAKE_SOURCE_DIR}/core/cpu_class6.cpp
    ${CMAKE_SOURCE_DIR}/core/cpu_dispatch.cpp
    ${CMAKE_SOURCE_DIR}/core/cpu_disasm.cpp
    ${CMAKE_SOURCE_DIR}/core/diag.cpp

    # soc
    ${CMAKE_SOURCE_DIR}/soc/peripheral_intc.cpp
    ${CMAKE_SOURCE_DIR}/soc/peripheral_clkctl.cpp
    ${CMAKE_SOURCE_DIR}/soc/peripheral_t8.cpp
    ${CMAKE_SOURCE_DIR}/soc/peripheral_t16.cpp
    ${CMAKE_SOURCE_DIR}/soc/peripheral_portctrl.cpp
    ${CMAKE_SOURCE_DIR}/soc/peripheral_bcu_area.cpp
    ${CMAKE_SOURCE_DIR}/soc/peripheral_wdt.cpp
    ${CMAKE_SOURCE_DIR}/soc/peripheral_rtc.cpp
    ${CMAKE_SOURCE_DIR}/soc/peripheral_hsdma.cpp
    ${CMAKE_SOURCE_DIR}/soc/peripheral_sif3.cpp
    ${CMAKE_SOURCE_DIR}/soc/peripheral_sound.cpp

    # board
    ${CMAKE_SOURCE_DIR}/board/s6b0741.cpp
    ${CMAKE_SOURCE_DIR}/board/flash_sst39vf.cpp

    # blob-based PFI loader
    ${CMAKE_SOURCE_DIR}/debug/pfi_loader_blob.cpp

    # peripherals aggregate (vcpkg-free)
    ${CMAKE_SOURCE_DIR}/system/piece_peripherals.cpp
)

if(EMSCRIPTEN)
    add_library(piece_libretro STATIC ${PIECE_LIBRETRO_SOURCES})
    # Static archive: -fPIC and visibility hiding don't apply.  RetroArch
    # Web's build system pulls this archive into retroarch.wasm via
    # STATIC_LIBS.  Modern emcc generates wasm-object archives but the .bc
    # extension is the long-standing RetroArch convention for emscripten
    # core artifacts, so downstream Makefile rules pattern-match on it.
    set_target_properties(piece_libretro PROPERTIES
        PREFIX ""                       # output: piece_libretro.a (no "lib")
        OUTPUT_NAME piece_libretro
    )
else()
    add_library(piece_libretro SHARED ${PIECE_LIBRETRO_SOURCES})
    set_target_properties(piece_libretro PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        PREFIX ""                       # output: piece_libretro.so (no "lib")
        OUTPUT_NAME piece_libretro
        CXX_VISIBILITY_PRESET hidden     # only retro_* symbols exported
        C_VISIBILITY_PRESET   hidden
    )
endif()

target_include_directories(piece_libretro PRIVATE
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/soc
    ${CMAKE_SOURCE_DIR}/board
    ${CMAKE_SOURCE_DIR}/debug
    ${CMAKE_SOURCE_DIR}/system
    ${CMAKE_CURRENT_SOURCE_DIR}         # libretro.h is right here
)

target_link_libraries(piece_libretro PRIVATE piece_pfi_format)

# Copy piece_libretro.info next to the .so/.dll/.a so the build directory is
# directly drop-in for testing.  RetroArch reads this file from its info/
# directory at runtime; users still need to install it there separately for
# menu UI integration ("Load Content" file picker filtering).
add_custom_command(TARGET piece_libretro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/piece_libretro.info
        $<TARGET_FILE_DIR:piece_libretro>/piece_libretro.info
)

message(STATUS "piece_libretro target enabled")
