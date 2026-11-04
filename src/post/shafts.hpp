// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_POST_SHAFTS_HPP
#define BUH_POST_SHAFTS_HPP

namespace Buh {
    /**
     * Sun shafts over the world before the bloom and the HUD, and their buh_sun_shafts commands
     */
    void set_up_shafts() noexcept;

    /**
     * The sun, which the volumetric light shares: which way it is in the world (toward it) and its colour, the map's
     * or where buh_sun_shafts_direction puts it
     * @return false if there's none
     */
    bool current_sun(float direction[3], float color[3]) noexcept;
}

#endif
