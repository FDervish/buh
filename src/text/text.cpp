// SPDX-License-Identifier: GPL-3.0-only
//
// Derived from Chimera's text drawing.

#include "text.hpp"
#include "../halo.hpp"
#include "../memory.hpp"

#include <cstring>
#include <string>

#define GLOBALS_INTERFACE_BITMAPS_OFFSET 0x140
#define INTERFACE_BITMAPS_HUD_GLOBALS_OFFSET 0x60
#define HUD_GLOBALS_SINGLEPLAYER_FONT_OFFSET 0x48
#define HUD_GLOBALS_MULTIPLAYER_FONT_OFFSET 0x58
#define FONT_CHARACTERS_OFFSET 0x7C

namespace Buh {
    // Halo's text state, which its text renderer reads
    struct FontData {
        std::uint32_t font;
        std::uint32_t unknown;
        std::int16_t style;
        TextAlignment alignment;
        float color[4];
        std::int16_t tabs[0x12];
        std::uint32_t offset;
    };
    static_assert(sizeof(FontData) == 0x44);

    struct FontCharacter {
        std::uint16_t character;
        std::uint16_t width;
        std::byte rest[0x10];
    };
    static_assert(sizeof(FontCharacter) == 0x14);

    static FontData *font_data = nullptr;
    static const void *draw_text_16_bit = nullptr;

    extern "C" void buh_draw_text_asm(const wchar_t *text, const std::int16_t *bounds, const void *function) noexcept;

    bool text_available() noexcept {
        return font_data && draw_text_16_bit;
    }

    // A dependency is its class, path, path length and then tag ID
    static std::uint32_t dependency(const std::byte *at) noexcept {
        return *reinterpret_cast<const std::uint32_t *>(at + 0xC);
    }

    std::uint32_t hud_message_font() noexcept {
        auto *globals = find_tag("globals\\globals", TAG_CLASS_GLOBALS);
        if(!globals || block_count(globals->data + GLOBALS_INTERFACE_BITMAPS_OFFSET) == 0) {
            return 0xFFFFFFFF;
        }
        auto *interface_bitmaps = block_elements(globals->data + GLOBALS_INTERFACE_BITMAPS_OFFSET);
        auto *hud_globals = readable(interface_bitmaps, INTERFACE_BITMAPS_HUD_GLOBALS_OFFSET + 0x10) ? get_tag(dependency(interface_bitmaps + INTERFACE_BITMAPS_HUD_GLOBALS_OFFSET)) : nullptr;
        if(!hud_globals) {
            return 0xFFFFFFFF;
        }
        auto font = dependency(hud_globals->data + (singleplayer() ? HUD_GLOBALS_SINGLEPLAYER_FONT_OFFSET : HUD_GLOBALS_MULTIPLAYER_FONT_OFFSET));
        auto *tag = get_tag(font);
        return tag && tag->primary_class == TAG_CLASS_FONT ? font : 0xFFFFFFFF;
    }

    static Tag *font_tag(std::uint32_t font) noexcept {
        auto *tag = get_tag(font);
        return tag && tag->primary_class == TAG_CLASS_FONT ? tag : nullptr;
    }

    int font_line_height(std::uint32_t font) noexcept {
        auto *tag = font_tag(font);
        if(!tag) {
            return 0;
        }
        // Ascending and descending height
        return *reinterpret_cast<const std::int16_t *>(tag->data + 4) + *reinterpret_cast<const std::int16_t *>(tag->data + 6);
    }

    int text_width(std::wstring_view text, std::uint32_t font) noexcept {
        auto *tag = font_tag(font);
        if(!tag) {
            return 0;
        }
        auto count = block_count(tag->data + FONT_CHARACTERS_OFFSET);
        auto *characters = block_elements<FontCharacter>(tag->data + FONT_CHARACTERS_OFFSET);
        if(!readable(characters, count * sizeof(FontCharacter))) {
            return 0;
        }
        int width = 0;
        for(std::size_t i = 0; i < text.size(); i++) {
            // |x is a formatting code, not text
            if(text[i] == L'|' && i + 1 < text.size()) {
                i++;
                continue;
            }
            for(std::size_t c = 0; c < count; c++) {
                if(characters[c].character == text[i]) {
                    width += characters[c].width;
                    break;
                }
            }
        }
        return width;
    }

    void draw_text(const std::wstring &text, int left, int top, int right, int bottom, std::uint32_t font, TextAlignment alignment, const float color[4]) noexcept {
        if(!text_available() || !font_tag(font)) {
            return;
        }
        auto saved = *font_data;
        font_data->font = font;
        font_data->alignment = alignment;
        std::memcpy(font_data->color, color, sizeof(font_data->color));
        const std::int16_t bounds[4] = { static_cast<std::int16_t>(top), static_cast<std::int16_t>(left), static_cast<std::int16_t>(bottom), static_cast<std::int16_t>(right) };
        buh_draw_text_asm(text.c_str(), bounds, draw_text_16_bit);
        *font_data = saved;
    }

    void set_up_text() noexcept {
        auto *text_16_bit = find_signature({ 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0xA4, 0x00, 0x00, 0x00, 0x53, 0x8B, 0xD8, 0xA0, -1, -1, -1, -1, 0x84, 0xC0, 0x56, 0x57, 0x8B, 0xF9, 0x0F, 0x84, 0xD8, 0x01, 0x00, 0x00 });
        auto *font_data_write = find_signature({ 0xC7, 0x44, 0x24, 0x0C, 0xEB, 0xEA, 0xEA, 0x3E, 0x8B, 0x4C, 0x24, 0x0C, 0xA3, -1, -1, -1, -1, 0x8B, 0xC2 });
        if(!text_16_bit || !font_data_write) {
            return;
        }
        std::uint32_t address;
        std::memcpy(&address, font_data_write + 13, sizeof(address));
        font_data = reinterpret_cast<FontData *>(address);
        draw_text_16_bit = text_16_bit;
    }
}
