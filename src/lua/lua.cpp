// SPDX-License-Identifier: GPL-3.0-only
//
// Chimera exports every one of its Lua functions by name, and makes each script's Lua with one call to
// luaL_openselectedlibs. That call is pointed here instead: once Chimera's libraries are open, buh adds its functions
// through Chimera's own Lua. Nothing of Chimera's is matched by its bytes, and a Chimera that doesn't export its Lua
// just doesn't get them. Chimera adds its own functions after, so where it has one by the same name, it's Chimera's.

#include "lua.hpp"
#include "../memory.hpp"

#include <vector>

namespace Buh {
    LuaApi lua = {};

    struct ScriptFunction {
        const char *name;
        LuaFunction function;
    };
    static auto *script_functions = new std::vector<ScriptFunction>;
    static bool available = false;

    void add_lua_function(const char *name, LuaFunction function) noexcept {
        script_functions->push_back({ name, function });
    }

    bool lua_available() noexcept {
        return available;
    }

    // Stands in for luaL_openselectedlibs where Chimera makes a script's Lua
    static void open_libraries(lua_State *state, int load, int preload) noexcept {
        lua.openselectedlibs(state, load, preload);
        for(auto &function : *script_functions) {
            bool taken = lua.getglobal(state, function.name) != LUA_TNIL;
            lua.settop(state, -2);
            if(!taken) {
                lua.pushcclosure(state, function.function, 0);
                lua.setglobal(state, function.name);
            }
        }
    }

    template<typename T> static bool resolve(HMODULE module, const char *name, T &function) noexcept {
        auto *address = reinterpret_cast<void *>(GetProcAddress(module, name));
        function = reinterpret_cast<T>(address);
        return address != nullptr;
    }

    void set_up_lua(HMODULE module) noexcept {
        if(!module) {
            return;
        }
        bool found = resolve(module, "lua_gettop", lua.gettop) && resolve(module, "lua_checkstack", lua.checkstack)
            && resolve(module, "lua_settop", lua.settop) && resolve(module, "lua_pushvalue", lua.pushvalue)
            && resolve(module, "lua_type", lua.type) && resolve(module, "lua_tonumberx", lua.tonumberx)
            && resolve(module, "lua_tointegerx", lua.tointegerx) && resolve(module, "lua_toboolean", lua.toboolean)
            && resolve(module, "lua_tolstring", lua.tolstring) && resolve(module, "lua_tothread", lua.tothread)
            && resolve(module, "lua_pushnil", lua.pushnil) && resolve(module, "lua_pushnumber", lua.pushnumber)
            && resolve(module, "lua_pushinteger", lua.pushinteger) && resolve(module, "lua_pushstring", lua.pushstring)
            && resolve(module, "lua_pushboolean", lua.pushboolean)
            && resolve(module, "lua_pushcclosure", lua.pushcclosure)
            && resolve(module, "lua_createtable", lua.createtable)
            && resolve(module, "lua_newuserdatauv", lua.newuserdatauv)
            && resolve(module, "lua_setmetatable", lua.setmetatable) && resolve(module, "lua_setfield", lua.setfield)
            && resolve(module, "lua_getfield", lua.getfield) && resolve(module, "lua_rawseti", lua.rawseti)
            && resolve(module, "lua_rawgeti", lua.rawgeti) && resolve(module, "lua_rawsetp", lua.rawsetp)
            && resolve(module, "lua_rawgetp", lua.rawgetp) && resolve(module, "lua_getglobal", lua.getglobal)
            && resolve(module, "lua_setglobal", lua.setglobal) && resolve(module, "lua_pcallk", lua.pcallk)
            && resolve(module, "luaL_error", lua.error)
            && resolve(module, "luaL_openselectedlibs", lua.openselectedlibs);
        if(!found) {
            return;
        }
        auto calls = find_calls(reinterpret_cast<const void *>(lua.openselectedlibs), module);
        for(auto *call : calls) {
            retarget_call(call, reinterpret_cast<const void *>(open_libraries));
        }
        available = !calls.empty();
    }

    void set_up_lua() noexcept {
        set_up_lua(GetModuleHandleA("strings.dll"));
    }
}
