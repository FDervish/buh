// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_PARTICLES_PARTICLE_DEPTH_SORT_HPP
#define BUH_PARTICLES_PARTICLE_DEPTH_SORT_HPP

namespace Buh {
    /**
     * Sort particles back to front within each batch so overlapping sprites stop swapping order
     */
    void set_up_particle_depth_sort() noexcept;
}

#endif
