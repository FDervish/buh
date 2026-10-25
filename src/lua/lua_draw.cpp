// SPDX-License-Identifier: GPL-3.0-only
//
// Drawing for Chimera's Lua scripts: rectangles, lines, quads and circles on the HUD, lines, boxes and circles in the
// world, and world_to_screen, each drawn in the frame a script draws it. And collision_test, Halo's own ray trace, and
// get_hud_elements, set_hud_element and reset_hud_elements, which change how single HUD elements are drawn.
//
// Chimera exports every one of its Lua functions by name, and makes each script's Lua with one call to
// luaL_openselectedlibs. That call is pointed here instead: once Chimera's libraries are open, buh adds its functions
// through Chimera's own Lua. Nothing of Chimera's is matched by its bytes, and a Chimera that doesn't export its Lua
// just doesn't get them. Chimera adds its own functions after, so where it has one by the same name, it's Chimera's.

#include "lua_draw.hpp"
#include "../halo.hpp"
#include "../hud/hud_elements.hpp"
#include "../memory.hpp"
#include "../render.hpp"
#include "../sprites/image.hpp"
#include "../sprites/shapes.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#define LUA_TNONE (-1)
#define LUA_TNIL 0
#define LUA_TBOOLEAN 1
#define LUA_TNUMBER 3
#define LUA_TSTRING 4
#define LUA_TTABLE 5

// The HUD's units, which scripts draw in: 640 across and 480 down, whatever the screen's shape
#define HUD_WIDTH 640.0
#define HUD_HEIGHT 480.0

#define MAXIMUM_QUEUED_SHAPES 16384

#define MAXIMUM_LISTED_HUD_ELEMENTS 256

struct lua_State;

namespace Buh {
    using LuaFunction = int (*)(lua_State *state);

    // The Lua functions used here, Chimera's own
    static struct {
        int (*type)(lua_State *, int);
        void (*settop)(lua_State *, int);
        double (*tonumberx)(lua_State *, int, int *);
        long long (*tointegerx)(lua_State *, int, int *);
        int (*toboolean)(lua_State *, int);
        const char *(*tolstring)(lua_State *, int, std::size_t *);
        void (*pushnil)(lua_State *);
        void (*pushnumber)(lua_State *, double);
        void (*pushinteger)(lua_State *, long long);
        const char *(*pushstring)(lua_State *, const char *);
        void (*pushboolean)(lua_State *, int);
        void (*createtable)(lua_State *, int, int);
        void (*setfield)(lua_State *, int, const char *);
        void (*rawseti)(lua_State *, int, long long);
        void (*pushcclosure)(lua_State *, LuaFunction, int);
        int (*getglobal)(lua_State *, const char *);
        void (*setglobal)(lua_State *, const char *);
        int (*getfield)(lua_State *, int, const char *);
        int (*rawgeti)(lua_State *, int, long long);
        int (*error)(lua_State *, const char *, ...);
        void (*openselectedlibs)(lua_State *, int, int);
    } lua = {};

    // The HUD shapes scripts drew for this frame
    struct QueuedShape {
        Shape shape;
        float x;
        float y;
        float rotation;
    };
    static auto *queued_shapes = new std::vector<QueuedShape>;
    static IDirect3DStateBlock9 *state_block = nullptr;

    // ---------------------------------------------------------------- arguments
    //
    // A Lua error jumps straight out of the function that raises it, past any C++ destructors, so every function here
    // reads and checks all of its arguments, into plain values, before it makes anything that needs destroying.

    // The numbers from first on, or false if any of them isn't one
    static bool numbers(lua_State *state, int first, int count, double *out) noexcept {
        for(int i = 0; i < count; i++) {
            int is_number = 0;
            out[i] = lua.tonumberx(state, first + i, &is_number);
            if(!is_number) {
                return false;
            }
        }
        return true;
    }

