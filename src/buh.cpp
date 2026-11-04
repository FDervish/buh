// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>

#include "audio/audio.hpp"
#include "bink/bink.hpp"
#include "console.hpp"
#include "damage.hpp"
#include "game_state.hpp"
#include "halo.hpp"
#include "hs.hpp"
#include "hud/hud_elements.hpp"
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
#include "post/ao.hpp"
#include "post/bloom.hpp"
#include "post/depth.hpp"
#include "post/grading.hpp"
#include "post/post.hpp"
#include "post/shafts.hpp"
#include "post/smaa.hpp"
#include "render.hpp"
#include "sound/sound_length.hpp"
#include "sound/subtitles.hpp"
#include "lua/lua.hpp"
#include "lua/lua_damage.hpp"
#include "lua/lua_draw.hpp"
#include "lua/lua_hs.hpp"
#include "sprites/shapes.hpp"
#include "sprites/sprites.hpp"
#include "text/text.hpp"

// Chimera loads mods after it has applied its own patches, so everything here goes in on top of them. Each fix
// leaves the game alone if its signatures aren't found (e.g. on a dedicated server). Anything that needs threads,
// COM or Direct3D starts later, when it's first used.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if(reason == DLL_PROCESS_ATTACH) {
        Buh::set_up_halo();
        Buh::set_up_hs();
        Buh::set_up_extend_effect_limits(Buh::set_up_extend_game_state());
        Buh::set_up_extend_render_limits();
        Buh::set_up_particle_culling_fix();
        Buh::set_up_particle_depth_sort();
        Buh::set_up_region_permutation_limit();

        Buh::set_up_console();
        Buh::set_up_render();
        Buh::set_up_damage();
        Buh::set_up_lua();
        Buh::set_up_text();
        Buh::set_up_map_load();
        Buh::set_up_fog_reset();
        Buh::set_up_texture_preload();
        Buh::set_up_tag_packs();
        Buh::set_up_loading_screen();

        // Drawn in the world before the HUD in this order: ambient occlusion on the opaque world, before its
        // transparent surfaces; then shapes, and the post processes over them, the bloom working from the
        // anti-aliased and shafted world and graded with it; then the depth view over all of it
        Buh::set_up_post();
        Buh::set_up_depth();
        Buh::set_up_ao();
        Buh::set_up_shapes();
        Buh::set_up_smaa();
        Buh::set_up_shafts();
        Buh::set_up_bloom();
        Buh::set_up_grading();
        Buh::set_up_depth_view();

        // Drawn after the HUD in this order
        Buh::set_up_bink();
        Buh::set_up_sprites();
        Buh::set_up_lua_drawing();
        Buh::set_up_subtitles();

        Buh::set_up_lua_damage();
        Buh::set_up_lua_hs();
        Buh::set_up_sound_length();
        Buh::set_up_audio();
        Buh::set_up_hud_skew();
        Buh::set_up_hud_elements();
    }
    return TRUE;
}
