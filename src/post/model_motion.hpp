// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_POST_MODEL_MOTION_HPP
#define BUH_POST_MODEL_MOTION_HPP

#include <windows.h>
#include <d3d9.h>

namespace Buh {
    /**
     * Have the model parts Halo draws noted, from the next frame on, until the world's opaque surfaces are drawn.
     * Ask each frame it's wanted.
     */
    void want_model_motion() noexcept;

    /**
     * Draw how far the model parts drawn into target this frame moved since the last, in pixels, over the motion
     * buffer (the render target, with the world's depth bound for post_depth.hlsl and the world's size in c0). Only
     * parts drawn last frame too, whose bones moved, are drawn.
     * @param now this frame's world to clip, a column each, and last, the last frame's
     * @return false if it can't be drawn here
     */
    bool draw_model_motion(IDirect3DDevice9 *device, IDirect3DSurface9 *target, const float (&now)[4][4], const float (&last)[4][4]) noexcept;

    /**
     * Done with this frame's model parts: they're the last frame's for the next
     */
    void finish_model_motion() noexcept;

    void set_up_model_motion() noexcept;
}

#endif
