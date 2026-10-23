// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_AUDIO_AUDIO_HPP
#define BUH_AUDIO_AUDIO_HPP

namespace SoLoud {
    class Soloud;
}

namespace Buh {
    /**
     * buh_audio_play and friends: play wav, mp3, flac and ogg files with SoLoud, following the game's pause
     */
    void set_up_audio() noexcept;

    /**
     * buh's SoLoud, started if it hasn't been, or nullptr if it can't be. buh_audio_volume applies to all of it, and
     * the game pausing pauses all of it.
     */
    SoLoud::Soloud *audio_engine() noexcept;
}

#endif
