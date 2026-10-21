// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_PARTICLES_PARTICLE_CULLING_HPP
#define BUH_PARTICLES_PARTICLE_CULLING_HPP

namespace Buh {
    /**
     * Stop Halo from deleting or skipping particles that should still be drawn
     */
    void set_up_particle_culling_fix() noexcept;
}

#endif
