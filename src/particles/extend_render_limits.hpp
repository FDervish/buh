// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_PARTICLES_EXTEND_RENDER_LIMITS_HPP
#define BUH_PARTICLES_EXTEND_RENDER_LIMITS_HPP

namespace Buh {
    /**
     * Raise the renderer's per-frame vertex block, index block and sprite batch ceilings
     */
    void set_up_extend_render_limits() noexcept;
}

#endif
