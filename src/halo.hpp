// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_HALO_HPP
#define BUH_HALO_HPP

#include <windows.h>
#include <d3d9.h>
#include <cstddef>
#include <cstdint>

namespace Buh {
    /**
     * Halo's Direct3D device, or nullptr before it exists
     */
    IDirect3DDevice9 *halo_device() noexcept;

    bool singleplayer() noexcept;
    bool game_paused() noexcept;
    bool console_open() noexcept;

    /**
     * game_time_set_paused(), which is what the pause menu calls
     */
    void set_game_paused(bool paused) noexcept;

    /**
     * In singleplayer, the game's time stops while it's paused or the console is open
     */
    bool game_time_stopped() noexcept;

    /**
     * An object's data, or nullptr if there's no such object (any more)
     */
    const std::byte *object_datum(std::uint32_t object) noexcept;

    /**
     * Which of the scenario's structure BSPs is loaded, as Halo's structure_bsp_index script function says: -1 for none,
     * or -2 if buh couldn't find where Halo keeps it
     */
    int structure_bsp_index() noexcept;

    /**
     * Where an object is in the world: its origin, or its centre while it's attached to another object (in a seat, or
     * held), when its origin is relative to that object
     * @return false if there's no such object
     */
    bool object_position(std::uint32_t object, float (&position)[3]) noexcept;

    /**
     * The ID of the object at an address, or 0xFFFFFFFF if there's none there
     */
    std::uint32_t object_id_at(std::uintptr_t address) noexcept;

    // What Halo's collision tests hit: which way the surfaces they hit face, which surfaces they pass through, and
    // whether they hit the level, water and objects, of which types (0 bipeds, 1 vehicles, 2 weapons, 3 equipment,
    // 4 garbage, 5 projectiles, 6 scenery, 7 machines, 8 controls, 9 light fixtures, 10 placeholders, 11 sound scenery)
    #define COLLISION_FRONT_FACES 0x1
    #define COLLISION_BACK_FACES 0x2
    #define COLLISION_SKIP_TWO_SIDED 0x4
    #define COLLISION_SKIP_INVISIBLE 0x8
    #define COLLISION_LEVEL 0x20
    #define COLLISION_WATER 0x40
    #define COLLISION_OBJECTS 0x80
    #define COLLISION_OBJECT_TYPE(type) (COLLISION_OBJECTS | 0x100U << (type))
    #define COLLISION_ALL_OBJECTS (COLLISION_OBJECTS | 0xFFF00U)

    struct CollisionHit {
        enum What { LEVEL, WATER, OBJECT } what;
        float point[3];
        // Facing back along the ray
        float normal[3];
        // An index into Halo's materials, or -1
        std::int16_t material;
        // 0xFFFFFFFF unless what is OBJECT
        std::uint32_t object;
    };

    /**
     * Halo's collision_test_vector(), the ray trace its bullets and its AI's line of sight use: from origin to
     * origin + delta, hitting what flags say and passing through the object ignore (0xFFFFFFFF for none)
     * @return false if it hit nothing, or there's no level loaded to trace through
     */
    bool collision_test(std::uint32_t flags, const float (&origin)[3], const float (&delta)[3], std::uint32_t ignore, CollisionHit &hit) noexcept;

    #define TAG_CLASS_BITMAP 0x6269746D
    #define TAG_CLASS_DAMAGE_EFFECT 0x6A707421
    #define TAG_CLASS_FONT 0x666F6E74
    #define TAG_CLASS_GLOBALS 0x6D617467
    #define TAG_CLASS_HUD_GLOBALS 0x68756467
    #define TAG_CLASS_SOUND 0x736E6421

    struct Tag {
        std::uint32_t primary_class;
        std::uint32_t secondary_class;
        std::uint32_t tertiary_class;
        std::uint32_t id;
        const char *path;
        std::byte *data;
        std::uint32_t indexed;
        std::uint32_t padding;
    };
    static_assert(sizeof(Tag) == 0x20);

    /**
     * A tag in the loaded map, or nullptr if there's no such tag or its data isn't in tag space
     */
    Tag *get_tag(std::uint32_t id) noexcept;
    Tag *find_tag(const char *path, std::uint32_t tag_class) noexcept;

    /**
     * Every tag in the loaded map
     */
    Tag *tag_array() noexcept;
    std::size_t tag_count() noexcept;

    /**
     * The loaded map's scenario tag's data, or nullptr
     */
    std::byte *scenario_data() noexcept;

    /**
     * One of Halo's script functions' definitions (hs_function_definition: return type, flags, then pointers to its
     * name, parser, evaluator, help and usage), found by its name in Halo's image, or nullptr. Where two tables point
     * at the same name, the one whose help is help comes first.
     */
    const std::byte *script_function(const char *name, const char *help) noexcept;

    /**
     * Whether a pointer read out of tag data points into tag space
     */
    bool in_tag_space(const void *pointer) noexcept;

    /**
     * Whether a pointer read out of tag data can be read. Tags Chimera loads from bitmaps.map, sounds.map and
     * loc.map have their data in Chimera's memory, not tag space, and protected maps have garbage pointers.
     */
    bool readable(const void *pointer, std::size_t size = 1) noexcept;

    /**
     * Count and address of a tag block (reflexive) at a pointer into tag data
     */
    inline std::uint32_t block_count(const std::byte *at) noexcept {
        return *reinterpret_cast<const std::uint32_t *>(at);
    }
    template<typename T = std::byte> inline T *block_elements(const std::byte *at) noexcept {
        return *reinterpret_cast<T * const *>(at + 4);
    }

    void set_up_halo() noexcept;
}

#endif
