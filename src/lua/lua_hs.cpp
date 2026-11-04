// SPDX-License-Identifier: GPL-3.0-only
//
// hs(script [, values] [, type]) runs a script of Halo's, the way Chimera's execute_script does, and gives back what
// it returns. Halo's console only runs scripts, so the script is run as (set <global> <script>): one of Halo's own
// globals of the type the script gives (the type its outermost function returns, or the one asked for) takes the
// result, buh reads it, and puts the global back. A value the script can't have left there says it didn't run. A
// script that gives nothing is run with (set <a boolean> true) after it, so that says it ran too.
//
// $name in the script is replaced with values.name: a string as a string ("..."), a boolean as true or false, and an
// object (by ID or by address) as a name buh gives it for the moment (hs.hpp), so a script can be given any object.

#include "lua_hs.hpp"
#include "lua.hpp"
#include "../halo.hpp"
#include "../hs.hpp"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

// Halo's console takes 1023 characters, and none of a script with a ; in it
#define CONSOLE_LENGTH 1023

namespace Buh {
    // While hs() runs a script, a callback the script sets off (damage, say) can't run another
    static bool running = false;

    static int usage(lua_State *state) noexcept {
        return lua.error(state, "usage: hs(script [, values] [, type])");
    }

    static int failed(lua_State *state, const std::string &why) noexcept {
        lua.pushnil(state);
        lua.pushstring(state, why.c_str());
        return 2;
    }

    struct TypeName {
        const char *name;
        std::int16_t type;
    };
    static const TypeName type_names[] = {
        { "void", HS_TYPE_VOID }, { "boolean", HS_TYPE_BOOLEAN }, { "real", HS_TYPE_REAL }, { "short", HS_TYPE_SHORT }, { "long", HS_TYPE_LONG },
    };

    // What a type a script gives is, for saying it can't come back
    static std::string type_said(std::int16_t type) {
        if(type >= 32 && type <= 36) {
            return "one of a set of names (compare it in the script, like (= (game_difficulty_get) hard))";
        }
        if(type >= 37 && type <= 42) {
            return "an object (ask the script for what you want of it, like (unit_get_health ...))";
        }
        switch(type) {
            case 9: return "a string";
            case 23: return "a list of objects (ask the script for what you want of it, like (list_count ...))";
            default: return "a value of Halo's type " + std::to_string(type);
        }
    }

    static std::size_t type_size(std::int16_t type) noexcept {
        return type == HS_TYPE_BOOLEAN ? 1 : type == HS_TYPE_SHORT ? 2 : 4;
    }

    // What a script can't set a global of each type to: not a boolean's 0 or 1, a float's NaN, the least short and long
    static void write_sentinel(void *address, std::int16_t type) noexcept {
        static const std::uint8_t boolean = 0xA5;
        static const std::uint32_t real = 0x7FC1D00D;
        static const std::int16_t short_integer = -0x8000;
        static const std::uint32_t long_integer = 0x80000000;
        const void *value = type == HS_TYPE_BOOLEAN ? static_cast<const void *>(&boolean) : type == HS_TYPE_REAL ? static_cast<const void *>(&real)
            : type == HS_TYPE_SHORT ? static_cast<const void *>(&short_integer) : static_cast<const void *>(&long_integer);
        std::memcpy(address, value, type_size(type));
    }

    static bool is_sentinel(const std::byte *value, std::int16_t type) noexcept {
        std::byte sentinel[4];
        write_sentinel(sentinel, type);
        return std::memcmp(value, sentinel, type_size(type)) == 0;
    }

