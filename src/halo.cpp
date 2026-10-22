// SPDX-License-Identifier: GPL-3.0-only

#include "halo.hpp"
#include "memory.hpp"

#include <cstring>

// The tag data header is always here on Custom Edition, and Chimera grows tag space to 64 MiB
#define TAG_DATA_ADDRESS 0x40440000
#define TAG_SPACE_SIZE 0x4000000

namespace Buh {
    static IDirect3DDevice9 **device = nullptr;
    static std::uint8_t **game_time_globals = nullptr;
    static const std::uint8_t *console = nullptr;
    static const std::int16_t *server_type = nullptr;

    struct TagDataHeader {
        Tag *tag_array;
        std::uint32_t scenario_tag;
        std::uint32_t checksum;
        std::uint32_t tag_count;
    };

    static TagDataHeader &tag_data_header() noexcept {
        return *reinterpret_cast<TagDataHeader *>(TAG_DATA_ADDRESS);
    }

    IDirect3DDevice9 *halo_device() noexcept {
        return device ? *device : nullptr;
    }

    bool singleplayer() noexcept {
        return !server_type || *server_type == 0;
    }

    bool game_paused() noexcept {
        auto *globals = game_time_globals ? *game_time_globals : nullptr;
        return globals && globals[2];
    }

    bool console_open() noexcept {
        return console && *console;
    }

    void set_game_paused(bool paused) noexcept {
        auto *globals = game_time_globals ? *game_time_globals : nullptr;
        if(!globals) {
            return;
        }
        if(globals[0]) {
            globals[1] = !paused;
        }
        globals[2] = paused;
    }

    bool game_time_stopped() noexcept {
        return singleplayer() && (game_paused() || console_open());
    }

    bool in_tag_space(const void *pointer) noexcept {
        auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return address >= TAG_DATA_ADDRESS && address < TAG_DATA_ADDRESS + TAG_SPACE_SIZE;
    }

    bool readable(const void *pointer, std::size_t size) noexcept {
        auto address = reinterpret_cast<std::uintptr_t>(pointer);
        if(address == 0) {
            return false;
        }
        if(in_tag_space(pointer) && in_tag_space(reinterpret_cast<const std::byte *>(pointer) + size - 1)) {
            return true;
        }
        // The last readable region found, since lookups tend to stay in one
        thread_local std::uintptr_t known_start = 0, known_end = 0;
        if(address >= known_start && address + size <= known_end) {
            return true;
        }
        MEMORY_BASIC_INFORMATION info;
        const DWORD readable_protection = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if(!VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(info.Protect & readable_protection)) {
            return false;
        }
        known_start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        known_end = known_start + info.RegionSize;
        return address + size <= known_end;
    }

    Tag *tag_array() noexcept {
        return tag_data_header().tag_array;
    }

    std::size_t tag_count() noexcept {
        return tag_array() ? tag_data_header().tag_count : 0;
    }

    Tag *get_tag(std::uint32_t id) noexcept {
        auto index = id & 0xFFFF;
        if(id == 0xFFFFFFFF || index >= tag_count()) {
            return nullptr;
        }
        auto *tag = tag_array() + index;
        return tag->id == id && readable(tag->data) ? tag : nullptr;
    }

    Tag *find_tag(const char *path, std::uint32_t tag_class) noexcept {
        for(std::size_t i = 0; i < tag_count(); i++) {
            auto *tag = tag_array() + i;
            // Protected maps can have paths and data that point anywhere
            if(tag->primary_class == tag_class && readable(tag->path) && _stricmp(tag->path, path) == 0 && readable(tag->data)) {
                return tag;
            }
        }
        return nullptr;
    }

    template<typename T> static T *read_address(const std::byte *at) noexcept {
        std::uint32_t address;
        std::memcpy(&address, at, sizeof(address));
        return reinterpret_cast<T *>(address);
    }

    void set_up_halo() noexcept {
        // Halo's own Bink player, which reads the device from a global
        auto *halo_player = find_signature({ 0x83, 0xEC, 0x68, 0xA1, -1, -1, -1, -1, 0x53, 0x33, 0xDB, 0x3B, 0xC3, 0x89, 0x5C, 0x24, 0x0C, 0x89, 0x5C, 0x24, 0x08, 0x88, 0x5C, 0x24, 0x05, 0xC6, 0x44, 0x24, 0x06, 0x01, 0x88, 0x5C, 0x24, 0x07, 0x0F, 0x85, -1, -1, -1, -1, 0xA1, -1, -1, -1, -1, 0x8B, 0x08, 0x53, 0x8D, 0x54, 0x24, 0x0C, 0x52, 0x53, 0x6A, 0x16 });
        auto *time_globals = find_signature({ 0x8B, 0x15, -1, -1, -1, -1, 0x8A, 0x42, 0x02, 0x84, 0xC0, 0x75, 0x22, 0x8B, 0x0D });
        auto *console_enabled = find_signature({ 0xA0, -1, -1, -1, -1, 0x84, 0xC0, 0x74, 0x28, 0xA0, -1, -1, -1, -1, 0x84, 0xC0, 0x75, 0x1F, 0x57 });
        auto *server = find_signature({ 0x0F, 0xBF, 0x2D, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x39, 0x1D, -1, -1, -1, -1, 0x75, 0x05 });
        if(halo_player) {
            device = read_address<IDirect3DDevice9 *>(halo_player + 0x29);
        }
        if(time_globals) {
            game_time_globals = read_address<std::uint8_t *>(time_globals + 2);
        }
        if(console_enabled) {
            // The byte before "console enabled" is "console open"
            console = read_address<const std::uint8_t>(console_enabled + 1) - 1;
        }
        if(server) {
            server_type = read_address<const std::int16_t>(server + 3);
        }
    }
}
