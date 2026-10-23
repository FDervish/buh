# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.16)

project(buh
    VERSION 0.1.0
    DESCRIPTION "Chimera mod for Halo Combat Evolved"
    LANGUAGES C CXX ASM RC
)

set(CMAKE_CXX_STANDARD 20)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "buh has to be built for 32-bit Windows (i686-w64-mingw32)")
endif()

# SoLoud, unmodified, with just the WASAPI backend and the wav/mp3/flac/ogg decoders
set(SOLOUD ext/soloud)
add_library(soloud STATIC
    ${SOLOUD}/src/core/soloud.cpp
    ${SOLOUD}/src/core/soloud_audiosource.cpp
    ${SOLOUD}/src/core/soloud_bus.cpp
    ${SOLOUD}/src/core/soloud_core_3d.cpp
    ${SOLOUD}/src/core/soloud_core_basicops.cpp
    ${SOLOUD}/src/core/soloud_core_faderops.cpp
    ${SOLOUD}/src/core/soloud_core_filterops.cpp
    ${SOLOUD}/src/core/soloud_core_getters.cpp
    ${SOLOUD}/src/core/soloud_core_setters.cpp
    ${SOLOUD}/src/core/soloud_core_voicegroup.cpp
    ${SOLOUD}/src/core/soloud_core_voiceops.cpp
    ${SOLOUD}/src/core/soloud_fader.cpp
    ${SOLOUD}/src/core/soloud_fft.cpp
    ${SOLOUD}/src/core/soloud_fft_lut.cpp
    ${SOLOUD}/src/core/soloud_file.cpp
    ${SOLOUD}/src/core/soloud_filter.cpp
    ${SOLOUD}/src/core/soloud_misc.cpp
    ${SOLOUD}/src/core/soloud_queue.cpp
    ${SOLOUD}/src/core/soloud_thread.cpp
    ${SOLOUD}/src/audiosource/wav/dr_impl.cpp
    ${SOLOUD}/src/audiosource/wav/soloud_wav.cpp
    ${SOLOUD}/src/audiosource/wav/soloud_wavstream.cpp
    ${SOLOUD}/src/audiosource/wav/stb_vorbis.c
    ${SOLOUD}/src/backend/wasapi/soloud_wasapi.cpp
)
target_include_directories(soloud PUBLIC ${SOLOUD}/include)
target_compile_definitions(soloud PUBLIC WITH_WASAPI SOLOUD_NO_ASSERTS)
# Threads Windows starts don't have the 16 byte aligned stack SSE wants
target_compile_options(soloud PRIVATE -msse2 -mstackrealign -w)

add_library(buh MODULE
    src/buh.cpp
    src/audio/audio.cpp
    src/bink/bink.cpp
    src/bink/bink_player.cpp
    src/console.cpp
    src/console.S
    src/game_state.cpp
    src/halo.cpp
    src/hud/hud_skew.cpp
    src/hud/hud_skew.S
    src/loading/loading_screen.cpp
    src/loading/loading_screen.S
    src/loading/loading_screen.rc
    src/loading/loading_tone.cpp
    src/map/fog_reset.cpp
    src/map/map_load.cpp
    src/map/map_load.S
    src/map/texture_preload.cpp
    src/map/texture_preload.S
    src/memory.cpp
    src/models/region_permutations.cpp
    src/models/region_permutations.S
    src/packs/tag_packs.cpp
    src/packs/tag_packs.S
    src/particles/extend_effect_limits.cpp
    src/particles/extend_effect_limits.S
    src/particles/extend_render_limits.cpp
    src/particles/particle_culling.cpp
    src/particles/particle_depth_sort.cpp
    src/render.cpp
    src/render.S
    src/sound/sound_length.cpp
    src/sound/subtitles.cpp
    src/sound/subtitles.S
    src/lua/lua_draw.cpp
    src/sprites/image.cpp
    src/sprites/shapes.cpp
    src/sprites/sprites.cpp
    src/text/text.cpp
    src/text/text.S
)

target_compile_options(buh PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -pedantic -Wold-style-cast>)
target_link_libraries(buh PRIVATE soloud ole32 windowscodecs uuid)
target_link_options(buh PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols)

# Chimera loads mods\*.dll
set_target_properties(buh PROPERTIES PREFIX "" OUTPUT_NAME "buh")
