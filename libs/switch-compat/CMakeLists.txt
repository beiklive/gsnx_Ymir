# Nintendo Switch compatibility layer (devkitA64 / libnx).
# Provides the few POSIX facilities that newlib on Switch lacks and that ymir-core
# (and its vendored headers, e.g. mio) need to compile and link.

add_library(switch-compat STATIC
    src/mman.cpp
)
add_library(switch-compat::switch-compat ALIAS switch-compat)

target_include_directories(switch-compat PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
