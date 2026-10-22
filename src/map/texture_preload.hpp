// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_MAP_TEXTURE_PRELOAD_HPP
#define BUH_MAP_TEXTURE_PRELOAD_HPP

namespace Buh {
    /**
     * buh_preload_textures: load every texture of a map while it loads, rather than as each one first shows up
     */
    void set_up_texture_preload() noexcept;
}

#endif
