// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_RENDER_HPP
#define BUH_RENDER_HPP

#include <windows.h>
#include <d3d9.h>

namespace Buh {
    /**
     * Called right after Halo draws the HUD, so whatever it draws sorts with the HUD: menus and the console go on
     * top, and nothing is drawn while Halo isn't drawing the HUD.
     * @param seconds game time since the HUD was last drawn: 0 while it's stopped, or after a long gap
     */
    using HudDrawCallback = void (*)(IDirect3DDevice9 *device, double seconds);
    void add_hud_draw_callback(HudDrawCallback callback) noexcept;

    /**
     * Called once a frame, before it's presented
     */
    using PresentCallback = void (*)(IDirect3DDevice9 *device);
    void add_present_callback(PresentCallback callback) noexcept;

    /**
     * Called before the device resets, which default pool resources mustn't survive
     */
    using ResetCallback = void (*)();
    void add_reset_callback(ResetCallback callback) noexcept;

    /**
     * Width of the HUD's 480 unit tall canvas: 640, or wider with Chimera's widescreen fix
     */
    float hud_canvas_width() noexcept;

    /**
     * Whether the HUD draw hook is in, which everything drawn with the HUD needs
     */
    bool hud_drawing_available() noexcept;

    void set_up_render() noexcept;
}

#endif
