// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_POST_DEPTH_HPP
#define BUH_POST_DEPTH_HPP

#include "post.hpp"

namespace Buh {
    /**
     * Ask for the world's depth to be readable, each frame an effect that reads it is on. Halo then draws into a depth
     * buffer that can be read, from the next frame on, until nothing has asked for a while.
     */
    void want_world_depth() noexcept;

    /**
     * Bind the world's depth during a post frame, for post_depth.hlsl to read: the texture in sampler 7 and its
     * constants in c4 to c6, from the camera the world was drawn with
     * @return false if it can't be read this frame
     */
    bool bind_world_depth(IDirect3DDevice9 *device, const PostFrame &frame) noexcept;

    /**
     * Try again to read the world's depth if it couldn't be, when an effect that reads it is turned on
     */
    void retry_world_depth() noexcept;

    /**
     * The depth buffer swapping and its commands; the depth view, which goes over everything, comes last
     */
    void set_up_depth() noexcept;
    void set_up_depth_view() noexcept;
}

#endif
