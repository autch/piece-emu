cmake_minimum_required(VERSION 3.20)
enable_language(C)

# PFI/PFFS library shared by mkpfi and pfar
add_library(piece_pfi STATIC
    pfi.c
    pffs.c
)
target_include_directories(piece_pfi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(piece_pfi PUBLIC piece_pfi_format)

# mkpfi — create a PFI flash image from a raw P/ECE kernel (all.bin)
add_executable(mkpfi mkpfi.c)
target_link_libraries(mkpfi PRIVATE piece_pfi)

# pfar — P/ECE Flash image ARchiver (list/add/delete/extract PFFS files)
add_executable(pfar pfar.c)
target_link_libraries(pfar PRIVATE piece_pfi)

# ---------------------------------------------------------------------------
# Real-hardware tools (require libusb-1.0)
#   Linux / macOS : pkg-config (distro package or Homebrew's libusb)
#   Windows       : vcpkg port "libusb" (find_path/find_library via toolchain)
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB QUIET libusb-1.0)
endif()

if(NOT LIBUSB_FOUND)
    # Fallback when pkg-config is unavailable (typically Windows / vcpkg).
    find_path(LIBUSB_INCLUDE_DIR
        NAMES libusb.h
        PATH_SUFFIXES libusb-1.0)
    find_library(LIBUSB_LIBRARY
        NAMES usb-1.0 libusb-1.0 libusb)
    if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
        set(LIBUSB_FOUND TRUE)
        # Strip the trailing "/libusb-1.0" so #include <libusb-1.0/libusb.h> works.
        get_filename_component(_libusb_parent "${LIBUSB_INCLUDE_DIR}" DIRECTORY)
        set(LIBUSB_INCLUDE_DIRS "${_libusb_parent}" "${LIBUSB_INCLUDE_DIR}")
        set(LIBUSB_LINK_LIBRARIES "${LIBUSB_LIBRARY}")
    endif()
endif()

if(LIBUSB_FOUND)
    # pieceif — P/ECE USB interface library (libusb port of pieceif.dll)
    add_library(piece_if STATIC pieceif_libusb.c)
    target_include_directories(piece_if PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                               ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(piece_if PUBLIC ${LIBUSB_LINK_LIBRARIES})

    # ripper — read P/ECE flash image via USB and save as PFI
    add_executable(ripper ripper.c)
    target_link_libraries(ripper PRIVATE piece_if piece_pfi)

    message(STATUS "libusb-1.0 found — real-hardware tools enabled (ripper)")
else()
    message(STATUS "libusb-1.0 not found — real-hardware tools skipped (ripper)")
endif()
