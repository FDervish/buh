// SPDX-License-Identifier: GPL-3.0-only

#include "hud_skew.hpp"
#include "../console.hpp"
#include "../map/map_load.hpp"
#include "../memory.hpp"
#include "../render.hpp"

#include <algorithm>
#include <cmath>
#include <string>

// Elements centred closer than this to the middle (as a fraction of half the canvas), like the crosshair, stay put
#define CENTER_FRACTION 0.3F

#define MAXIMUM_DEGREES 45.0F

namespace Buh {
    struct QuadVertex {
        float x;
        float y;
        float z;
        float rhw;
        float u;
        float v;
    };

    static float degrees = 0.0F;
    static float slope = 0.0F;
    static std::byte *bitmap_draw_call = nullptr;
    static std::byte *navpoint_draw = nullptr;
    static bool navpoint_hooked = false;

    extern "C" {
        std::uint8_t buh_drawing_navpoint = 0;
        const void *buh_hud_bitmap_draw = nullptr;
        const void *buh_navpoint_draw_continue = nullptr;
        void buh_hud_bitmap_draw_asm() noexcept;
        void buh_navpoint_draw_asm() noexcept;
    }

    // Each side of the HUD is sheared as one, about a point halfway out, so its elements stay lined up and only
    // their inner edges dip (or rise, with a negative angle)
    extern "C" void buh_skew_hud_quad(QuadVertex *quad) noexcept {
        if(slope == 0.0F || buh_drawing_navpoint) {
            return;
        }
        float middle = hud_canvas_width() / 2.0F;
        float left = quad[0].x, right = quad[0].x, center = 0.0F;
        for(int i = 0; i < 4; i++) {
            left = std::min(left, quad[i].x);
            right = std::max(right, quad[i].x);
            center += quad[i].x / 4.0F;
        }
        // Overlays that span the screen aren't HUD elements on a side
        if(std::fabs(center - middle) < middle * CENTER_FRACTION || right - left > middle) {
            return;
        }
        bool right_side = center > middle;
        float pivot = right_side ? middle * 1.5F : middle * 0.5F;
        float side_slope = right_side ? -slope : slope;
        for(int i = 0; i < 4; i++) {
            quad[i].y += (quad[i].x - pivot) * side_slope;
        }
    }

    // Safe to repeat: Chimera's widescreen fix moves the call into its own hook while it's on, and puts the
    // original back when it's turned off
    static void install() noexcept {
        auto *call = bitmap_draw_call;
        if(static_cast<std::uint8_t>(*call) == 0xE9) {
            call = branch_target(call) + 5;
        }
        if(static_cast<std::uint8_t>(*call) != 0xE8) {
            return;
        }
        auto *target = branch_target(call);
        if(target != reinterpret_cast<const std::byte *>(buh_hud_bitmap_draw_asm)) {
            buh_hud_bitmap_draw = target;
            retarget_call(call, reinterpret_cast<const void *>(buh_hud_bitmap_draw_asm));
        }
        if(!navpoint_hooked) {
            write_jump(navpoint_draw, reinterpret_cast<const void *>(buh_navpoint_draw_asm), 8);
            navpoint_hooked = true;
        }
    }

    static void skew_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty()) {
            float value;
            if(!parse_float(arguments[0], value) || std::fabs(value) > MAXIMUM_DEGREES) {
                console_error("usage: buh_hud_skew [degrees], from -45 to 45, 0 is off");
                return;
            }
            degrees = value;
            slope = std::tan(degrees * 3.14159265F / 180.0F);
            if(slope != 0.0F) {
                install();
            }
        }
        console_print(("buh_hud_skew: " + std::to_string(degrees)).c_str());
    }

    void set_up_hud_skew() noexcept {
        // The HUD bitmap function's call to draw its quad, with the quad as its argument; the call itself may have
        // been replaced by Chimera's widescreen fix, so it's left out of the pattern
        auto *bitmap_draw = find_signature({ -1, -1, -1, -1, -1, 0x83, 0xC4, 0x04, 0x5F, 0x5D, 0x5B, 0x81, 0xC4, 0x10, 0x01, 0x00, 0x00, 0xC3 });
        // The start of the navpoint drawing function: sub esp, 0xA8; push ebx; push ebp; movsx ebp, [esp + 0xB8]; ...
        auto *navpoint = find_signature({ 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x53, 0x55, 0x0F, 0xBF, 0xAC, 0x24, 0xB8, 0x00, 0x00, 0x00, 0x6B, 0xED, 0x68, 0x56, 0x8B, 0xF0 });
        if(!bitmap_draw || !navpoint) {
            return;
        }
        bitmap_draw_call = bitmap_draw;
        navpoint_draw = navpoint;
        buh_navpoint_draw_continue = navpoint + 8;
        add_after_map_load_callback([](const char *) noexcept {
            if(slope != 0.0F) {
                install();
            }
        });
        add_console_command("buh_hud_skew", "[degrees]", 0, 1, skew_command);
    }
}