    // Alpha, red, green and blue, as draw_text takes them
    static ShapeColor argb(const double *values) noexcept {
        return { static_cast<float>(values[1]), static_cast<float>(values[2]), static_cast<float>(values[3]), static_cast<float>(values[0]) };
    }

    struct Options {
        bool outline = false;
        double outline_thickness = 0.0;
        double corner_radius = 0.0;
        double arc_from = 0.0;
        double arc_to = 360.0;
        bool has_gradient = false;
        double gradient[4] = {};
        double rotate = 0.0;
        bool through_walls = false;
        bool fill = false;
    };

    static void option_number(lua_State *state, int table, const char *key, double &value) noexcept {
        if(lua.getfield(state, table, key) == LUA_TNUMBER) {
            value = lua.tonumberx(state, -1, nullptr);
        }
        lua.settop(state, -2);
    }

    static bool option_flag(lua_State *state, int table, const char *key) noexcept {
        lua.getfield(state, table, key);
        bool set = lua.toboolean(state, -1);
        lua.settop(state, -2);
        return set;
    }

    // A list of numbers, like { from, to } or { a, r, g, b }
    static bool option_list(lua_State *state, int table, const char *key, int count, double *out) noexcept {
        bool found = lua.getfield(state, table, key) == LUA_TTABLE;
        for(int i = 0; found && i < count; i++) {
            found = lua.rawgeti(state, -1, i + 1) == LUA_TNUMBER;
            if(found) {
                out[i] = lua.tonumberx(state, -1, nullptr);
            }
            lua.settop(state, -2);
        }
        lua.settop(state, -2);
        return found;
    }

    // The options table at index, if there's one there: false if something else is
    static bool read_options(lua_State *state, int index, Options &options) noexcept {
        auto type = lua.type(state, index);
        if(type == LUA_TNONE || type == LUA_TNIL) {
            return true;
        }
        if(type != LUA_TTABLE) {
            return false;
        }
        // outline = thickness, or true for the line's own
        auto outline = lua.getfield(state, index, "outline");
        if(outline == LUA_TNUMBER) {
            options.outline = true;
            options.outline_thickness = lua.tonumberx(state, -1, nullptr);
        }
        else if(outline == LUA_TBOOLEAN) {
            options.outline = lua.toboolean(state, -1);
        }
        lua.settop(state, -2);
        option_number(state, index, "round", options.corner_radius);
        double arc[2];
        if(option_list(state, index, "arc", 2, arc)) {
            options.arc_from = arc[0];
            options.arc_to = arc[1];
        }
        options.has_gradient = option_list(state, index, "gradient", 4, options.gradient);
        option_number(state, index, "rotate", options.rotate);
        options.through_walls = option_flag(state, index, "through_walls");
        options.fill = option_flag(state, index, "fill");
        return true;
    }

    // The usage, raised as a Lua error
    static int usage(lua_State *state, const char *text) noexcept {
        return lua.error(state, "usage: %s", text);
    }

    // ---------------------------------------------------------------- on the HUD

    static void queue(Shape shape, const Options &options, double x, double y) {
        if(options.outline) {
            shape.outline = true;
            if(options.outline_thickness > 0.0) {
                shape.thickness = static_cast<float>(options.outline_thickness);
            }
        }
        shape.corner_radius = static_cast<float>(options.corner_radius / HUD_HEIGHT);
        shape.arc_from = static_cast<float>(options.arc_from);
        shape.arc_to = static_cast<float>(options.arc_to);
        if(options.has_gradient) {
            shape.gradient = argb(options.gradient);
            shape.has_gradient = true;
        }
        if(queued_shapes->size() < MAXIMUM_QUEUED_SHAPES) {
            queued_shapes->push_back({ shape, static_cast<float>(x / HUD_WIDTH), static_cast<float>(y / HUD_HEIGHT), static_cast<float>(options.rotate) });
        }
    }

