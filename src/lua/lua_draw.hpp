// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_LUA_LUA_DRAW_HPP
#define BUH_LUA_LUA_DRAW_HPP

namespace Buh {
    /**
     * Drawing functions for Chimera's Lua scripts (shapes on the HUD and in the world, and world_to_screen),
     * collision_test, and HUD element changes, if buh found Chimera's Lua (set_up_lua)
     */
    void set_up_lua_drawing() noexcept;
}

#endif
