# Ymir frontend for the Nintendo Switch (devkitA64 / libnx).
# A minimal homebrew UI that browses the SD card, selects a Saturn disc image,
# and boots it on the emulator core using the software VDP renderer.

if (NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    message(FATAL_ERROR "ymir-switch can only be built for the Nintendo Switch (devkitA64)")
endif ()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_ttf REQUIRED IMPORTED_TARGET SDL2_ttf)

add_executable(ymir-switch
    src/main.cpp
)

target_link_libraries(ymir-switch PRIVATE
    ymir::ymir-core
    PkgConfig::SDL2
    PkgConfig::SDL2_ttf
)

# The bundled font is embedded into the .nro's ROMFS.
set(YMIR_SWITCH_ROMFS "${CMAKE_CURRENT_SOURCE_DIR}/res")
nx_create_nro(ymir-switch ROMFS "${YMIR_SWITCH_ROMFS}")
