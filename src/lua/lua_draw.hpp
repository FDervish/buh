// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_LUA_LUA_DRAW_HPP
#define BUH_LUA_LUA_DRAW_HPP

#include <windows.h>

namespace Buh {
    /**
     * Drawing functions for Chimera's Lua scripts (shapes on the HUD and in the world, and world_to_screen), if
     * Chimera exports its Lua
     */
    void set_up_lua_drawing() noexcept;

    /**
     * The same, with the Lua (and the call that opens each script's libraries) in that module
     */
    void set_up_lua_drawing(HMODULE module) noexcept;
}

#endif
