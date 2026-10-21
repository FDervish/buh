// SPDX-License-Identifier: GPL-3.0-only

#include "extend_effect_limits.hpp"
#include "../game_state.hpp"
#include "../memory.hpp"

#include <cstring>
#include <memory>

// An imm8, hence 127. Stock is 64.
#define PARTICLE_SYSTEM_LIMIT 127

#define DATUM_ARRAY_SIGNATURE 0x64407440

namespace Buh {
    // Stock is 256 effects, 512 effect locations, 512 particle system particles, 2048 decals, 8 glows and 512 glow particles
    struct EffectLimits {
        std::uint32_t effects;
        std::uint32_t effect_locations;
        std::uint32_t particle_system_particles;
        std::uint32_t decals;
        std::uint32_t glows;
        std::uint32_t glow_particles;
    };

    // Everything but the particle array lives in the game state, which has no bounds check
    static constexpr EffectLimits STOCK_GAME_STATE_LIMITS = { 726, 1280, 512, 2048, 8, 512 };
    static constexpr EffectLimits LARGE_GAME_STATE_LIMITS = { 2048, 4096, 4096, 4096, 64, 1024 };

    // A decal also takes a 28 byte block of the decal vertex cache
    static constexpr std::size_t extra_game_state_bytes(const EffectLimits &limits) {
        return (limits.effects - 256) * 0xFC + (limits.effect_locations - 512) * 0x3C + (PARTICLE_SYSTEM_LIMIT - 64) * 0x158
            + (limits.particle_system_particles - 512) * 0x80 + (limits.decals - 2048) * (0x38 + 0x1C)
            + (limits.glows - 8) * 0x25C + (limits.glow_particles - 512) * 0x64;
    }

    // The stock game state has about 310 KiB to spare; the enlarged one is counted as if the stock arrays filled all of the stock size
    static_assert(extra_game_state_bytes(STOCK_GAME_STATE_LIMITS) <= 310 * 1024);
    static_assert(extra_game_state_bytes(LARGE_GAME_STATE_LIMITS) <= GAME_STATE_SIZE - GAME_STATE_STOCK_SIZE);

    // Halo never frees game state arrays, so this one lives for the whole process and is reset on each map load
    static std::unique_ptr<std::byte []> particle_array;
    static std::size_t particle_array_size = 0;

    // render_particles() keeps these on its stack, sized for the stock limits
    static auto rendered_particles = std::make_unique<std::byte []>(PARTICLE_LIMIT * 8 + 8);
    static auto group_particle_counts = std::make_unique<std::byte []>(PARTICLE_LIMIT * 2 + 8);

    extern "C" void *extend_effect_limits_data_new(const char *name, std::int32_t maximum_count, std::int32_t size) noexcept {
        std::size_t bytes = sizeof(DatumArray) + static_cast<std::size_t>(maximum_count) * static_cast<std::size_t>(size);
        if(particle_array_size < bytes) {
            particle_array = std::make_unique<std::byte []>(bytes);
            particle_array_size = bytes;
        }

        // The arena starts zeroed, so this has to as well or the previous map's datum identifiers carry over
        auto *data = particle_array.get();
        std::memset(data, 0, bytes);
        auto *array = reinterpret_cast<DatumArray *>(data);
        std::strncpy(array->name, name, sizeof(array->name) - 1);
        array->maximum_count = static_cast<std::uint16_t>(maximum_count);
        array->element_size = static_cast<std::uint16_t>(size);
        array->signature = DATUM_ARRAY_SIGNATURE;
        array->elements = data + sizeof(DatumArray);
        return array;
    }

    extern "C" void extend_effect_limits_data_new_asm() noexcept;

    static void write_lea_indexed(std::byte *at, std::uint8_t modrm, std::uint8_t sib, const void *address) noexcept {
        std::uint8_t code[7] = { 0x8D, modrm, sib };
        auto value = reinterpret_cast<std::uint32_t>(address);
        std::memcpy(code + 3, &value, sizeof(value));
        overwrite(at, code, sizeof(code));
    }

    static void write_lea_absolute(std::byte *at, std::uint8_t modrm, const void *address) noexcept {
        std::uint8_t code[7] = { 0x8D, modrm, 0, 0, 0, 0, 0x90 };
        auto value = reinterpret_cast<std::uint32_t>(address);
        std::memcpy(code + 2, &value, sizeof(value));
        overwrite(at, code, sizeof(code));
    }

