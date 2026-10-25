// SPDX-License-Identifier: GPL-3.0-only
//
// Changing how single HUD elements are drawn. Halo draws each element its HUD tags define (static elements, meters,
// numbers, and weapons' overlays and crosshairs) with a function that's given the element's definition, and each quad
// any of them draws goes through the one function that puts quads on the screen. The element functions are entered
// through buh, which notes what's being drawn until they return, so each quad can be put down to its element. An
// element is known by where its definition is in the map's tag data, which stays put for as long as the map is loaded.

#include "hud_elements.hpp"
#include "../console.hpp"
#include "../halo.hpp"
#include "../map/map_load.hpp"
#include "../memory.hpp"
#include "../render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Element functions called from element functions, as a static element's is from its own and a meter's from its own
#define MAXIMUM_SCOPES 8

#define MAXIMUM_LISTED 256

#define PI 3.14159265F

namespace Buh {
    // A corner of a quad, as Halo's PC build lays them out
    struct QuadVertex {
        float x;
        float y;
        float z;
        std::uint32_t color;
        float u;
        float v;
    };

    struct Scope {
        const void *return_address;
        std::uint32_t element;
        HudElement::Kind kind;
    };

    struct Tracked {
        HudElement::Kind kind = HudElement::STATIC;
        // Where Halo put it in the HUD's canvas this frame and last frame: left, top, right and bottom
        float drawing[4] = {};
        float drawn[4] = {};
        bool drawing_now = false;
        bool visible = false;
        bool changed = false;
        HudElementChange change;
    };

    extern "C" {
        const void *buh_hud_quad_continue = nullptr;
        const void *buh_hud_bitmap_continue = nullptr;
        const void *buh_hud_static_continue = nullptr;
        const void *buh_hud_numbers_continue = nullptr;
        const void *buh_hud_meter_continue = nullptr;
        // What the first instructions of the static element and numbers functions read
        const void *buh_hud_static_global = nullptr;
        const void *buh_hud_numbers_global = nullptr;
        void buh_hud_quad_asm() noexcept;
        void buh_hud_bitmap_scope_asm() noexcept;
        void buh_hud_static_scope_asm() noexcept;
        void buh_hud_numbers_scope_asm() noexcept;
        void buh_hud_meter_scope_asm() noexcept;
        void buh_hud_scope_exit_asm() noexcept;
    }

    static Scope scopes[MAXIMUM_SCOPES];
    static std::size_t depth = 0;
    // Never freed, so nothing runs while Windows unloads DLLs
    static auto *tracked = new std::map<std::uint32_t, Tracked>;
    static bool available = false;

    extern "C" void buh_hud_enter(const void **return_address, std::uint32_t element, int kind) noexcept {
        if(depth == MAXIMUM_SCOPES) {
            return;
        }
        scopes[depth++] = { *return_address, element, static_cast<HudElement::Kind>(kind) };
        *return_address = reinterpret_cast<const void *>(buh_hud_scope_exit_asm);
    }

    extern "C" const void *buh_hud_leave() noexcept {
        return scopes[--depth].return_address;
    }

    static bool is_changed(const HudElementChange &change) noexcept {
        return change.dx != 0.0F || change.dy != 0.0F || change.rotation != 0.0F || change.scale != 1.0F || change.opacity < 1.0F || change.hidden;
    }

    // A quad Halo's about to draw: if it's part of an element, where the element is so far, and the element's change
    extern "C" void buh_hud_quad(QuadVertex *quad) noexcept {
        // The innermost element whose definition is in the tag data (numbers in HUD messages and navpoints are made
        // up on the stack), drawn as the outermost function drawing it draws it
        std::uint32_t element = 0;
        for(auto i = depth; i-- > 0;) {
            if(scopes[i].element && in_tag_space(reinterpret_cast<const void *>(scopes[i].element))) {
                element = scopes[i].element;
                break;
            }
        }
        if(!element) {
            return;
        }
        auto [entry, added] = tracked->try_emplace(element);
        auto &t = entry->second;
        if(added) {
            t.kind = scopes[0].kind;
        }
        for(int i = 0; i < 4; i++) {
            if(!t.drawing_now) {
                t.drawing[0] = t.drawing[2] = quad[i].x;
                t.drawing[1] = t.drawing[3] = quad[i].y;
                t.drawing_now = true;
            }
            t.drawing[0] = std::min(t.drawing[0], quad[i].x);
            t.drawing[1] = std::min(t.drawing[1], quad[i].y);
            t.drawing[2] = std::max(t.drawing[2], quad[i].x);
            t.drawing[3] = std::max(t.drawing[3], quad[i].y);
        }
        if(!t.changed) {
            return;
        }

        auto &change = t.change;
        if(change.hidden) {
            for(int i = 1; i < 4; i++) {
                quad[i].x = quad[0].x;
                quad[i].y = quad[0].y;
            }
            return;
        }
        // About the middle of where the whole element was last frame, so its quads (a number's digits, a static
        // element's overlays) turn and scale together
        const float *about = t.visible ? t.drawn : t.drawing;
        float middle_x = (about[0] + about[2]) / 2.0F;
        float middle_y = (about[1] + about[3]) / 2.0F;
        float across = hud_canvas_width() / 640.0F;
        float radians = change.rotation * PI / 180.0F;
        float cosine = std::cos(radians) * change.scale;
        float sine = std::sin(radians) * change.scale;
        float opacity = std::clamp(change.opacity, 0.0F, 1.0F);
        for(int i = 0; i < 4; i++) {
            float x = quad[i].x - middle_x;
            float y = quad[i].y - middle_y;
            quad[i].x = middle_x + x * cosine - y * sine + change.dx * across;
            quad[i].y = middle_y + x * sine + y * cosine + change.dy;
            if(opacity < 1.0F) {
                auto alpha = static_cast<std::uint32_t>(static_cast<float>(quad[i].color >> 24) * opacity + 0.5F);
                quad[i].color = (quad[i].color & 0xFFFFFF) | alpha << 24;
            }
        }
    }

