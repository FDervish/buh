// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_LOADING_LOADING_SCREEN_HPP
#define BUH_LOADING_LOADING_SCREEN_HPP

namespace Buh {
    /**
     * buh_loading_screen: Balltze's loading screen, its animated background in place of Halo's, kept moving while
     * maps load on another thread
     */
    void set_up_loading_screen() noexcept;
}

#endif