    // Particle system particles, decals and glows only grow with the enlarged game state
    static bool extend_game_state_arrays(const EffectLimits &limits) noexcept {
        auto *psp_count = find_signature({ 0x68, 0x00, 0x02, 0x00, 0x00, 0x68, -1, -1, -1, -1, 0xBB, 0x80, 0x00, 0x00, 0x00, 0xA3, -1, -1, -1, -1, 0xE8 });
        auto *decal_count = find_signature({ 0x51, 0x53, 0x56, 0x68, 0x00, 0x08, 0x00, 0x00, 0x68, -1, -1, -1, -1, 0xBB, 0x38, 0x00, 0x00, 0x00, 0xE8 });
        auto *decal_vertex_buffer = find_signature({ 0x8D, 0x7C, 0x24, -1, 0x57, 0x52, 0x6A, 0x00, 0x50, 0x68, 0x00, 0xC0, 0x03, 0x00, 0x51, 0xFF, 0x56, 0x68 });
        auto *decal_cache = find_signature({ 0xC7, 0x44, 0x24, -1, 0x7C, 0xE0, 0x00, 0x00, 0x49, 0x23, 0xCA, 0x8B, 0x15, -1, -1, -1, -1, 0x8B, 0xC1, 0xA3, -1, -1, -1, -1, 0xA1, -1, -1, -1, -1, 0x8D, 0x34, 0x10, 0x05, 0x7C, 0xE0, 0x00, 0x00, 0xA3, -1, -1, -1, -1, 0x8D, 0x44, 0x24, -1, 0x50, 0x68, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x68, -1, -1, -1, -1, 0x68, -1, -1, -1, -1, 0x68, 0x00, 0x08, 0x00, 0x00, 0x6A, 0x06, 0x68, 0x00, 0x0A, 0x00, 0x00 });
        auto *glow_count = find_signature({ 0x53, 0x6A, 0x08, 0x68, -1, -1, -1, -1, 0xBB, 0x5C, 0x02, 0x00, 0x00, 0xE8 });
        auto *glow_particle_count = find_signature({ 0x68, 0x00, 0x02, 0x00, 0x00, 0x68, -1, -1, -1, -1, 0xBB, 0x64, 0x00, 0x00, 0x00, 0xE8 });
        if(!psp_count || !decal_count || !decal_vertex_buffer || !decal_cache || !glow_count || !glow_particle_count) {
            return false;
        }

        overwrite(psp_count + 1, limits.particle_system_particles);
        overwrite(glow_count + 2, static_cast<std::uint8_t>(limits.glows));
        overwrite(glow_particle_count + 1, limits.glow_particles);

        // The decal vertex cache hands out 64 byte pages of the decal vertex buffer and evicts the oldest decal when
        // full, so both grow with the count, at the engine's 1.25 pages and 120 buffer bytes per decal. The cache is
        // carved straight out of the game state, so its size is both recorded and bumped.
        auto decal_cache_bytes = static_cast<std::uint32_t>(0x7C + limits.decals * 0x1C);
        overwrite(decal_count + 4, limits.decals);
        overwrite(decal_vertex_buffer + 10, static_cast<std::uint32_t>(limits.decals * 120));
        overwrite(decal_cache + 4, decal_cache_bytes);
        overwrite(decal_cache + 33, decal_cache_bytes);
        overwrite(decal_cache + 68, limits.decals);
        overwrite(decal_cache + 75, static_cast<std::uint32_t>(limits.decals * 5 / 4));
        return true;
    }

