// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_SOUND_SUBTITLES_HPP
#define BUH_SOUND_SUBTITLES_HPP

namespace Buh {
    /**
     * Show the subtitles kept in a sound permutation's (otherwise unused) subtitle data while it plays, plus
     * buh_subtitles to turn that off and buh_subtitle to show one
     */
    void set_up_subtitles() noexcept;
}

#endif
