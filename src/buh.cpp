// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>

#include "audio/audio.hpp"
#include "bink/bink.hpp"
#include "console.hpp"
#include "game_state.hpp"
#include "halo.hpp"
#include "hud/hud_skew.hpp"
#include "loading/loading_screen.hpp"
#include "map/fog_reset.hpp"
#include "map/map_load.hpp"
#include "map/texture_preload.hpp"
#include "models/region_permutations.hpp"
#include "packs/tag_packs.hpp"
#include "particles/extend_effect_limits.hpp"
#include "particles/extend_render_limits.hpp"
#include "particles/particle_culling.hpp"
#include "particles/particle_depth_sort.hpp"
#include "render.hpp"
#include "sound/sound_length.hpp"
#include "sound/subtitles.hpp"
#include "lua/lua_draw.hpp"
#include "sprites/shapes.hpp"
#include "sprites/sprites.hpp"
#include "text/text.hpp"

// Chimera loads mods after it has applied its own patches, so everything here goes in on top of them. Each fix
// leaves the game alone if its signatures aren't found (e.g. on a dedicated server). Anything that needs threads,
// COM or Direct3D starts later, when it's first used.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if(reason == DLL_PROCESS_ATTACH) {
        Buh::set_up_halo();
        Buh::set_up_extend_effect_limits(Buh::set_up_extend_game_state());
        Buh::set_up_extend_render_limits();
        Buh::set_up_particle_culling_fix();
        Buh::set_up_particle_depth_sort();
        Buh::set_up_region_permutation_limit();

        Buh::set_up_console();
        Buh::set_up_render();
        Buh::set_up_text();
        Buh::set_up_map_load();
        Buh::set_up_fog_reset();
        Buh::set_up_texture_preload();
        Buh::set_up_tag_packs();
        Buh::set_up_loading_screen();

        // Drawn in the world before the HUD
        Buh::set_up_shapes();

        // Drawn after the HUD in this order
        Buh::set_up_bink();
        Buh::set_up_sprites();
        Buh::set_up_lua_drawing();
        Buh::set_up_subtitles();

        Buh::set_up_sound_length();
        Buh::set_up_audio();
        Buh::set_up_hud_skew();
    }
    return TRUE;
}