    void set_up_extend_effect_limits(bool large_game_state) noexcept {
        auto *particle_count = find_signature({ 0x68, 0x00, 0x04, 0x00, 0x00, 0x68, -1, -1, -1, -1, 0xBB, 0x70, 0x00, 0x00, 0x00, 0xE8 });
        auto *particle_systems_count = find_signature({ 0x6A, 0x40, 0x68, -1, -1, -1, -1, 0xBB, 0x58, 0x01, 0x00, 0x00, 0xA3, -1, -1, -1, -1, 0xE8 });
        auto *effect_count = find_signature({ 0x68, 0x00, 0x01, 0x00, 0x00, 0x68, -1, -1, -1, -1, 0xBB, 0xFC, 0x00, 0x00, 0x00, 0xA3, -1, -1, -1, -1, 0xE8 });
        auto *effect_location_count = find_signature({ 0x68, 0x00, 0x02, 0x00, 0x00, 0x68, -1, -1, -1, -1, 0xBB, 0x3C, 0x00, 0x00, 0x00, 0xA3, -1, -1, -1, -1, 0xE8 });
        auto *render_list_1 = find_signature({ 0x8B, 0x74, 0x24, -1, 0x0F, 0xBF, 0xCE, 0x46, 0x84, 0xC0, 0x8D, 0x8C, 0xCC, -1, -1, -1, -1, 0x89, 0x74, 0x24, -1, 0x66, 0x89, 0x39 });
        auto *render_list_23 = find_signature({ 0x8B, 0x4C, 0x24, -1, 0x0F, 0xBF, 0xC0, 0x51, 0x8D, 0x84, 0xC4, -1, -1, -1, -1, 0x8D, 0x8C, 0x24, -1, -1, -1, -1, 0x8B, 0xD0, 0x2B, 0xD1, 0xC1, 0xFA, 0x03 });
        auto *render_list_4 = find_signature({ 0x32, 0xDB, 0x8D, 0x84, 0x24, -1, -1, -1, -1, 0xFF, 0x4C, 0x24, -1, 0x66, 0x8B, 0x48, 0xFE, 0x66, 0x3B, 0xCD });
        auto *render_list_5 = find_signature({ 0x33, 0xFF, 0x66, 0x3B, 0xF7, 0x8D, 0x84, 0x24, -1, -1, -1, -1, 0x89, 0x44, 0x24, -1, 0x0F, 0x8E });
        auto *render_group_1 = find_signature({ 0x66, 0x81, 0xFE, -1, -1, 0x7D, -1, 0x66, 0x8B, 0x10, 0x8A, 0x58, 0x02, 0x0F, 0xBF, 0xFE, 0x8D, 0xBC, 0x7C, -1, -1, -1, -1, 0x46, 0x66, 0xC7, 0x07, 0x01, 0x00 });
        auto *render_group_2 = find_signature({ 0x0F, 0xB7, 0xD6, 0x8D, 0x8C, 0x24, -1, -1, -1, -1, 0x89, 0x4C, 0x24, -1, 0x89, 0x54, 0x24, -1 });
        auto *sprite_vertices = find_signature({ 0xC7, 0x44, 0x24, -1, 0x00, 0x20, 0x00, 0x00, 0x0F, 0xBF, 0x3C, 0x45, -1, -1, -1, -1, 0x0F, 0xAF, 0x7C, 0x24, -1 });
        auto *screen_sprite_vertices = find_signature({ 0xC7, 0x44, 0x24, -1, 0x02, 0x00, 0x00, 0x00, 0xEB, -1, 0xC7, 0x44, 0x24, -1, 0x00, 0x40, 0x00, 0x00, 0xEB, -1, 0xC7, 0x44, 0x24, -1, 0x00, 0x08, 0x00, 0x00 });
        if(!particle_count || !particle_systems_count || !effect_count || !effect_location_count || !render_list_1 || !render_list_23
        || !render_list_4 || !render_list_5 || !render_group_1 || !render_group_2 || !sprite_vertices || !screen_sprite_vertices) {
            return;
        }

        // The particle array is too big for the game state, so it gets our own memory
        overwrite(particle_count + 1, static_cast<std::uint32_t>(PARTICLE_LIMIT));
        retarget_call(particle_count + 15, reinterpret_cast<const void *>(extend_effect_limits_data_new_asm));

        const auto &limits = large_game_state ? LARGE_GAME_STATE_LIMITS : STOCK_GAME_STATE_LIMITS;
        overwrite(effect_count + 1, limits.effects);
        overwrite(effect_location_count + 1, limits.effect_locations);
        overwrite(particle_systems_count + 1, static_cast<std::uint8_t>(PARTICLE_SYSTEM_LIMIT));

        // Particle system particles and glow particles draw from the same sprite vertex pools as particles
        std::uint32_t sprites = PARTICLE_LIMIT;
        if(large_game_state && extend_game_state_arrays(limits)) {
            sprites += limits.particle_system_particles + limits.glow_particles;
        }

        // Move render_particles' particle list off the stack. The begin/end pair follows a push, hence the 0x504.
        write_lea_indexed(render_list_1 + 10, 0x0C, 0xCD, rendered_particles.get());   // lea ecx, [esp + ecx*8 + 0x500]
        write_lea_indexed(render_list_23 + 8, 0x04, 0xC5, rendered_particles.get());   // lea eax, [esp + eax*8 + 0x504]
        write_lea_absolute(render_list_23 + 15, 0x0D, rendered_particles.get());       // lea ecx, [esp + 0x504]
        write_lea_absolute(render_list_4 + 2, 0x05, rendered_particles.get() + 4);     // lea eax, [esp + 0x504]
        write_lea_absolute(render_list_5 + 5, 0x05, rendered_particles.get());         // lea eax, [esp + 0x500]

        // Same for the group counts. Groups can't outnumber particles, so the 512 group cap goes too.
        overwrite(render_group_1 + 3, static_cast<std::uint16_t>(PARTICLE_LIMIT));
        write_lea_indexed(render_group_1 + 16, 0x3C, 0x7D, group_particle_counts.get()); // lea edi, [esp + edi*2 + 0x100]
        write_lea_absolute(render_group_2 + 3, 0x0D, group_particle_counts.get());       // lea ecx, [esp + 0x100]

        // A sprite batch that doesn't fit its vertex pool is dropped for the frame. Other geometry shares these pools,
        // so keep the stock size on top.
        overwrite(sprite_vertices + 4, sprites * 4 + 0x2000);
        overwrite(screen_sprite_vertices + 14, sprites * 4 + 0x4000);
    }
}