    // A shape of the HUD's, its lines one unit (a 480th of the screen's height) thick unless they say
    static Shape hud_shape(Shape::Kind kind, const double *color) noexcept {
        Shape shape;
        shape.kind = kind;
        shape.color = argb(color);
        shape.thickness = 1.0F;
        shape.thickness_scaled = true;
        return shape;
    }

    // draw_rectangle(left, top, width, height, a, r, g, b [, options])
    static int lua_draw_rectangle(lua_State *state) noexcept {
        double v[8];
        Options options;
        if(!numbers(state, 1, 8, v) || !read_options(state, 9, options)) {
            return usage(state, "draw_rectangle(left, top, width, height, a, r, g, b [, options])");
        }
        // Measured the other way, from the other corner
        if(v[2] < 0.0) {
            v[0] += v[2];
            v[2] = -v[2];
        }
        if(v[3] < 0.0) {
            v[1] += v[3];
            v[3] = -v[3];
        }
        auto shape = hud_shape(Shape::RECTANGLE, v + 4);
        shape.points[0] = static_cast<float>(v[2] / HUD_WIDTH);
        shape.points[1] = static_cast<float>(v[3] / HUD_HEIGHT);
        queue(shape, options, v[0], v[1]);
        return 0;
    }

    // draw_line(x1, y1, x2, y2, thickness, a, r, g, b [, options])
    static int lua_draw_line(lua_State *state) noexcept {
        double v[9];
        Options options;
        if(!numbers(state, 1, 9, v) || !read_options(state, 10, options)) {
            return usage(state, "draw_line(x1, y1, x2, y2, thickness, a, r, g, b [, options])");
        }
        auto shape = hud_shape(Shape::LINE, v + 5);
        shape.points[0] = static_cast<float>((v[2] - v[0]) / HUD_WIDTH);
        shape.points[1] = static_cast<float>((v[3] - v[1]) / HUD_HEIGHT);
        shape.thickness = static_cast<float>(v[4]);
        options.outline = false;
        queue(shape, options, v[0], v[1]);
        return 0;
    }

    // draw_quad(x1, y1, x2, y2, x3, y3, x4, y4, a, r, g, b [, options])
    static int lua_draw_quad(lua_State *state) noexcept {
        double v[12];
        Options options;
        if(!numbers(state, 1, 12, v) || !read_options(state, 13, options)) {
            return usage(state, "draw_quad(x1, y1, x2, y2, x3, y3, x4, y4, a, r, g, b [, options])");
        }
        auto shape = hud_shape(Shape::QUAD, v + 8);
        for(int i = 0; i < 3; i++) {
            shape.points[i * 2] = static_cast<float>((v[2 + i * 2] - v[0]) / HUD_WIDTH);
            shape.points[i * 2 + 1] = static_cast<float>((v[3 + i * 2] - v[1]) / HUD_HEIGHT);
        }
        queue(shape, options, v[0], v[1]);
        return 0;
    }

    // draw_circle(x, y, radius, a, r, g, b [, options])
    static int lua_draw_circle(lua_State *state) noexcept {
        double v[7];
        Options options;
        if(!numbers(state, 1, 7, v) || v[2] <= 0.0 || !read_options(state, 8, options)) {
            return usage(state, "draw_circle(x, y, radius, a, r, g, b [, options])");
        }
        auto shape = hud_shape(Shape::CIRCLE, v + 3);
        shape.points[0] = static_cast<float>(v[2] / HUD_HEIGHT);
        queue(shape, options, v[0], v[1]);
        return 0;
    }

    // ---------------------------------------------------------------- in the world

