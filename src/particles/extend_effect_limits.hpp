// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_PARTICLES_EXTEND_EFFECT_LIMITS_HPP
#define BUH_PARTICLES_EXTEND_EFFECT_LIMITS_HPP

#include <cstddef>
#include <cstdint>

#define PARTICLE_LIMIT 16384

namespace Buh {
    /**
     * Header of one of Halo's datum arrays
     */
    struct DatumArray {
        char name[0x20];
        std::uint16_t maximum_count;
        std::uint16_t element_size;
        std::uint8_t valid;
        std::uint8_t pad_1[3];
        std::uint32_t signature;
        std::uint16_t pad_2;
        std::uint16_t current_size;
        std::uint16_t count;
        std::uint16_t next_id;
        std::byte *elements;
    };
    static_assert(sizeof(DatumArray) == 0x38);
    static_assert(offsetof(DatumArray, current_size) == 0x2E);

    /**
     * Raise Halo's particle, effect and particle system limits, and with an enlarged game state its particle system
     * particle, decal and glow limits too
     * @param large_game_state whether set_up_extend_game_state() moved the game state
     */
    void set_up_extend_effect_limits(bool large_game_state) noexcept;
}

#endif
