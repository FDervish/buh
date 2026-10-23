// SPDX-License-Identifier: GPL-3.0-only
//
// Drawing for Chimera's Lua scripts: rectangles, lines, quads and circles on the HUD, lines, boxes and circles in the
// world, and world_to_screen, each drawn in the frame a script draws it.
//
// Chimera exports every one of its Lua functions by name, and makes each script's Lua with one call to
// luaL_openselectedlibs. That call is pointed here instead: once Chimera's libraries are open, buh adds its functions
// through Chimera's own Lua. Nothing of Chimera's is matched by its bytes, and a Chimera that doesn't export its Lua
// just doesn't get them. Chimera adds its own functions after, so where it has one by the same name, it's Chimera's.

#include "lua_draw.hpp"
#include "../memory.hpp"
#include "../render.hpp"
#include "../sprites/image.hpp"
#include "../sprites/shapes.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#define LUA_TNONE (-1)
#define LUA_TNIL 0
#define LUA_TBOOLEAN 1
#define LUA_TNUMBER 3
#define LUA_TTABLE 5

// The HUD's units, which scripts draw in: 640 across and 480 down, whatever the screen's shape
#define HUD_WIDTH 640.0
#define HUD_HEIGHT 480.0

#define MAXIMUM_QUEUED_SHAPES 16384

struct lua_State;

namespace Buh {
    using LuaFunction = int (*)(lua_State *state);

    // The Lua functions used here, Chimera's own
    static struct {
        int (*type)(lua_State *, int);
        void (*settop)(lua_State *, int);
        double (*tonumberx)(lua_State *, int, int *);
        int (*toboolean)(lua_State *, int);
        void (*pushnil)(lua_State *);
        void (*pushnumber)(lua_State *, double);
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
            && resolve(module, "lua_tonumberx", lua.tonumberx) && resolve(module, "lua_toboolean", lua.toboolean)
            && resolve(module, "lua_pushnil", lua.pushnil) && resolve(module, "lua_pushnumber", lua.pushnumber)
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
