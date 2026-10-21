// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>

#include "game_state.hpp"
#include "particles/extend_effect_limits.hpp"
#include "particles/extend_render_limits.hpp"
#include "particles/particle_culling.hpp"
#include "particles/particle_depth_sort.hpp"

// Chimera loads mods after it has applied its own patches, so everything here goes in on top of them. Each fix
// leaves the game alone if its signatures aren't found (e.g. on a dedicated server).
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if(reason == DLL_PROCESS_ATTACH) {
        Buh::set_up_extend_effect_limits(Buh::set_up_extend_game_state());
        Buh::set_up_extend_render_limits();
        Buh::set_up_particle_culling_fix();
        Buh::set_up_particle_depth_sort();
    }
    return TRUE;
}
