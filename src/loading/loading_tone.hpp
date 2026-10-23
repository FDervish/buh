// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_LOADING_LOADING_TONE_HPP
#define BUH_LOADING_LOADING_TONE_HPP

namespace Buh {
    /**
     * Keep Xbox Halo's loading screen tone going, at this point through the loading screen: 0 just up, 1 gone
     */
    void update_loading_tone(double progress) noexcept;

    /**
     * Fade the tone out, if it's playing
     */
    void stop_loading_tone() noexcept;

    /**
     * buh_loading_screen_tone [volume]
     */
    void set_up_loading_tone() noexcept;
}

#endif
