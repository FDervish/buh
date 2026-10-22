// SPDX-License-Identifier: GPL-3.0-only

#include "map_load.hpp"
#include "../memory.hpp"

#include <windows.h>
#include <cstring>
#include <string>
#include <vector>

namespace Buh {
    static std::vector<MapLoadCallback> before_callbacks;
    static std::vector<MapLoadCallback> after_callbacks;
    static std::string loading_map;
    static VirtualReadHandler virtual_read_handler = nullptr;
    static MapLoadRunner map_load_runner = nullptr;

    extern "C" {
        const void *buh_load_map = nullptr;
        const void *buh_read_map_data = nullptr;
        void buh_load_map_asm() noexcept;
        void buh_read_map_data_asm() noexcept;
        std::uint32_t buh_call_load_map(const char *map) noexcept;
    }

    void add_before_map_load_callback(MapLoadCallback callback) noexcept {
        before_callbacks.push_back(callback);
    }

    void add_after_map_load_callback(MapLoadCallback callback) noexcept {
        after_callbacks.push_back(callback);
    }

    extern "C" void buh_before_map_load(const char *map) noexcept {
        loading_map = map ? map : "";
        for(auto *callback : before_callbacks) {
            callback(loading_map.c_str());
        }
    }

    extern "C" void buh_after_map_load() noexcept {
        for(auto *callback : after_callbacks) {
            callback(loading_map.c_str());
        }
    }

    const char *loading_map_name() noexcept {
        return loading_map.c_str();
    }

    bool is_ui_map(const char *map) noexcept {
        auto *name = std::strrchr(map, '\\');
        return _stricmp(name ? name + 1 : map, "ui") == 0;
    }

    void set_virtual_read_handler(VirtualReadHandler handler) noexcept {
        virtual_read_handler = handler;
    }

    void set_map_load_runner(MapLoadRunner runner) noexcept {
        map_load_runner = runner;
    }

    extern "C" std::uint32_t buh_run_map_load(const char *map) noexcept {
        return map_load_runner ? map_load_runner(map, buh_call_load_map) : buh_call_load_map(map);
    }

    // On Halo's file thread
    extern "C" void buh_read_virtual_data(std::byte *buffer, std::uint32_t offset, std::uint32_t size) noexcept {
        if(virtual_read_handler) {
            virtual_read_handler(buffer, offset, size);
        }
        else {
            std::memset(buffer, 0, size);
        }
    }

    extern "C" [[noreturn]] void buh_map_data_out_of_memory() noexcept {
        MessageBoxA(nullptr, "Halo ran out of memory loading the map.\n\nTry turning off maps in RAM or lowering its buffer size in chimera.ini.", "buh", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }

    // Retarget the calls that go where the first one goes, so they all share one original
    static void hook_calls(std::initializer_list<std::byte *> calls, const void *&original, const void *hook) noexcept {
        for(auto *call : calls) {
            if(!call || static_cast<std::uint8_t>(*call) != 0xE8) {
                continue;
            }
            auto *target = branch_target(call);
            if(!original) {
                original = target;
            }
            if(target == original) {
                retarget_call(call, hook);
            }
        }
    }

    void set_up_map_load() noexcept {
        // The two calls to the function that loads a map, with its name in eax
        auto *multiplayer_load = find_signature({ 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x74, 0x0A, 0x8B, 0x15, -1, -1, -1, -1, 0x88, 0x1A });
        auto *singleplayer_load = find_signature({ 0xE8, -1, -1, -1, -1, 0x33, 0xDB, 0x84, 0xC0, 0x74, 0x0B, 0x8B, 0x0D, -1, -1, -1, -1, 0xC6, 0x01, 0x01 });
        hook_calls({ multiplayer_load, singleplayer_load }, buh_load_map, reinterpret_cast<const void *>(buh_load_map_asm));

        // The two calls to the function that reads map data into a buffer, the buffer being its third argument
        auto *header_read = find_signature({ 0xE8, -1, -1, -1, -1, 0x83, 0xC4, 0x0C, 0x8D, 0x74, 0x24, 0x13, 0xE8, -1, -1, -1, -1, 0x8A, 0x44, 0x24, 0x13 });
        auto *data_read = find_signature({ 0xBF, -1, -1, -1, -1, 0xC6, 0x46, 0x1E, 0x01, 0xE8, -1, -1, -1, -1, 0x83, 0xC4, 0x0C, 0xE9, -1, -1, -1, -1 });
        hook_calls({ header_read, data_read ? data_read + 9 : nullptr }, buh_read_map_data, reinterpret_cast<const void *>(buh_read_map_data_asm));
    }
}