    static void queue_world(WorldShape::Kind kind, const double *a, const double *b, double thickness, const double *color, const Options &options) {
        WorldShape shape;
        shape.kind = kind;
        for(int i = 0; i < 3; i++) {
            shape.a[i] = static_cast<float>(a[i]);
            shape.b[i] = static_cast<float>(b[i]);
        }
        shape.color = argb(color);
        if(options.has_gradient) {
            shape.gradient = argb(options.gradient);
            shape.has_gradient = true;
        }
        // In the HUD's units, so it stays visible far away
        shape.thickness = static_cast<float>(thickness);
        shape.thickness_scaled = true;
        shape.through_walls = options.through_walls;
        shape.fill = options.fill;
        queue_world_shape(shape);
    }

    // draw_line_3d(x1, y1, z1, x2, y2, z2, thickness, a, r, g, b [, through_walls | options])
    static int lua_draw_line_3d(lua_State *state) noexcept {
        double v[11];
        Options options;
        bool valid = numbers(state, 1, 11, v);
        if(valid && lua.type(state, 12) == LUA_TBOOLEAN) {
            options.through_walls = lua.toboolean(state, 12);
        }
        else if(valid) {
            valid = read_options(state, 12, options);
        }
        if(!valid) {
            return usage(state, "draw_line_3d(x1, y1, z1, x2, y2, z2, thickness, a, r, g, b [, through_walls | options])");
        }
        queue_world(WorldShape::LINE, v, v + 3, v[6], v + 7, options);
        return 0;
    }

    // draw_box_3d(x, y, z, width, length, height, thickness, a, r, g, b [, options])
    static int lua_draw_box_3d(lua_State *state) noexcept {
        double v[11];
        Options options;
        if(!numbers(state, 1, 11, v) || v[3] < 0.0 || v[4] < 0.0 || v[5] < 0.0 || !read_options(state, 12, options)) {
            return usage(state, "draw_box_3d(x, y, z, width, length, height, thickness, a, r, g, b [, options])");
        }
        queue_world(WorldShape::BOX, v, v + 3, v[6], v + 7, options);
        return 0;
    }

    // draw_circle_3d(x, y, z, radius, thickness, a, r, g, b [, options])
    static int lua_draw_circle_3d(lua_State *state) noexcept {
        double v[9];
        Options options;
        if(!numbers(state, 1, 9, v) || v[3] <= 0.0 || !read_options(state, 10, options)) {
            return usage(state, "draw_circle_3d(x, y, z, radius, thickness, a, r, g, b [, options])");
        }
        const double radius[3] = { v[3], 0.0, 0.0 };
        queue_world(WorldShape::CIRCLE, v, radius, v[4], v + 5, options);
        return 0;
    }

