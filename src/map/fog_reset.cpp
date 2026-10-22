// SPDX-License-Identifier: GPL-3.0-only
//
// Derived from Balltze's fog plane reset fix.

#include "fog_reset.hpp"
#include "map_load.hpp"
#include "../memory.hpp"

#include <cstring>

namespace Buh {
    // The fog Halo is rendering with, in its render globals
    struct RenderFog {
        std::uint16_t definition_flags;
        std::uint16_t runtime_flags;
        float atmospheric_color[3];
        float atmospheric_maximum_density;
        float atmospheric_minimum_distance;
        float atmospheric_maximum_distance;
        std::int16_t planar_mode;
        std::uint16_t padding;
        float plane[4];
        float planar_color[3];
        float planar_maximum_density;
        float planar_maximum_distance;
        float planar_maximum_depth;
        void *screen;
        float screen_external_intensity;
    };
    static_assert(sizeof(RenderFog) == 0x50);

    static RenderFog *fog = nullptr;

    static void before_map_load(const char *) noexcept {
        std::memset(fog->plane, 0, sizeof(fog->plane));
        std::memset(fog->planar_color, 0, sizeof(fog->planar_color));
        fog->planar_maximum_density = 0.0F;
        fog->planar_maximum_depth = 0.0F;
    }

    void set_up_fog_reset() noexcept {
        // Where Halo copies the frame's fog in: mov ecx, 0x14; mov esi, edx; mov edi, <fog>; rep movsd
        auto *copy = find_signature({ 0x83, 0xEC, 0x48, 0x56, 0x57, 0xB9, 0x14, 0x00, 0x00, 0x00, 0x8B, 0xF2, 0xBF, -1, -1, -1, -1, 0xF3, 0xA5 });
        if(!copy) {
            return;
        }
        std::uint32_t address;
        std::memcpy(&address, copy + 13, sizeof(address));
        fog = reinterpret_cast<RenderFog *>(address);
        add_before_map_load_callback(before_map_load);
    }
}
