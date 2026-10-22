// SPDX-License-Identifier: GPL-3.0-only
//
// Derived from Balltze's map textures preloading.

#include "texture_preload.hpp"
#include "map_load.hpp"
#include "../console.hpp"
#include "../halo.hpp"
#include "../memory.hpp"

#include <windows.h>
#include <string>

#define BITMAP_DATA_SIZE 0x30
#define BITMAP_BITMAP_DATA_OFFSET 0x60

namespace Buh {
    static bool preload = false;

    extern "C" {
        const void *buh_load_bitmap_function = nullptr;
        void buh_load_bitmap_texture(std::byte *bitmap_data) noexcept;
    }

    static void after_map_load(const char *map) noexcept {
        if(!preload || is_ui_map(map)) {
            return;
        }
        LARGE_INTEGER start, end, frequency;
        QueryPerformanceCounter(&start);
        std::size_t textures = 0;
        for(std::size_t i = 0; i < tag_count(); i++) {
            auto &tag = tag_array()[i];
            // Chimera has already loaded the ones the map keeps in bitmaps.map and cleared indexed, so they count too
            if(tag.primary_class != TAG_CLASS_BITMAP || tag.indexed || !readable(tag.data, BITMAP_BITMAP_DATA_OFFSET + 8)) {
                continue;
            }
            auto count = block_count(tag.data + BITMAP_BITMAP_DATA_OFFSET);
            auto *bitmap_data = block_elements(tag.data + BITMAP_BITMAP_DATA_OFFSET);
            if(count == 0 || !readable(bitmap_data, count * BITMAP_DATA_SIZE)) {
                continue;
            }
            for(std::size_t b = 0; b < count; b++, textures++) {
                buh_load_bitmap_texture(bitmap_data + b * BITMAP_DATA_SIZE);
            }
        }
        QueryPerformanceCounter(&end);
        QueryPerformanceFrequency(&frequency);
        auto milliseconds = (end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart;
        console_print(("buh: preloaded " + std::to_string(textures) + " textures in " + std::to_string(milliseconds) + " ms").c_str());
    }

    static void preload_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], preload)) {
            console_error("usage: buh_preload_textures [true|false]");
            return;
        }
        console_print(preload ? "buh_preload_textures: true" : "buh_preload_textures: false");
    }

    void set_up_texture_preload() noexcept {
        // Halo's texture loader: eax is the bitmap data, then whether to wait for it and whether to request it
        buh_load_bitmap_function = find_signature({ 0x83, 0xEC, 0x08, 0x53, 0x55, 0x56, 0x8B, 0xF0, 0x8A, 0x46, 0x0E, 0x33, 0xDB });
        if(!buh_load_bitmap_function) {
            return;
        }
        add_after_map_load_callback(after_map_load);
        add_console_command("buh_preload_textures", "[true|false]", 0, 1, preload_command);
    }
}
