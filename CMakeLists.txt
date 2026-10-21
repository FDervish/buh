# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.16)

project(buh
    VERSION 0.1.0
    DESCRIPTION "Chimera mod for Halo Combat Evolved"
    LANGUAGES C CXX ASM
)

set(CMAKE_CXX_STANDARD 20)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "buh has to be built for 32-bit Windows (i686-w64-mingw32)")
endif()

add_library(buh MODULE
    src/buh.cpp
    src/memory.cpp
    src/particles/extend_effect_limits.cpp
    src/particles/extend_effect_limits.S
    src/particles/extend_render_limits.cpp
    src/particles/particle_culling.cpp
    src/particles/particle_depth_sort.cpp
)

target_compile_options(buh PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -pedantic -Wold-style-cast>)
target_link_options(buh PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols)

# Chimera loads mods\*.dll
set_target_properties(buh PROPERTIES PREFIX "" OUTPUT_NAME "buh")