    // A new map's elements are somewhere else
    static void forget_elements(const char *) noexcept {
        tracked->clear();
    }

    // Once the HUD's drawn, what was drawn this frame is what was drawn last frame
    static void end_frame(IDirect3DDevice9 *, double) noexcept {
        for(auto &[id, t] : *tracked) {
            t.visible = t.drawing_now;
            if(t.drawing_now) {
                std::copy(t.drawing, t.drawing + 4, t.drawn);
            }
            t.drawing_now = false;
        }
    }

    bool hud_elements_available() noexcept {
        return available;
    }

    std::size_t hud_elements(HudElement *elements, std::size_t maximum) noexcept {
        float across = 640.0F / hud_canvas_width();
        std::size_t count = 0;
        for(auto &[id, t] : *tracked) {
            if(!t.visible) {
                continue;
            }
            if(count < maximum) {
                elements[count] = { id, t.kind, t.drawn[0] * across, t.drawn[1], (t.drawn[2] - t.drawn[0]) * across, t.drawn[3] - t.drawn[1] };
            }
            count++;
        }
        return count;
    }

    bool set_hud_element(std::uint32_t id, const HudElementChange &change) noexcept {
        auto found = tracked->find(id);
        if(found == tracked->end()) {
            return false;
        }
        found->second.change = change;
        found->second.changed = is_changed(change);
        return true;
    }

    bool reset_hud_element(std::uint32_t id) noexcept {
        return set_hud_element(id, {});
    }

    void reset_hud_elements() noexcept {
        for(auto &[id, t] : *tracked) {
            t.change = {};
            t.changed = false;
        }
    }

    const char *hud_element_kind_name(HudElement::Kind kind) noexcept {
        static const char *const NAMES[] = { "static", "meter", "number", "overlay" };
        return NAMES[kind];
    }

    // ---------------------------------------------------------------- commands

    static bool parse_id(const std::string &text, std::uint32_t &id) noexcept {
        char *end = nullptr;
        auto value = std::strtoul(text.c_str(), &end, 0);
        if(text.empty() || *end != '\0') {
            return false;
        }
        id = static_cast<std::uint32_t>(value);
        return true;
    }

    static void list_command(const std::vector<std::string> &) {
        static HudElement elements[MAXIMUM_LISTED];
        auto count = std::min(hud_elements(elements, MAXIMUM_LISTED), static_cast<std::size_t>(MAXIMUM_LISTED));
        if(count == 0) {
            console_print("buh_hud_elements: none were drawn last frame");
            return;
        }
        for(std::size_t i = 0; i < count; i++) {
            auto &element = elements[i];
            char line[128];
            std::snprintf(line, sizeof(line), "0x%08X %-7s at %.0f, %.0f, %.0f by %.0f", static_cast<unsigned>(element.id), hud_element_kind_name(element.kind),
                element.x, element.y, element.width, element.height);
            console_print(line);
        }
    }

    #define ELEMENT_OPTIONS "[offset <dx> <dy>] [rotate <degrees>] [scale <scale>] [opacity <opacity>] [hide]"

    static void element_command(const std::vector<std::string> &arguments) {
        std::uint32_t id = 0;
        HudElementChange change;
        bool valid = parse_id(arguments[0], id);
        for(std::size_t i = 1; valid && i < arguments.size(); i++) {
            auto &option = arguments[i];
            auto values = arguments.size() - i - 1;
            if(option == "offset" && values >= 2 && parse_float(arguments[i + 1], change.dx) && parse_float(arguments[i + 2], change.dy)) {
                i += 2;
            }
            else if(option == "rotate" && values >= 1 && parse_float(arguments[i + 1], change.rotation)) {
                i++;
            }
            else if(option == "scale" && values >= 1 && parse_float(arguments[i + 1], change.scale) && change.scale >= 0.0F) {
                i++;
            }
            else if(option == "opacity" && values >= 1 && parse_float(arguments[i + 1], change.opacity)) {
                i++;
            }
            else if(option == "hide") {
                change.hidden = true;
            }
            else {
                valid = false;
            }
        }
        if(!valid) {
            console_error("usage: buh_hud_element <id> " ELEMENT_OPTIONS);
            return;
        }
        if(!set_hud_element(id, change)) {
            console_error("buh_hud_element: Halo hasn't drawn that element on this map (buh_hud_elements lists them)");
        }
    }

