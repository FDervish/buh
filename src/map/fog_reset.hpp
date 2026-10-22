// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_MAP_FOG_RESET_HPP
#define BUH_MAP_FOG_RESET_HPP

namespace Buh {
    /**
     * Clear the planar fog Halo renders with before each map loads, so the last map's fog plane can't show in the
     * next one
     */
    void set_up_fog_reset() noexcept;
}

#endif
