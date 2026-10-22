// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_PACKS_TAG_PACKS_HPP
#define BUH_PACKS_TAG_PACKS_HPP

namespace Buh {
    /**
     * buh_tag_pack and friends: add tag packs (made with tools/tagpack.py) to every map that loads, so their tags can
     * be spawned and used without rebuilding the map
     */
    void set_up_tag_packs() noexcept;
}

#endif
