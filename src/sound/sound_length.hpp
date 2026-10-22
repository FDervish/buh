// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_SOUND_SOUND_LENGTH_HPP
#define BUH_SOUND_SOUND_LENGTH_HPP

#include <cstddef>

namespace Buh {
    /**
     * Seconds of audio in one permutation of a sound tag (not counting the permutations it's chained to), or 0 if
     * it can't be told
     */
    double sound_permutation_seconds(const std::byte *sound, const std::byte *permutation) noexcept;

    /**
     * The sound tag a permutation belongs to, or nullptr
     */
    const std::byte *sound_of_permutation(const std::byte *permutation) noexcept;

    /**
     * buh_sound_length: how long each permutation of a sound tag is
     */
    void set_up_sound_length() noexcept;
}

#endif
