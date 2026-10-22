// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_LOADING_LOADING_SCREEN_HPP
#define BUH_LOADING_LOADING_SCREEN_HPP

namespace Buh {
    /**
     * buh_loading_screen: Halo's loading screen with an animated background in place of its own, kept moving while
     * maps load on another thread
     */
    void set_up_loading_screen() noexcept;
}

#endif
