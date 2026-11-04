// SPDX-License-Identifier: GPL-3.0-only
//
// Damage for Chimera's Lua scripts. set_predamage_callback's callback sees each damage before Halo does it, and can
// change its multiplier or stop it; set_damage_callback's sees what it did, the way Chimera's pretick and tick
// callbacks see a tick. Either one gets a table: the damaged object, the damage effect tag and its path, the object
// and the player that caused it, its multiplier, scale and proc, and whether it's an explosion's or kills outright.
// After, it also has the health and the shield the object lost, and whether it was killed. damage_object() does damage
// of a script's own, the same way, credited to whoever the script says.

#include "lua_damage.hpp"
#include "lua.hpp"
#include "../console.hpp"
#include "../damage.hpp"
#include "../halo.hpp"

#include <string>
#include <vector>

namespace Buh {
    // Where each script keeps its two callbacks, in its registry, and the userdata that tells buh when it's closed
    static char before_key, after_key, closed_key;

    // The scripts with callbacks, by their main threads
    static auto *scripts = new std::vector<lua_State *>;

    // While a callback runs, the damage it does doesn't go to callbacks
    static bool running = false;

    static int usage(lua_State *state, const char *text) noexcept {
        return lua.error(state, "usage: %s", text);
    }

    // The finalizer of a userdata only its script's registry has, so it's called when Chimera closes the script
    static int forget_script(lua_State *state) noexcept {
        std::erase(*scripts, state);
        return 0;
    }

    // A function, the name of a global one (as Chimera's set_callback takes it), or nil for none
    static int set_callback(lua_State *state, const void *key, const char *text) noexcept {
        auto type = lua.type(state, 1);
        if(type != LUA_TFUNCTION && type != LUA_TSTRING && type != LUA_TNIL && type != LUA_TNONE) {
            return usage(state, text);
        }
        lua.settop(state, 1);
        lua.rawsetp(state, LUA_REGISTRYINDEX, key);
        if(lua.rawgetp(state, LUA_REGISTRYINDEX, &closed_key) == LUA_TNIL) {
            lua.newuserdatauv(state, 0, 0);
            lua.createtable(state, 0, 1);
            lua.pushcclosure(state, forget_script, 0);
            lua.setfield(state, -2, "__gc");
            lua.setmetatable(state, -2);
            lua.rawsetp(state, LUA_REGISTRYINDEX, &closed_key);
            lua.rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
            scripts->push_back(lua.tothread(state, -1));
        }
        lua.settop(state, 0);
        return 0;
    }

    static int lua_set_predamage_callback(lua_State *state) noexcept {
        return set_callback(state, &before_key, "set_predamage_callback(<function, or a global function's name, or nil>)");
    }

    static int lua_set_damage_callback(lua_State *state) noexcept {
        return set_callback(state, &after_key, "set_damage_callback(<function, or a global function's name, or nil>)");
    }

    // A script's callback pushed, or false (with nothing pushed) if it has none
    static bool push_callback(lua_State *state, const void *key) noexcept {
        auto type = lua.rawgetp(state, LUA_REGISTRYINDEX, key);
        if(type == LUA_TSTRING) {
            // The name stays in the registry
            auto *name = lua.tolstring(state, -1, nullptr);
            lua.settop(state, -2);
            type = lua.getglobal(state, name);
        }
        if(type != LUA_TFUNCTION) {
            lua.settop(state, -2);
            return false;
        }
        return true;
    }

    static void set_integer(lua_State *state, const char *key, long long value) noexcept {
        lua.pushinteger(state, value);
        lua.setfield(state, -2, key);
    }

    static void set_number(lua_State *state, const char *key, double value) noexcept {
        lua.pushnumber(state, value);
        lua.setfield(state, -2, key);
    }

    static void set_boolean(lua_State *state, const char *key, bool value) noexcept {
        lua.pushboolean(state, value);
        lua.setfield(state, -2, key);
    }

    static void push_damage(lua_State *state, const Damage &damage) noexcept {
        lua.createtable(state, 0, 12);
        set_integer(state, "object", damage.object);
        set_integer(state, "tag", damage.tag);
        if(auto *tag = get_tag(damage.tag)) {
            lua.pushstring(state, tag->path);
            lua.setfield(state, -2, "path");
        }
        if(damage.causer != 0xFFFFFFFF) {
            set_integer(state, "causer", damage.causer);
        }
        // As a player's index, 0 to 15
        if(damage.player != 0xFFFFFFFF) {
            set_integer(state, "player", damage.player & 0xFFFF);
        }
        set_number(state, "multiplier", damage.multiplier);
        set_number(state, "scale", damage.scale);
        set_number(state, "proc", damage.proc);
        set_boolean(state, "area", damage.flags & DAMAGE_AREA_OF_EFFECT);
        set_boolean(state, "instant", damage.flags & DAMAGE_KILL_INSTANTLY);
    }

    // A number from the options table on top of the stack, or the default
    static double option(lua_State *state, const char *key, double otherwise) noexcept {
        lua.getfield(state, -1, key);
        int is_number = 0;
        double value = lua.tonumberx(state, -1, &is_number);
        lua.settop(state, -2);
        return is_number ? value : otherwise;
    }

    static std::uint32_t handle_option(lua_State *state, const char *key) noexcept {
        lua.getfield(state, -1, key);
        int is_integer = 0;
        long long value = lua.tointegerx(state, -1, &is_integer);
        lua.settop(state, -2);
        return is_integer ? static_cast<std::uint32_t>(value) : 0xFFFFFFFF;
    }

