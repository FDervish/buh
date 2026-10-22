// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_MAP_MAP_LOAD_HPP
#define BUH_MAP_MAP_LOAD_HPP

#include <cstddef>
#include <cstdint>

namespace Buh {
    /**
     * Called before and after Halo loads a map, with its name
     */
    using MapLoadCallback = void (*)(const char *map);
    void add_before_map_load_callback(MapLoadCallback callback) noexcept;
    void add_after_map_load_callback(MapLoadCallback callback) noexcept;

    /**
     * The map being loaded, or last loaded
     */
    const char *loading_map_name() noexcept;

    /**
     * Whether a map name is the main menu's
     */
    bool is_ui_map(const char *map) noexcept;

    /**
     * What actually loads a map: Halo's function, given the map's name, returning whether it worked in the low byte.
     * A runner decides how it's called; by default it's just called.
     */
    using MapLoadFunction = std::uint32_t (*)(const char *map);
    using MapLoadRunner = std::uint32_t (*)(const char *map, MapLoadFunction load);
    void set_map_load_runner(MapLoadRunner runner) noexcept;

    /**
     * Map data reads from this file offset up are handed to a handler instead of read from the file, so data can be
     * given offsets past the end of any map
     */
    #define VIRTUAL_FILE_OFFSET 0x80000000
    using VirtualReadHandler = void (*)(std::byte *buffer, std::uint32_t offset, std::uint32_t size);
    void set_virtual_read_handler(VirtualReadHandler handler) noexcept;

    /**
     * Hook map loading. If reading a map's data fails because its buffer couldn't be allocated, say so and quit
     * rather than crash reading into nothing.
     */
    void set_up_map_load() noexcept;
}

#endif
