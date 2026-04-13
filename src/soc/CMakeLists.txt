add_library(piece_soc STATIC
    peripheral_intc.cpp
    peripheral_clkctl.cpp
    peripheral_t8.cpp
    peripheral_t16.cpp
    peripheral_portctrl.cpp
    peripheral_bcu_area.cpp
    peripheral_wdt.cpp
    peripheral_rtc.cpp
)

target_include_directories(piece_soc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# PUBLIC: consumers of piece_soc also inherit piece_core's include dirs.
target_link_libraries(piece_soc PUBLIC piece_core)
