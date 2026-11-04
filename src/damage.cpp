// SPDX-License-Identifier: GPL-3.0-only
//
// Everything Halo damages, it damages through object_cause_damage(damage, object, node, region, material, normal),
// which works out how much from the damage's multiplier first thing. Its calls are pointed here instead (the function
// itself is left alone, so Chimera's chimera_block_damage can still hook it): a callback sees each damage before it's
// done, and can scale it or stop it, and another sees what it did. damage_object() does damage of its own the same
// way, for scripts.

#include "damage.hpp"
#include "halo.hpp"
#include "memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

// Halo's damage data: its damage effect, its flags, the player and the object that caused it, and (past their team,
// and the damage's location, origin, epicenter and direction) its scale and multiplier
#define DAMAGE_DATA_TAG 0x00
#define DAMAGE_DATA_FLAGS 0x04
#define DAMAGE_DATA_PLAYER 0x08
#define DAMAGE_DATA_CAUSER 0x0C
#define DAMAGE_DATA_SCALE 0x40
#define DAMAGE_DATA_MULTIPLIER 0x44

// An object's player (a unit's, when a player controls it), its health and shield, as fractions of its maximums, and
// its damage flags, in which it's dead
#define OBJECT_PLAYER 0xC0
#define OBJECT_HEALTH 0xE0
#define OBJECT_SHIELD 0xE4
#define OBJECT_DAMAGE_FLAGS 0x106
#define OBJECT_DEAD 0x4

namespace Buh {
    using ObjectCauseDamage = void (*)(std::byte *damage, std::uint32_t object, std::int16_t node, std::int16_t region, std::int16_t material, const float *normal);
    static ObjectCauseDamage object_cause_damage = nullptr;

    static BeforeDamageCallback before_callback = nullptr;
    static AfterDamageCallback after_callback = nullptr;

    // The proc of the damage being done: damage_object()'s, while it's doing one, and 1 for Halo's own
    static float current_proc = 1.0F;

    bool damage_hook_available() noexcept {
        return object_cause_damage != nullptr;
    }

    void set_damage_callbacks(BeforeDamageCallback before, AfterDamageCallback after) noexcept {
        before_callback = before;
        after_callback = after;
    }

    template<typename T> static T read(const std::byte *at) noexcept {
        T value;
        std::memcpy(&value, at, sizeof(value));
        return value;
    }

    struct Vitality {
        float health;
        float shield;
        bool dead;
    };

    // An object's health, shield and whether it's dead, or false if it's gone
    static bool vitality(std::uint32_t object, Vitality &vitality) noexcept {
        auto *datum = object_datum(object);
        if(!datum) {
            return false;
        }
        vitality.health = read<float>(datum + OBJECT_HEALTH);
        vitality.shield = read<float>(datum + OBJECT_SHIELD);
        vitality.dead = read<std::uint16_t>(datum + OBJECT_DAMAGE_FLAGS) & OBJECT_DEAD;
        return true;
    }

    // Stands in for object_cause_damage() at each of its calls
    static void cause_damage(std::byte *data, std::uint32_t object, std::int16_t node, std::int16_t region, std::int16_t material, const float *normal) noexcept {
        if(!before_callback && !after_callback) {
            object_cause_damage(data, object, node, region, material, normal);
            return;
        }
        float multiplier = read<float>(data + DAMAGE_DATA_MULTIPLIER);
        Damage damage = {
            object,
            read<std::uint32_t>(data + DAMAGE_DATA_TAG),
            read<std::uint32_t>(data + DAMAGE_DATA_CAUSER),
            read<std::uint32_t>(data + DAMAGE_DATA_PLAYER),
            read<std::uint32_t>(data + DAMAGE_DATA_FLAGS),
            read<float>(data + DAMAGE_DATA_SCALE),
            multiplier,
            current_proc
        };
        if(before_callback && !before_callback(damage)) {
            return;
        }

        // The callbacks' multiplier is for this object only: one damage can go to several, one after another
        float changed = std::isfinite(damage.multiplier) ? std::max(damage.multiplier, 0.0F) : multiplier;
        damage.multiplier = changed;
        std::memcpy(data + DAMAGE_DATA_MULTIPLIER, &changed, sizeof(changed));
        Vitality before;
        bool existed = vitality(object, before);
        object_cause_damage(data, object, node, region, material, normal);
        // ...unless Halo changed it itself, as it does once it's passed a vehicle's damage on to who's in it
        if(read<float>(data + DAMAGE_DATA_MULTIPLIER) == changed) {
            std::memcpy(data + DAMAGE_DATA_MULTIPLIER, &multiplier, sizeof(multiplier));
        }

        if(after_callback && existed) {
            // Gone is destroyed
            Vitality after = { 0.0F, 0.0F, true };
            vitality(object, after);
            after_callback(damage, { before.health - after.health, before.shield - after.shield, !before.dead && after.dead });
        }
    }

    template<typename T> static void write(std::byte *at, T value) noexcept {
        std::memcpy(at, &value, sizeof(value));
    }

    bool damage_object(std::uint32_t object, std::uint32_t tag, std::uint32_t causer, std::uint32_t player, float multiplier, float scale,
        float proc, DamageDone &done) noexcept {
        Vitality before;
        if(!object_cause_damage || !vitality(object, before)) {
            return false;
        }
        if(player == 0xFFFFFFFF) {
            if(auto *by = object_datum(causer)) {
                player = read<std::uint32_t>(by + OBJECT_PLAYER);
            }
        }

        // As Chimera's apply_damage() fills it: the effect, its flags, who caused it, fields it leaves 0 and 0xFFFF, and
        // its scale and multiplier; the rest 0, with room to spare past the 0x54 Halo's own have
        std::byte data[0x60] = {};
        write<std::uint32_t>(data + DAMAGE_DATA_TAG, tag);
        write<std::uint32_t>(data + DAMAGE_DATA_PLAYER, player);
        write<std::uint32_t>(data + DAMAGE_DATA_CAUSER, causer);
        write<std::uint32_t>(data + 0x14, 0xFFFF);
        write<std::uint32_t>(data + 0x1C, 0xFFFF);
        write<std::uint32_t>(data + 0x20, 0xFFFF);
        write<float>(data + DAMAGE_DATA_SCALE, scale);
        write<float>(data + DAMAGE_DATA_MULTIPLIER, multiplier);

        // Through the stand-in, as Chimera calls it: no node, region or material, and no normal
        float proc_was = current_proc;
        current_proc = proc;
        cause_damage(data, object, -1, -1, -1, nullptr);
        current_proc = proc_was;

        Vitality after = { 0.0F, 0.0F, true };
        vitality(object, after);
        done = { before.health - after.health, before.shield - after.shield, !before.dead && after.dead };
        return true;
    }

    void set_up_damage() noexcept {
        // object_cause_damage(): sub esp, 0x94; mov eax, [esp + 0x9C] (the object); and eax, 0xFFFF
        auto *cause = find_signature({ 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00, 0x8B, 0x84, 0x24, 0x9C, 0x00, 0x00, 0x00, 0x25, 0xFF, 0xFF, 0x00, 0x00 });
        if(!cause) {
            return;
        }
        auto calls = find_calls(cause);
        if(calls.empty()) {
            return;
        }
        object_cause_damage = reinterpret_cast<ObjectCauseDamage>(cause);
        for(auto *call : calls) {
            retarget_call(call, reinterpret_cast<const void *>(cause_damage));
        }
    }
}
