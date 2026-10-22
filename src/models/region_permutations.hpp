// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_MODELS_REGION_PERMUTATIONS_HPP
#define BUH_MODELS_REGION_PERMUTATIONS_HPP

namespace Buh {
    /**
     * Picking a model region's permutation copies every candidate into a 32 slot stack buffer without checking how
     * many there are, so a region with more than 32 overruns it. Give it room for 256, the most a permutation
     * index (a byte) can address.
     */
    void set_up_region_permutation_limit() noexcept;
}

#endif
