# board/ — P/ECE board-level external devices
#
# Planned (P2 phase):
#   nand_flash.cpp  — NAND Flash (BCU/CS, 512 KB factory / 2 MB modified)
#   s6b0741.cpp     — Samsung S6B0741 LCD controller (via SIF)
#   pdiusbd12.cpp   — Philips PDIUSBD12 USB Full-Speed (via BCU/CS)
#   sdcard.cpp      — SD card via GPIO bit-bang SPI (future)

add_library(piece_board INTERFACE)

# Consumers of piece_board inherit piece_soc (and transitively piece_core).
target_link_libraries(piece_board INTERFACE piece_soc)
