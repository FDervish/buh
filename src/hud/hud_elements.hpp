// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_HUD_HUD_ELEMENTS_HPP
#define BUH_HUD_HUD_ELEMENTS_HPP

#include <cstddef>
#include <cstdint>

namespace Buh {
    /**
     * A HUD element Halo drew last frame: one of the elements its HUD tags define (a static element, a meter, a
     * number, or a weapon's overlay or crosshair), identified by where its definition is in the map's tag data
     */
    struct HudElement {
        std::uint32_t id;
        enum Kind { STATIC, METER, NUMBER, OVERLAY } kind;
        // Where Halo put it, before any changes made here, in the HUD's 640 by 480 (whatever the screen's shape)
        float x;
        float y;
        float width;
        float height;
    };

    /**
     * How an element is drawn instead: moved (in the HUD's 640 by 480), turned clockwise about its centre (degrees),
     * scaled about its centre, made more transparent, or hidden
     */
    struct HudElementChange {
        float dx = 0.0F;
        float dy = 0.0F;
        float rotation = 0.0F;
        float scale = 1.0F;
        float opacity = 1.0F;
        bool hidden = false;
    };

    /**
     * Whether HUD elements can be changed: the hooks they need were found
     */
    bool hud_elements_available() noexcept;

    /**
     * The elements Halo drew last frame, up to maximum of them
     * @return how many there are, which can be more than maximum
     */
    std::size_t hud_elements(HudElement *elements, std::size_t maximum) noexcept;

    /**
     * Draw an element Halo has drawn since the map loaded with this change, until it's reset or another map loads
     * @return false if there's no such element
     */
    bool set_hud_element(std::uint32_t id, const HudElementChange &change) noexcept;

    /**
     * Draw an element the way Halo does again, or all of them
     * @return false if there's no such element
     */
    bool reset_hud_element(std::uint32_t id) noexcept;
    void reset_hud_elements() noexcept;

    const char *hud_element_kind_name(HudElement::Kind kind) noexcept;

    /**
     * Find the HUD's element drawing, and add buh_hud_elements, buh_hud_element and buh_hud_element_reset
     */
    void set_up_hud_elements() noexcept;
}

#endif
