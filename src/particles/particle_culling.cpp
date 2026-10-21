// SPDX-License-Identifier: GPL-3.0-only

#include "particle_culling.hpp"
#include "../memory.hpp"

#include <cstring>

namespace Buh {
    static float particle_size_threshold = 0.0F;
    static float large_sprite_coverage = 1.0E30F;

    static void stop_visibility_culling() noexcept {
        auto *reaper = find_signature({ 0xA1, -1, -1, -1, -1, 0x2B, 0x41, 0x10, 0x83, 0xF8, 0x0F, 0x7E, 0x0B, 0x8B, 0xD7, 0x8B, 0xC3, 0xE8 });
        auto *system_gate = find_signature({ 0x0F, 0xBF, 0x40, 0x1C, 0x8B, 0xC8, 0x83, 0xE1, 0x1F, 0xBA, 0x01, 0x00, 0x00, 0x00, 0xD3, 0xE2, 0x8B, 0x0D, -1, -1, -1, -1, 0xC1, 0xF8, 0x05, 0x85, 0x54, 0x81, 0x58, 0x74, -1, 0x8B, 0xC6, 0xE8 });
        auto *cluster_cull = find_signature({ 0xBA, 0x01, 0x00, 0x00, 0x00, 0xD3, 0xE2, 0xC1, 0xF8, 0x05, 0x85, 0x14, 0x85, -1, -1, -1, -1, 0x0F, 0x84, -1, -1, -1, -1 });
        auto *effect_gate = find_signature({ 0x66, 0x83, 0x7B, 0x14, 0xFF, 0x74, 0x3F, 0xF6, 0x06, 0x04, 0x0F, 0xBF, 0x43, 0x14, 0xBA, 0x01 });
        auto *spawn_gate = find_signature({ 0x0F, 0x84, 0xFB, 0x02, 0x00, 0x00, 0x8B, 0x15, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x83, 0xF8, 0xFF });
        if(!reaper || !system_gate || !cluster_cull || !effect_gate || !spawn_gate) {
            return;
        }

        // Particles undrawn for 15 frames get deleted, which is milliseconds at modern frame rates (jle -> jmp)
        overwrite(reaper + 11, static_cast<std::uint8_t>(0xEB));

        // particle_systems_render() drops a whole system on one cached-location PVS test
        system_gate += 29;
        if(*reinterpret_cast<std::int8_t *>(system_gate + 1) > 0) {
            fill_with_nops(system_gate, 2);
        }

        // particle_system_render() then skips particles whose cluster isn't in the rendered set
        cluster_cull += 17;
        std::int32_t cluster_skip;
        std::memcpy(&cluster_skip, cluster_cull + 2, sizeof(cluster_skip));
        if(cluster_skip > 0) {
            fill_with_nops(cluster_cull, 6);
        }

        // effect_update() deletes a one-shot effect the first time it leaves the PVS, so off-screen explosions die at
        // birth. Jump to the visible path instead.
        static const std::uint8_t visible_path[] = { 0x33, 0xC0, 0x66, 0x8B, 0x43, 0x02 };
        if(std::memcmp(effect_gate + 0x5D, visible_path, sizeof(visible_path)) == 0) {
            static const std::uint8_t jmp_visible[] = { 0xE9, 0x58, 0x00, 0x00, 0x00, 0x90, 0x90 };
            overwrite(effect_gate, jmp_visible, sizeof(jmp_visible));
        }

        // particle_new() refuses to spawn particles outside the PVS (the BSP leaf check before it stays)
        fill_with_nops(spawn_gate, 6);
    }

    void set_up_particle_culling_fix() noexcept {
        stop_visibility_culling();

        // Particles under their tag's apparent size threshold flicker in and out with distance
        if(auto *size_cull = find_signature({ 0xD9, 0x44, 0x24, -1, 0xD8, 0x9D, 0x90, 0x00, 0x00, 0x00, 0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x0F, 0x85, -1, -1, -1, -1, 0xD9, 0x44, 0x24, -1 })) {
            std::uint8_t fcomp[6] = { 0xD8, 0x1D }; // fcomp dword ptr [ebp + 0x90] -> fcomp dword ptr [particle_size_threshold]
            auto address = reinterpret_cast<std::uint32_t>(&particle_size_threshold);
            std::memcpy(fcomp + 2, &address, sizeof(address));
            overwrite(size_cull + 4, fcomp, sizeof(fcomp));
        }

        // Only 11 half-screen sprites are drawn per frame, which tears smoke apart from the inside
        if(auto *large_sprite_cull = find_signature({ 0xD9, 0x05, -1, -1, -1, -1, 0xD8, 0xC1, 0xD9, 0x1D, -1, -1, -1, -1, 0xD8, 0x1D, -1, -1, -1, -1, 0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x75, -1, 0x66, 0xA1, -1, -1, -1, -1, 0x66, 0x8B, 0xC8, 0x66, 0x40, 0x66, 0x83, 0xF9, -1, 0x66, 0xA3, -1, -1, -1, -1, 0x7E, -1 })) {
            overwrite(large_sprite_cull + 16, reinterpret_cast<std::uint32_t>(&large_sprite_coverage));
        }
    }
}
