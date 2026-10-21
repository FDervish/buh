// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_GAME_STATE_HPP
#define BUH_GAME_STATE_HPP

// Stock is 0x400000 bytes plus 0x40000 after it, at 0x40000000 right below the tag data
#define GAME_STATE_STOCK_SIZE 0x440000
#define GAME_STATE_SIZE 0x640000

namespace Buh {
    /**
     * Move Halo's game state into a larger block so more of its arrays fit. Saved games and cores from the stock
     * layout no longer load.
     * @return whether the game state was moved
     */
    bool set_up_extend_game_state() noexcept;
}

#endif