    static int failed(lua_State *state, const char *why) noexcept {
        lua.pushboolean(state, false);
        lua.pushstring(state, why);
        return 2;
    }

    // damage_object(object, tag [, options]): true and the health and shield it lost and whether it died, or false and why
    static int lua_damage_object(lua_State *state) noexcept {
        static const char *USAGE = "damage_object(<object ID>, <damage effect tag ID or path> [, { multiplier, scale, causer, player, proc }])";
        int is_integer = 0;
        auto object = static_cast<std::uint32_t>(lua.tointegerx(state, 1, &is_integer));
        auto tag_type = lua.type(state, 2);
        auto options_type = lua.type(state, 3);
        if(!is_integer || (tag_type != LUA_TNUMBER && tag_type != LUA_TSTRING)
            || (options_type != LUA_TTABLE && options_type != LUA_TNIL && options_type != LUA_TNONE)) {
            return usage(state, USAGE);
        }
        Tag *tag = nullptr;
        if(tag_type == LUA_TSTRING) {
            tag = find_tag(lua.tolstring(state, 2, nullptr), TAG_CLASS_DAMAGE_EFFECT);
        }
        else {
            tag = get_tag(static_cast<std::uint32_t>(lua.tointegerx(state, 2, &is_integer)));
        }
        // Anything else in Halo's damage data would be read as one
        if(!tag || tag->primary_class != TAG_CLASS_DAMAGE_EFFECT) {
            return failed(state, "no such damage effect tag");
        }
        double multiplier = 1, scale = 1, proc = 1;
        std::uint32_t causer = 0xFFFFFFFF, player = 0xFFFFFFFF;
        if(options_type == LUA_TTABLE) {
            lua.settop(state, 3);
            multiplier = option(state, "multiplier", 1);
            scale = option(state, "scale", 1);
            proc = option(state, "proc", 1);
            causer = handle_option(state, "causer");
            player = handle_option(state, "player");
        }
        // From a coroutine, the script's main thread is waiting on it and can't be called into, so it's done without
        // the callbacks, as damage a callback does is
        lua.rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
        bool from_main = lua.tothread(state, -1) == state;
        lua.settop(state, -2);
        bool was_running = running;
        running = running || !from_main;
        DamageDone done;
        bool damaged = damage_object(object, tag->id, causer, player, static_cast<float>(multiplier), static_cast<float>(scale),
            static_cast<float>(proc), done);
        running = was_running;
        if(!damaged) {
            return failed(state, "no such object");
        }
        lua.pushboolean(state, true);
        lua.pushnumber(state, done.health);
        lua.pushnumber(state, done.shield);
        lua.pushboolean(state, done.killed);
        return 4;
    }

    // The error on top of the stack, a line at a time, as Chimera prints its own callbacks' errors
    static void print_error(lua_State *state) noexcept {
        auto *message = lua.tolstring(state, -1, nullptr);
        std::string error = message ? message : "error in a damage callback";
        for(std::size_t start = 0; start <= error.size();) {
            auto end = error.find('\n', start);
            if(end == std::string::npos) {
                end = error.size();
            }
            console_error(error.substr(start, end - start).c_str());
            start = end + 1;
        }
    }

    // The damage in a table, to each script's callback in turn: each sees the multiplier the one before it left
    static bool before_damage(Damage &damage) noexcept {
        if(running) {
            return true;
        }
        running = true;
        bool allowed = true;
        for(std::size_t i = 0; allowed && i < scripts->size(); i++) {
            auto *state = (*scripts)[i];
            auto top = lua.gettop(state);
            if(lua.checkstack(state, 6) && push_callback(state, &before_key)) {
                push_damage(state, damage);
                lua.pushvalue(state, top + 1);
                lua.pushvalue(state, top + 2);
                if(lua.pcallk(state, 1, 1, 0, 0, nullptr) == LUA_OK) {
                    allowed = lua.type(state, -1) != LUA_TBOOLEAN || lua.toboolean(state, -1);
                    int is_number = 0;
                    lua.getfield(state, top + 2, "multiplier");
                    double multiplier = lua.tonumberx(state, -1, &is_number);
                    if(is_number) {
                        damage.multiplier = static_cast<float>(multiplier);
                    }
                }
                else {
                    print_error(state);
                }
            }
            lua.settop(state, top);
        }
        running = false;
        return allowed;
    }

    static void after_damage(const Damage &damage, const DamageDone &done) noexcept {
        if(running) {
            return;
        }
        running = true;
        for(std::size_t i = 0; i < scripts->size(); i++) {
            auto *state = (*scripts)[i];
            auto top = lua.gettop(state);
            if(lua.checkstack(state, 4) && push_callback(state, &after_key)) {
                push_damage(state, damage);
                set_number(state, "health", done.health);
                set_number(state, "shield", done.shield);
                set_boolean(state, "killed", done.killed);
                if(lua.pcallk(state, 1, 0, 0, 0, nullptr) != LUA_OK) {
                    print_error(state);
                }
            }
            lua.settop(state, top);
        }
        running = false;
    }

    void set_up_lua_damage() noexcept {
        if(!lua_available() || !damage_hook_available()) {
            return;
        }
        add_lua_function("set_predamage_callback", lua_set_predamage_callback);
        add_lua_function("set_damage_callback", lua_set_damage_callback);
        add_lua_function("damage_object", lua_damage_object);
        set_damage_callbacks(before_damage, after_damage);
    }
}
