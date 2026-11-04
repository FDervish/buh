// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_LUA_LUA_DAMAGE_HPP
#define BUH_LUA_LUA_DAMAGE_HPP

namespace Buh {
    /**
     * set_predamage_callback and set_damage_callback for Chimera's Lua scripts, if buh found Chimera's Lua
     * (set_up_lua) and Halo's damage goes through buh (set_up_damage)
     */
    void set_up_lua_damage() noexcept;
}

#endif
