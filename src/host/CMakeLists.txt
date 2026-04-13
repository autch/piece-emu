# host/ — SDL3 host interface (future)
#
# Planned sources:
#   lcd_renderer.cpp   — S6B0741 framebuffer → SDL3 texture
#   audio_out.cpp      — Timer PWM samples  → SDL3 Audio
#   input_handler.cpp  — SDL3 events        → K/P port bits
#
# To enable: cmake with -DVCPKG_MANIFEST_FEATURES=system and uncomment below.

# find_package(SDL3 CONFIG REQUIRED)
#
# add_library(piece_host STATIC
#     lcd_renderer.cpp
#     audio_out.cpp
#     input_handler.cpp
# )
# target_include_directories(piece_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# target_link_libraries(piece_host PUBLIC piece_board piece_soc piece_core SDL3::SDL3)