    static void reset_command(const std::vector<std::string> &arguments) {
        if(arguments.empty()) {
            reset_hud_elements();
            return;
        }
        std::uint32_t id;
        if(!parse_id(arguments[0], id)) {
            console_error("usage: buh_hud_element_reset [id]");
            return;
        }
        if(!reset_hud_element(id)) {
            console_error("buh_hud_element_reset: Halo hasn't drawn that element on this map");
        }
    }

    template<typename T> static T read_address(const std::byte *at) noexcept {
        std::uint32_t address;
        std::memcpy(&address, at, sizeof(address));
        return reinterpret_cast<T>(address);
    }

    void set_up_hud_elements() noexcept {
        if(!hud_drawing_available()) {
            return;
        }
        // rasterizer_psuedo_dynamic_screen_quad_draw: sub esp, 0x148; push esi; mov esi, eax; ..., and it only draws
        // into the player's view
        auto *quad = find_signature({ 0x81, 0xEC, 0x48, 0x01, 0x00, 0x00, 0x56, 0x8B, 0xF0, 0xA0, -1, -1, -1, -1, 0x84, 0xC0, 0x0F, 0x84, -1, -1, -1, -1, 0x66, 0x83, 0x3D, -1, -1, -1, -1, 0x01 });
        // hud_draw_bitmap_with_meter: sub esp, 0x28; test bl, bl; push ebp; mov ebp, [esp + 0x38]; ...
        auto *bitmap = find_signature({ 0x83, 0xEC, 0x28, 0x84, 0xDB, 0x55, 0x8B, 0x6C, 0x24, 0x38, 0x56, 0x8B, 0x74, 0x24, 0x38, 0x57, 0x8B, 0xF8 });
        // hud_draw_static_element: sub esp, 0x48; mov edx, [tag instances]; push ebp; mov ebp, [esp + 0x58] (the element)
        auto *static_element = find_signature({ 0x83, 0xEC, 0x48, 0x8B, 0x15, -1, -1, -1, -1, 0x55, 0x8B, 0x6C, 0x24, 0x58, 0x8B, 0x45, 0x30, 0x8B, 0xC8, 0x81, 0xE1, 0xFF, 0xFF, 0x00, 0x00 });
        // hud_draw_numbers: mov eax, [globals]; mov ecx, [eax + 0x140]; sub esp, 0x28; ...
        auto *numbers = find_signature({ 0xA1, -1, -1, -1, -1, 0x8B, 0x88, 0x40, 0x01, 0x00, 0x00, 0x83, 0xEC, 0x28, 0x85, 0xC9, 0x55, 0x8B, 0x6C, 0x24, 0x3C });
        // hud_draw_meter: sub esp, 0xC4; push esi; mov esi, [...]; test byte [esi + 0x78], 1; ...
        auto *meter = find_signature({ 0x81, 0xEC, 0xC4, 0x00, 0x00, 0x00, 0x56, 0x8B, 0x35, -1, -1, -1, -1, 0xF6, 0x46, 0x78, 0x01, 0x0F, 0x84, -1, -1, -1, -1, 0x83, 0xF8, 0xFF });
        if(!quad || !bitmap || !static_element || !numbers || !meter) {
            return;
        }
        buh_hud_static_global = read_address<const void *>(static_element + 5);
        buh_hud_numbers_global = read_address<const void *>(numbers + 1);
        buh_hud_quad_continue = quad + 6;
        buh_hud_bitmap_continue = bitmap + 5;
        buh_hud_static_continue = static_element + 9;
        buh_hud_numbers_continue = numbers + 5;
        buh_hud_meter_continue = meter + 6;
        write_jump(quad, reinterpret_cast<const void *>(buh_hud_quad_asm), 6);
        write_jump(bitmap, reinterpret_cast<const void *>(buh_hud_bitmap_scope_asm), 5);
        write_jump(static_element, reinterpret_cast<const void *>(buh_hud_static_scope_asm), 9);
        write_jump(numbers, reinterpret_cast<const void *>(buh_hud_numbers_scope_asm), 5);
        write_jump(meter, reinterpret_cast<const void *>(buh_hud_meter_scope_asm), 6);
        add_hud_draw_callback(end_frame);
        add_after_map_load_callback(forget_elements);
        add_console_command("buh_hud_elements", "", 0, 0, list_command);
        add_console_command("buh_hud_element", "<id> " ELEMENT_OPTIONS, 1, 16, element_command);
        add_console_command("buh_hud_element_reset", "[id]", 0, 1, reset_command);
        available = true;
    }
}
