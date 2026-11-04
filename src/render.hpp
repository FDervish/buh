// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_RENDER_HPP
#define BUH_RENDER_HPP

#include <windows.h>
#include <d3d9.h>
#include <cstddef>

#include "memory.hpp"

namespace Buh {
    /**
     * Called right after Halo draws the HUD, so whatever it draws sorts with the HUD: menus and the console go on
     * top, and nothing is drawn while Halo isn't drawing the HUD.
     * @param seconds game time since the HUD was last drawn: 0 while it's stopped, or after a long gap
     */
    using HudDrawCallback = void (*)(IDirect3DDevice9 *device, double seconds);
    void add_hud_draw_callback(HudDrawCallback callback) noexcept;

    /**
     * Called right before Halo draws the HUD, once the world has been drawn: the depth buffer holds the world, and
     * world_view() gives the camera it was drawn with. The HUD then goes over whatever is drawn here.
     * @param seconds as for HudDrawCallback
     */
    using WorldDrawCallback = void (*)(IDirect3DDevice9 *device, double seconds);
    void add_world_draw_callback(WorldDrawCallback callback) noexcept;

    /**
     * Called once Halo has drawn the world's opaque surfaces, for the player's view, right before its transparent
     * ones: fog, water, glass, particles and the like. world_view() gives the camera, and the depth buffer holds the
     * opaque world.
     */
    using OpaqueDrawCallback = void (*)(IDirect3DDevice9 *device);
    void add_opaque_draw_callback(OpaqueDrawCallback callback) noexcept;

    /**
     * Whether there's an opaque draw: its hook, and Halo's rasterizer camera
     */
    bool opaque_drawing_available() noexcept;

    /**
     * Have Halo draw with another depth buffer wherever it would draw with its own: setting own as the depth buffer
     * sets substitute instead. nullptr stops it.
     */
    void set_depth_substitute(IDirect3DSurface9 *own, IDirect3DSurface9 *substitute) noexcept;

    /**
     * The camera the world being drawn was drawn with, straight out of Halo's rasterizer: its world-to-view matrix
     * (scale, then the forward, left and up the world's x, y and z go to, then the translation; view space looks
     * down -z) and its projection (a row vector times it, giving Direct3D's clip space). So depth worked out with
     * them is the depth already in the depth buffer.
     */
    struct WorldView {
        const float *world_to_view;
        const float *projection;
        float z_near;

        // Halo's atmospheric fog over it, already in the world's colours: how thick it gets (0 for none, up to 1), and
        // the depths it starts at and is thickest from. It thickens evenly between them, by depth along the view.
        float fog_density = 0.0F;
        float fog_start = 0.0F;
        float fog_end = 1.0F;
    };

    /**
     * The camera, during a WorldDrawCallback or OpaqueDrawCallback
     * @return false outside one, or if Halo's rasterizer camera couldn't be found
     */
    bool world_view(WorldView &view) noexcept;

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
     * Hook an entry of a device's table of functions, the hook going on to what was there, and hook it again whenever
     * it's been put back to that. Direct3D 9 puts the whole table back as BeginStateBlock is called, which D3DX's
     * fonts and sprites do (Chimera draws its text with them) as they're first drawn, and again after each reset, so
     * it's called each frame. Anything else's hook found in its place is left there.
     */
    template<typename Function> void hook_device_function(IDirect3DDevice9 *device, std::size_t index, Function hook, Function &original) noexcept {
        auto **table = *reinterpret_cast<void ***>(device);
        auto *entry = table[index];
        if(entry == reinterpret_cast<void *>(hook) || (original && entry != reinterpret_cast<void *>(original))) {
            return;
        }
        original = reinterpret_cast<Function>(entry);
        overwrite(table + index, reinterpret_cast<void *>(hook));
    }

    /**
     * Width of the HUD's 480 unit tall canvas: 640, or wider with Chimera's widescreen fix
     */
    float hud_canvas_width() noexcept;

    /**
     * Whether the HUD draw hook is in, which everything drawn with the HUD needs
     */
    bool hud_drawing_available() noexcept;

    /**
     * Whether there's a world draw too: the hook, and Halo's rasterizer camera
     */
    bool world_drawing_available() noexcept;

    void set_up_render() noexcept;
}

#endif
