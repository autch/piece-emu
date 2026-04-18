# host/ — SDL3 host interface
#
# Sources:
#   lcd_renderer.cpp   — S6B0741 framebuffer → SDL3 texture + event polling
#
# Planned (P2-4):
#   audio_out.cpp      — Timer PWM samples → SDL3 Audio
#
# SDL3 is found by the top-level CMakeLists.txt.  This file builds a STATIC
# library when SDL3 is present, or an empty INTERFACE stub otherwise.

if(SDL3_FOUND)
    add_library(piece_host STATIC
        lcd_renderer.cpp
        audio_output.cpp
        audio_log.cpp
    )
    target_include_directories(piece_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    # piece_soc is required publicly: audio_output.hpp references Sound.
    target_link_libraries(piece_host PUBLIC piece_board piece_soc SDL3::SDL3)
else()
    # Stub target so other targets can always link against piece_host.
    add_library(piece_host INTERFACE)
endif()
