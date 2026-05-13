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

# piece_imageio — SDL-independent PNG snapshot writer.
# Split out of piece_host so that the headless frontend (no SDL3) can save
# screenshots driven by its script.  Pure stb_image_write + chrono.
add_library(piece_imageio STATIC
    screenshot.cpp
)
target_include_directories(piece_imageio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/stb
)

if(SDL3_FOUND)
    add_library(piece_host STATIC
        lcd_renderer.cpp
        audio_output.cpp
        audio_log.cpp
    )
    target_include_directories(piece_host PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/stb
    )
    # piece_soc is required publicly: audio_output.hpp references Sound.
    # piece_imageio reaches the SDL frontend via piece_host's public link
    # so callers of piece_host (e.g. piece-emu-system) keep working with
    # no link-line changes.
    target_link_libraries(piece_host PUBLIC piece_board piece_soc piece_imageio SDL3::SDL3)
    set_target_properties(piece_host PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    # Stub target so other targets can always link against piece_host.
    add_library(piece_host INTERFACE)
endif()
