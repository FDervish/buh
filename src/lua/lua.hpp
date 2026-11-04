// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_LUA_LUA_HPP
#define BUH_LUA_LUA_HPP

#include <windows.h>
#include <climits>
#include <cstddef>
#include <cstdint>

#define LUA_TNONE (-1)
#define LUA_TNIL 0
#define LUA_TBOOLEAN 1
#define LUA_TNUMBER 3
#define LUA_TSTRING 4
#define LUA_TTABLE 5
#define LUA_TFUNCTION 6

#define LUA_OK 0

// Lua 5.5's registry (as lua.h has it), and where in it a state keeps its main thread
#define LUA_REGISTRYINDEX (-(INT_MAX/2 + 1000))
#define LUA_RIDX_MAINTHREAD 3

struct lua_State;

namespace Buh {
    using LuaFunction = int (*)(lua_State *state);

    // The Lua functions buh uses, Chimera's own
    struct LuaApi {
        int (*gettop)(lua_State *);
        int (*checkstack)(lua_State *, int);
        void (*settop)(lua_State *, int);
        void (*pushvalue)(lua_State *, int);
        int (*type)(lua_State *, int);
        double (*tonumberx)(lua_State *, int, int *);
        long long (*tointegerx)(lua_State *, int, int *);
        int (*toboolean)(lua_State *, int);
        const char *(*tolstring)(lua_State *, int, std::size_t *);
        lua_State *(*tothread)(lua_State *, int);
        void (*pushnil)(lua_State *);
        void (*pushnumber)(lua_State *, double);
        void (*pushinteger)(lua_State *, long long);
        const char *(*pushstring)(lua_State *, const char *);
        void (*pushboolean)(lua_State *, int);
        void (*pushcclosure)(lua_State *, LuaFunction, int);
        void (*createtable)(lua_State *, int, int);
        void *(*newuserdatauv)(lua_State *, std::size_t, int);
        int (*setmetatable)(lua_State *, int);
        void (*setfield)(lua_State *, int, const char *);
        int (*getfield)(lua_State *, int, const char *);
        void (*rawseti)(lua_State *, int, long long);
        int (*rawgeti)(lua_State *, int, long long);
        void (*rawsetp)(lua_State *, int, const void *);
        int (*rawgetp)(lua_State *, int, const void *);
        int (*getglobal)(lua_State *, const char *);
        void (*setglobal)(lua_State *, const char *);
        int (*pcallk)(lua_State *, int, int, int, std::intptr_t, const void *);
        int (*error)(lua_State *, const char *, ...);
        void (*openselectedlibs)(lua_State *, int, int);
    };
    extern LuaApi lua;

    /**
     * Give every script made from now on a function, unless Chimera has its own by that name
     */
    void add_lua_function(const char *name, LuaFunction function) noexcept;

    /**
     * Whether buh found Chimera's Lua, and gets to add its functions to each script
     */
    bool lua_available() noexcept;

    /**
     * Find Chimera's Lua in strings.dll, if it exports it, and point the call that makes each script's Lua at buh
     */
    void set_up_lua() noexcept;

    /**
     * The same, with the Lua (and the call that opens each script's libraries) in that module
     */
    void set_up_lua(HMODULE module) noexcept;
}

#endif
