// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_TEXT_TEXT_HPP
#define BUH_TEXT_TEXT_HPP

#include <cstdint>
#include <string_view>

namespace Buh {
    enum TextAlignment : std::int16_t {
        TEXT_ALIGN_LEFT = 0,
        TEXT_ALIGN_RIGHT,
        TEXT_ALIGN_CENTER
    };

    /**
     * The font Halo draws HUD messages with, or 0xFFFFFFFF if the map has none
     */
    std::uint32_t hud_message_font() noexcept;

    /**
     * Line height and text width of a bitmap font, in the HUD's 480 unit tall canvas
     */
    int font_line_height(std::uint32_t font) noexcept;
    int text_width(std::wstring_view text, std::uint32_t font) noexcept;

    /**
     * Draw text with Halo's own text renderer, into a rectangle of the HUD's canvas. "|n" starts a new line. Only
     * works while Halo is drawing the HUD.
     * @param color alpha, red, green and blue, from 0 to 1
     */
    void draw_text(const std::wstring &text, int left, int top, int right, int bottom, std::uint32_t font, TextAlignment alignment, const float color[4]) noexcept;

    bool text_available() noexcept;
    void set_up_text() noexcept;
}

#endif
