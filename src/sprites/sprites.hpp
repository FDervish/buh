// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_SPRITES_SPRITES_HPP
#define BUH_SPRITES_SPRITES_HPP

#include <string>

namespace Buh {
    /**
     * Take away a sprite or shape on the HUD
     */
    void remove_overlay(const std::string &name) noexcept;

    /**
     * buh_sprite and friends: draw images, still or animated from a sprite sheet, and shapes, with the HUD
     */
    void set_up_sprites() noexcept;
}

#endif