    static bool name_character(char c) noexcept {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    // The script with each $name replaced, and the objects it names, in the order their names were given
    static bool substitute(lua_State *state, const char *text, int values, std::string &script, std::vector<std::uint32_t> &objects, std::string &why) {
        for(const char *c = text; *c; c++) {
            if(*c != '$') {
                script.push_back(*c);
                continue;
            }
            std::string name;
            while(name_character(c[1])) {
                name.push_back(*++c);
            }
            if(name.empty()) {
                why = "a $ with no name after it";
                return false;
            }
            if(!values) {
                why = "$" + name + ", but no table of values";
                return false;
            }
            int type = lua.getfield(state, values, name.c_str());
            bool ok = true;
            if(type == LUA_TSTRING) {
                const char *string = lua.tolstring(state, -1, nullptr);
                if(std::strchr(string, '"')) {
                    why = "$" + name + " has a \" in it, which a script's string can't";
                    ok = false;
                }
                else {
                    script += std::string("\"") + string + "\"";
                }
            }
            else if(type == LUA_TBOOLEAN) {
                script += lua.toboolean(state, -1) ? "true" : "false";
            }
            else if(type == LUA_TNUMBER) {
                int is_integer = 0;
                auto number = lua.tointegerx(state, -1, &is_integer);
                auto object = static_cast<std::uint32_t>(number);
                if(is_integer && !object_datum(object)) {
                    object = object_id_at(static_cast<std::uintptr_t>(number));
                }
                if(!is_integer || !object_datum(object)) {
                    why = "$" + name + " is no object, by ID or by address";
                    ok = false;
                }
                else {
                    std::size_t index = 0;
                    while(index < objects.size() && objects[index] != object) {
                        index++;
                    }
                    if(index == objects.size()) {
                        objects.push_back(object);
                    }
                    script += hs_bound_name(index);
                }
            }
            else {
                why = "nothing given for $" + name;
                ok = false;
            }
            lua.settop(state, -2);
            if(!ok) {
                return false;
            }
        }
        return true;
    }

    // hs(script [, values] [, type]) -> what the script gives (true if nothing), or nil and why it didn't run
    static int lua_hs(lua_State *state) noexcept {
        if(lua.type(state, 1) != LUA_TSTRING) {
            return usage(state);
        }
        const char *text = lua.tolstring(state, 1, nullptr);
        int values = 0;
        const char *wanted = nullptr;
        if(lua.type(state, 2) == LUA_TTABLE) {
            values = 2;
            if(lua.type(state, 3) == LUA_TSTRING) {
                wanted = lua.tolstring(state, 3, nullptr);
            }
            else if(lua.type(state, 3) > LUA_TNIL) {
                return usage(state);
            }
        }
        else if(lua.type(state, 2) == LUA_TSTRING) {
            wanted = lua.tolstring(state, 2, nullptr);
        }
        else if(lua.type(state, 2) > LUA_TNIL) {
            return usage(state);
        }
        if(running) {
            return failed(state, "hs() is running a script already: a callback that script set off can't run another");
        }
        if(!hs_available()) {
            return failed(state, "buh couldn't find Halo's script functions and globals");
        }

        std::string script, why;
        std::vector<std::uint32_t> objects;
        if(!substitute(state, text, values, script, objects, why)) {
            return failed(state, why);
        }
        if(script.find(';') != std::string::npos) {
            return failed(state, "Halo's console runs nothing of a script with a ; in it");
        }
        auto first = script.find_first_not_of(" \t\r\n");
        auto last = script.find_last_not_of(" \t\r\n");
        if(first == std::string::npos) {
            return failed(state, "no script");
        }
        auto expression = script.substr(first, last - first + 1);

        // What it gives: the type asked for, or what its outermost function returns, or one of Halo's own globals' type
        bool call = expression[0] == '(';
        std::string head;
        if(call) {
            auto start = expression.find_first_not_of(" \t\r\n", 1);
            auto end = start == std::string::npos ? std::string::npos : expression.find_first_of(" \t\r\n()", start);
            head = start == std::string::npos ? "" : expression.substr(start, end - start);
        }
        else if(expression.find_first_of(" \t\r\n") != std::string::npos) {
            // Console style, a function and its arguments without brackets
            head = expression.substr(0, expression.find_first_of(" \t\r\n"));
            expression = "(" + expression + ")";
            call = true;
        }
        else if(hs_function_type(expression.c_str()) >= 0) {
            head = expression;
            expression = "(" + expression + ")";
            call = true;
        }
        std::int16_t type = HS_TYPE_VOID;
        if(wanted) {
            bool known = false;
            for(auto &name : type_names) {
                if(std::strcmp(name.name, wanted) == 0) {
                    type = name.type;
                    known = true;
                }
            }
            if(!known) {
                return failed(state, std::string("no type ") + wanted + ": void, boolean, real, short or long");
            }
        }
        else if(call) {
            int returns = hs_function_type(head.c_str());
            // A script's own or a passthrough (begin, if, cond, set) gives nothing back, unless a type is asked for
            type = returns > HS_TYPE_PASSTHROUGH ? static_cast<std::int16_t>(returns) : HS_TYPE_VOID;
        }
        else {
            void *address;
            if(!hs_global(expression.c_str(), type, address)) {
                type = HS_TYPE_VOID;
            }
        }
        if(type < HS_TYPE_VOID || type > HS_TYPE_LONG) {
            return failed(state, "it gives " + type_said(type) + ", which can't come back to Lua");
        }

        auto global = hs_scratch_global(type == HS_TYPE_VOID ? HS_TYPE_BOOLEAN : type);
        if(!global.name) {
            return failed(state, "buh found no global of Halo's to hold its result in");
        }
        auto held = type == HS_TYPE_VOID ? HS_TYPE_BOOLEAN : type;
        auto whole = type == HS_TYPE_VOID ? "(begin " + expression + " (set " + global.name + " true))" : "(set " + std::string(global.name) + " " + expression + ")";
        if(whole.size() > CONSOLE_LENGTH) {
            return failed(state, "it's too long for Halo's console");
        }

        int top = lua.gettop(state);
        if(lua.getglobal(state, "execute_script") != LUA_TFUNCTION) {
            lua.settop(state, top);
            return failed(state, "there's no execute_script to run it with");
        }
        if(!objects.empty()) {
            if(auto *problem = hs_bind_objects(objects.data(), objects.size())) {
                lua.settop(state, top);
                return failed(state, problem);
            }
        }

        // Something the script can't leave there, to say whether it ran; and the global put back after
        std::byte before[4], after[4];
        auto size = type_size(held);
        std::memcpy(before, global.address, size);
        write_sentinel(global.address, held);
        running = true;
        lua.pushstring(state, whole.c_str());
        int status = lua.pcallk(state, 1, 0, 0, 0, nullptr);
        running = false;
        std::memcpy(after, global.address, size);
        std::memcpy(global.address, before, size);
        hs_unbind_objects();

        if(status != LUA_OK) {
            std::string error = lua.type(state, -1) == LUA_TSTRING ? lua.tolstring(state, -1, nullptr) : "execute_script failed";
            lua.settop(state, top);
            return failed(state, error);
        }
        lua.settop(state, top);
        if(is_sentinel(after, held)) {
            return failed(state, "Halo didn't run it (its console says why)");
        }
        switch(type) {
            case HS_TYPE_VOID:
                lua.pushboolean(state, true);
                break;
            case HS_TYPE_BOOLEAN:
                lua.pushboolean(state, std::to_integer<int>(after[0]) != 0);
                break;
            case HS_TYPE_REAL: {
                float real;
                std::memcpy(&real, after, sizeof(real));
                lua.pushnumber(state, real);
                break;
            }
            case HS_TYPE_SHORT: {
                std::int16_t integer;
                std::memcpy(&integer, after, sizeof(integer));
                lua.pushinteger(state, integer);
                break;
            }
            default: {
                std::int32_t integer;
                std::memcpy(&integer, after, sizeof(integer));
                lua.pushinteger(state, integer);
                break;
            }
        }
        return 1;
    }

    void set_up_lua_hs() noexcept {
        if(!lua_available() || !hs_available()) {
            return;
        }
        add_lua_function("hs", lua_hs);
    }
}
