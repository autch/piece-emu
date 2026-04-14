add_library(piece_debug STATIC
    elf_loader.cpp
    pfi_loader.cpp
    semihosting.cpp
    gdb_rsp.cpp
    gdb_rsp_regs.cpp
)

target_include_directories(piece_debug PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# PUBLIC: consumers inherit piece_core include dirs (bus.hpp, cpu.hpp, etc.)
target_link_libraries(piece_debug PUBLIC piece_core piece_pfi_format)
