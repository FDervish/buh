// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_POST_VOLUMETRIC_LIGHT_HPP
#define BUH_POST_VOLUMETRIC_LIGHT_HPP

namespace Buh {
    /**
     * Volumetric light over the world before the motion blur, the bloom and the HUD, and its buh_volumetric_light
     * commands
     */
    void set_up_volumetric_light() noexcept;
}

#endif
