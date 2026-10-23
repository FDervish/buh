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
     * Where an object is in the world: its origin, or its centre while it's attached to another object (in a seat, or
     * held), when its origin is relative to that object
     * @return false if there's no such object
     */
    bool object_position(std::uint32_t object, float (&position)[3]) noexcept;

    #define TAG_CLASS_BITMAP 0x6269746D
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
