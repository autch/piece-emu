# board/ — P/ECE board-level external devices
#
# Sources:
#   s6b0741.cpp        — Samsung S6B0741 LCD controller (via SIF3)
#   flash_sst39vf.cpp  — SST 39VF400A / 39VF160 NOR flash (BCU Area 6)
#
# Planned (P2 phase):
#   pdiusbd12.cpp   — Philips PDIUSBD12 USB Full-Speed (via BCU/CS)

add_library(piece_board STATIC
    s6b0741.cpp
    flash_sst39vf.cpp
)
target_include_directories(piece_board PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Consumers of piece_board inherit piece_soc (and transitively piece_core).
target_link_libraries(piece_board PUBLIC piece_soc)
