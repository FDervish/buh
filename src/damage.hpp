// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_DAMAGE_HPP
#define BUH_DAMAGE_HPP

#include <cstdint>

// A damage's flags (not the damaged object's): an explosion's, and one that kills outright
#define DAMAGE_AREA_OF_EFFECT 0x1
#define DAMAGE_KILL_INSTANTLY 0x4

namespace Buh {
    /**
     * Halo damaging an object: with what, caused by whom, and its multiplier, which is what a callback can change
     */
    struct Damage {
        std::uint32_t object;
        // The damage effect tag
        std::uint32_t tag;
        // The object and the player (a player table handle) that caused it, or 0xFFFFFFFF
        std::uint32_t causer;
        std::uint32_t player;
        std::uint32_t flags;
        // 0 to 1: how far from its damage effect's least damage to its most (a charge, or how near an explosion)
        float scale;
        float multiplier;
        // How much it can set off what happens on a hit, as Risk of Rain 2's proc coefficient: 1 for Halo's own damage,
        // and what damage_object() was given for its own (and for the damage Halo passes on from it)
        float proc;
    };

    /**
     * What a damage did to its object: how much health and shield it lost, as fractions of its maximums, and whether
     * it died of it (destroyed, if it's not a unit)
     */
    struct DamageDone {
        float health;
        float shield;
        bool killed;
    };

    /**
     * Before Halo damages an object, with what it's about to do: change damage.multiplier, or return false to stop it
     */
    using BeforeDamageCallback = bool (*)(Damage &damage) noexcept;

    /**
     * After, with what it did
     */
    using AfterDamageCallback = void (*)(const Damage &damage, const DamageDone &done) noexcept;

    /**
     * Whether Halo's damage goes through buh
     */
    bool damage_hook_available() noexcept;

    void set_damage_callbacks(BeforeDamageCallback before, AfterDamageCallback after) noexcept;

    /**
     * Damage an object with a damage effect as Halo's own damage is done: through object_cause_damage(), past the
     * callbacks, with its damage data filled in as Chimera's chimera_apply_damage fills it. Halo credits the player
     * it's given, or else the causer's (a unit a player controls), with it and its kill.
     * @param player a player table handle, or 0xFFFFFFFF for the causer's
     * @return false, with nothing done, if there's no such object or no object_cause_damage()
     */
    bool damage_object(std::uint32_t object, std::uint32_t tag, std::uint32_t causer, std::uint32_t player, float multiplier, float scale,
        float proc, DamageDone &done) noexcept;

    /**
     * Point every call to Halo's object_cause_damage() at buh
     */
    void set_up_damage() noexcept;
}

#endif