    // world_to_screen(x, y, z) -> x, y, depth, or nil behind the camera
    static int lua_world_to_screen(lua_State *state) noexcept {
        double v[3];
        if(!numbers(state, 1, 3, v)) {
            return usage(state, "world_to_screen(x, y, z)");
        }
        float x, y, depth;
        if(!world_to_screen(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), x, y, depth)) {
            lua.pushnil(state);
            return 1;
        }
        lua.pushnumber(state, x);
        lua.pushnumber(state, y);
        lua.pushnumber(state, depth);
        return 3;
    }

    // ---------------------------------------------------------------- collisions

    // What a ray can hit, by name
    static const struct {
        const char *name;
        std::uint32_t flags;
    } HIT_NAMES[] = {
        { "everything", COLLISION_LEVEL | COLLISION_WATER | COLLISION_ALL_OBJECTS },
        // What blocks Halo's AI's line of sight: the level, vehicles, scenery and machines, past see-through surfaces
        { "sight", COLLISION_LEVEL | COLLISION_OBJECT_TYPE(1) | COLLISION_OBJECT_TYPE(6) | COLLISION_OBJECT_TYPE(7) | COLLISION_SKIP_TWO_SIDED },
        { "level", COLLISION_LEVEL },
        { "water", COLLISION_WATER },
        { "objects", COLLISION_ALL_OBJECTS },
        { "bipeds", COLLISION_OBJECT_TYPE(0) },
        { "vehicles", COLLISION_OBJECT_TYPE(1) },
        { "weapons", COLLISION_OBJECT_TYPE(2) },
        { "equipment", COLLISION_OBJECT_TYPE(3) },
        { "garbage", COLLISION_OBJECT_TYPE(4) },
        { "projectiles", COLLISION_OBJECT_TYPE(5) },
        { "scenery", COLLISION_OBJECT_TYPE(6) },
        { "machines", COLLISION_OBJECT_TYPE(7) },
        { "controls", COLLISION_OBJECT_TYPE(8) },
        { "light_fixtures", COLLISION_OBJECT_TYPE(9) },
        { "placeholders", COLLISION_OBJECT_TYPE(10) },
        { "sound_scenery", COLLISION_OBJECT_TYPE(11) }
    };

    // Halo's materials, by the names tags give them
    static const char *const MATERIAL_NAMES[] = {
        "dirt", "sand", "stone", "snow", "wood", "metal_hollow", "metal_thin", "metal_thick", "rubber", "glass",
        "force_field", "grunt", "hunter_armor", "hunter_skin", "elite", "jackal", "jackal_energy_shield", "engineer_skin",
        "engineer_force_field", "flood_combat_form", "flood_carrier_form", "cyborg_armor", "cyborg_energy_shield",
        "human_armor", "human_skin", "sentinel", "monitor", "plastic", "water", "leaves", "elite_energy_shield", "ice",
        "hunter_shield"
    };

    static const char *const HIT_WHAT[] = { "level", "water", "object" };

    // What the name at index hits, or 0 if it isn't one
    static std::uint32_t hit_flags(lua_State *state, int index) noexcept {
        if(lua.type(state, index) != LUA_TSTRING) {
            return 0;
        }
        auto *name = lua.tolstring(state, index, nullptr);
        for(auto &hit : HIT_NAMES) {
            if(std::strcmp(name, hit.name) == 0) {
                return hit.flags;
            }
        }
        return 0;
    }

    struct CollisionOptions {
        std::uint32_t hit = HIT_NAMES[0].flags;
        std::uint32_t ignore = 0xFFFFFFFF;
        bool back_faces = false;
        bool invisible = false;
    };

    // The options table at index, if there's one there: hit (a name or a list of them), ignore (an object), back_faces
    // and invisible. False if something else is there, or something in it is wrong.
    static bool read_collision_options(lua_State *state, int index, CollisionOptions &options) noexcept {
        auto type = lua.type(state, index);
        if(type == LUA_TNONE || type == LUA_TNIL) {
            return true;
        }
        if(type != LUA_TTABLE) {
            return false;
        }
        bool valid = true;
        auto hit = lua.getfield(state, index, "hit");
        if(hit == LUA_TSTRING) {
            options.hit = hit_flags(state, -1);
            valid = options.hit != 0;
        }
        else if(hit == LUA_TTABLE) {
            options.hit = 0;
            for(long long i = 1; valid; i++) {
                bool end = lua.rawgeti(state, -1, i) == LUA_TNIL;
                auto flags = end ? 0 : hit_flags(state, -1);
                lua.settop(state, -2);
                if(end) {
                    break;
                }
                valid = flags != 0;
                options.hit |= flags;
            }
            valid = valid && options.hit != 0;
        }
        else if(hit != LUA_TNIL) {
            valid = false;
        }
        lua.settop(state, -2);

        auto ignore = lua.getfield(state, index, "ignore");
        if(ignore == LUA_TNUMBER) {
            int is_integer = 0;
            auto value = static_cast<std::uint32_t>(lua.tointegerx(state, -1, &is_integer));
            // An object's ID has its salt's top bit set; anything else is its address, as get_object() and
            // get_dynamic_player() give one
            options.ignore = value & 0x80000000 ? value : object_id_at(value);
            valid = valid && is_integer;
        }
        else if(ignore != LUA_TNIL) {
            valid = false;
        }
        lua.settop(state, -2);

        options.back_faces = option_flag(state, index, "back_faces");
        options.invisible = option_flag(state, index, "invisible");
        return valid;
    }

    // collision_test(x, y, z, dx, dy, dz [, options]) -> where it hit, the surface's normal there, what it hit, its
    // material and the object, or nil if it hits nothing on its way to x + dx, y + dy, z + dz
    static int lua_collision_test(lua_State *state) noexcept {
        double v[6];
        CollisionOptions options;
        if(!numbers(state, 1, 6, v) || !read_collision_options(state, 7, options)) {
            return usage(state, "collision_test(x, y, z, dx, dy, dz [, { hit = <name or names>, ignore = <object>, back_faces = <true>, invisible = <true> }]); "
                "hit is everything (the default), sight, level, water, objects, bipeds, vehicles, weapons, equipment, garbage, projectiles, "
                "scenery, machines, controls, light_fixtures, placeholders or sound_scenery");
        }
        // Surfaces facing the ray, and not invisible ones (player clipping and the like) unless asked
        auto flags = options.hit | COLLISION_FRONT_FACES | (options.back_faces ? COLLISION_BACK_FACES : 0) | (options.invisible ? 0 : COLLISION_SKIP_INVISIBLE);
        const float origin[3] = { static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]) };
        const float delta[3] = { static_cast<float>(v[3]), static_cast<float>(v[4]), static_cast<float>(v[5]) };
        CollisionHit hit;
        if(!collision_test(flags, origin, delta, options.ignore, hit)) {
            lua.pushnil(state);
            return 1;
        }
        for(auto value : hit.point) {
            lua.pushnumber(state, value);
        }
        for(auto value : hit.normal) {
            lua.pushnumber(state, value);
        }
        lua.pushstring(state, HIT_WHAT[hit.what]);
        if(hit.material >= 0 && static_cast<std::size_t>(hit.material) < sizeof(MATERIAL_NAMES) / sizeof(MATERIAL_NAMES[0])) {
            lua.pushstring(state, MATERIAL_NAMES[hit.material]);
        }
        else {
            lua.pushnil(state);
        }
        if(hit.object != 0xFFFFFFFF) {
            lua.pushinteger(state, hit.object);
        }
        else {
            lua.pushnil(state);
        }
        return 9;
    }

    // ---------------------------------------------------------------- HUD elements

    // get_hud_elements() -> { { id, kind, x, y, width, height }, ... } for each element Halo drew last frame
    static int lua_get_hud_elements(lua_State *state) noexcept {
        static HudElement elements[MAXIMUM_LISTED_HUD_ELEMENTS];
        auto count = std::min(hud_elements(elements, MAXIMUM_LISTED_HUD_ELEMENTS), static_cast<std::size_t>(MAXIMUM_LISTED_HUD_ELEMENTS));
        lua.createtable(state, static_cast<int>(count), 0);
        for(std::size_t i = 0; i < count; i++) {
            auto &element = elements[i];
            lua.createtable(state, 0, 6);
            lua.pushinteger(state, element.id);
            lua.setfield(state, -2, "id");
            lua.pushstring(state, hud_element_kind_name(element.kind));
            lua.setfield(state, -2, "kind");
            lua.pushnumber(state, element.x);
            lua.setfield(state, -2, "x");
            lua.pushnumber(state, element.y);
            lua.setfield(state, -2, "y");
            lua.pushnumber(state, element.width);
            lua.setfield(state, -2, "width");
            lua.pushnumber(state, element.height);
            lua.setfield(state, -2, "height");
            lua.rawseti(state, -2, static_cast<long long>(i + 1));
        }
        return 1;
    }

    // The ID at index, as get_hud_elements() gives it
    static bool hud_element_id(lua_State *state, int index, std::uint32_t &id) noexcept {
        int is_integer = 0;
        auto value = lua.tointegerx(state, index, &is_integer);
        id = static_cast<std::uint32_t>(value);
        return is_integer && value >= 0 && value <= 0xFFFFFFFF;
    }

    // set_hud_element(id, { dx, dy, rotate, scale, opacity, hidden }) -> whether Halo has drawn that element on this map
    static int lua_set_hud_element(lua_State *state) noexcept {
        std::uint32_t id;
        auto options = lua.type(state, 2);
        if(!hud_element_id(state, 1, id) || (options != LUA_TTABLE && options != LUA_TNIL && options != LUA_TNONE)) {
            return usage(state, "set_hud_element(id, { dx = <x>, dy = <y>, rotate = <degrees>, scale = <scale>, opacity = <opacity>, hidden = <true> })");
        }
        HudElementChange change;
        if(options == LUA_TTABLE) {
            double value = 0.0;
            option_number(state, 2, "dx", value);
            change.dx = static_cast<float>(value);
            value = 0.0;
            option_number(state, 2, "dy", value);
            change.dy = static_cast<float>(value);
            value = 0.0;
            option_number(state, 2, "rotate", value);
            change.rotation = static_cast<float>(value);
            value = 1.0;
            option_number(state, 2, "scale", value);
            change.scale = static_cast<float>(std::max(value, 0.0));
            value = 1.0;
            option_number(state, 2, "opacity", value);
            change.opacity = static_cast<float>(value);
            change.hidden = option_flag(state, 2, "hidden");
        }
        lua.pushboolean(state, set_hud_element(id, change));
        return 1;
    }

    // reset_hud_elements([id]) -> each element (or that one) drawn the way Halo draws it again
    static int lua_reset_hud_elements(lua_State *state) noexcept {
        auto type = lua.type(state, 1);
        if(type == LUA_TNONE || type == LUA_TNIL) {
            reset_hud_elements();
            lua.pushboolean(state, 1);
            return 1;
        }
        std::uint32_t id;
        if(!hud_element_id(state, 1, id)) {
            return usage(state, "reset_hud_elements([id])");
        }
        lua.pushboolean(state, reset_hud_element(id));
        return 1;
    }

    // ---------------------------------------------------------------- frames

    // What scripts drew since the HUD was last drawn, drawn once and then gone, so it can't pile up into something more
    // opaque whether or not the frame's present is seen
    static void draw_queued(IDirect3DDevice9 *device, double) noexcept {
        if(queued_shapes->empty()) {
            return;
        }
        if(state_block || device->CreateStateBlock(D3DSBT_ALL, &state_block) >= 0) {
            D3DVIEWPORT9 viewport;
            device->GetViewport(&viewport);
            state_block->Capture();
            set_image_states(device);
            set_shape_states(device);
            for(auto &queued : *queued_shapes) {
                draw_shape(device, viewport, queued.shape, queued.x, queued.y, queued.rotation, 1.0F);
            }
            state_block->Apply();
        }
        else {
            state_block = nullptr;
        }
        queued_shapes->clear();
    }

    // Once the frame's done, what scripts drew for it goes, drawn or not (in a frame without the HUD)
    static void end_frame(IDirect3DDevice9 *) noexcept {
        queued_shapes->clear();
        clear_queued_world_shapes();
    }

    static void reset() noexcept {
        if(state_block) {
            state_block->Release();
            state_block = nullptr;
        }
    }

    // ---------------------------------------------------------------- scripts

    static void add_functions(lua_State *state) noexcept {
        static const struct {
            const char *name;
            LuaFunction function;
        } functions[] = {
            { "draw_rectangle", lua_draw_rectangle },
            { "draw_line", lua_draw_line },
            { "draw_line_3d", lua_draw_line_3d },
            { "world_to_screen", lua_world_to_screen },
            { "draw_quad", lua_draw_quad },
            { "draw_circle", lua_draw_circle },
            { "draw_box_3d", lua_draw_box_3d },
            { "draw_circle_3d", lua_draw_circle_3d },
            { "collision_test", lua_collision_test },
            { "get_hud_elements", lua_get_hud_elements },
            { "set_hud_element", lua_set_hud_element },
            { "reset_hud_elements", lua_reset_hud_elements },
        };
        for(auto &function : functions) {
            bool taken = lua.getglobal(state, function.name) != LUA_TNIL;
            lua.settop(state, -2);
            if(!taken) {
                lua.pushcclosure(state, function.function, 0);
                lua.setglobal(state, function.name);
            }
        }
    }

    // Stands in for luaL_openselectedlibs where Chimera makes a script's Lua
    static void open_libraries(lua_State *state, int load, int preload) noexcept {
        lua.openselectedlibs(state, load, preload);
        add_functions(state);
    }

    template<typename T> static bool resolve(HMODULE module, const char *name, T &function) noexcept {
        auto *address = reinterpret_cast<void *>(GetProcAddress(module, name));
        function = reinterpret_cast<T>(address);
        return address != nullptr;
    }

    // Every direct call to target in a module's code
    static std::vector<std::byte *> calls_to(HMODULE module, const void *target) {
        std::vector<std::byte *> calls;
        auto *base = reinterpret_cast<std::byte *>(module);
        auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
        auto *section = reinterpret_cast<const IMAGE_SECTION_HEADER *>(reinterpret_cast<const std::byte *>(&nt->OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader);
        for(WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
            if(!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
                continue;
            }
            auto *start = base + section->VirtualAddress;
            for(std::size_t at = 0; at + 5 <= section->Misc.VirtualSize; at++) {
                if(start[at] != std::byte { 0xE8 }) {
                    continue;
                }
                std::int32_t relative;
                std::memcpy(&relative, start + at + 1, sizeof(relative));
                if(start + at + 5 + relative == target) {
                    calls.push_back(start + at);
                }
            }
        }
        return calls;
    }

    void set_up_lua_drawing(HMODULE module) noexcept {
        if(!module || !hud_drawing_available()) {
            return;
        }
        bool found = resolve(module, "lua_type", lua.type) && resolve(module, "lua_settop", lua.settop)
            && resolve(module, "lua_tonumberx", lua.tonumberx) && resolve(module, "lua_tointegerx", lua.tointegerx)
            && resolve(module, "lua_toboolean", lua.toboolean) && resolve(module, "lua_tolstring", lua.tolstring)
            && resolve(module, "lua_pushnil", lua.pushnil) && resolve(module, "lua_pushnumber", lua.pushnumber)
            && resolve(module, "lua_pushinteger", lua.pushinteger) && resolve(module, "lua_pushstring", lua.pushstring)
            && resolve(module, "lua_pushboolean", lua.pushboolean) && resolve(module, "lua_createtable", lua.createtable)
            && resolve(module, "lua_setfield", lua.setfield) && resolve(module, "lua_rawseti", lua.rawseti)
            && resolve(module, "lua_pushcclosure", lua.pushcclosure) && resolve(module, "lua_getglobal", lua.getglobal)
            && resolve(module, "lua_setglobal", lua.setglobal) && resolve(module, "lua_getfield", lua.getfield)
            && resolve(module, "lua_rawgeti", lua.rawgeti) && resolve(module, "luaL_error", lua.error)
            && resolve(module, "luaL_openselectedlibs", lua.openselectedlibs);
        if(!found) {
            return;
        }
        auto calls = calls_to(module, reinterpret_cast<const void *>(lua.openselectedlibs));
        if(calls.empty()) {
            return;
        }
        for(auto *call : calls) {
            retarget_call(call, reinterpret_cast<const void *>(open_libraries));
        }
        add_hud_draw_callback(draw_queued);
        add_present_callback(end_frame);
        add_reset_callback(reset);
    }

    void set_up_lua_drawing() noexcept {
        set_up_lua_drawing(GetModuleHandleA("strings.dll"));
    }
}
