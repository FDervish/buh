// SPDX-License-Identifier: GPL-3.0-only

#include "render.hpp"
#include "halo.hpp"
#include "memory.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#define DEVICE_RESET_INDEX 16
#define DEVICE_PRESENT_INDEX 17
#define DEVICE_SET_DEPTH_STENCIL_SURFACE_INDEX 39

// Longer gaps between HUD draws than this (a hitch, a menu, the window being away) don't count as game time
#define MAXIMUM_HUD_GAP_SECONDS 0.25

// Halo's rasterizer window parameters, which rasterizer_window_begin() fills in for the window about to be drawn:
// what it's drawn into (the screen, or a mirror's texture), and its camera and frustum. On PC the player's view is
// target 1 and a mirror's 2, and interface_draw_screen() only draws the HUD for 1.
#define WINDOW_TARGET 0x000
#define WINDOW_TARGET_SCREEN 1
#define WINDOW_CAMERA_Z_NEAR 0x044
#define WINDOW_FRUSTUM_WORLD_TO_VIEW 0x06C
#define WINDOW_FRUSTUM_PROJECTION_VALID 0x19C
#define WINDOW_FRUSTUM_PROJECTION 0x1A0

// The window's fog, as rasterizer_window_begin() leaves it: with atmospheric fog off, its density is 0 and its
// distances are past the far clip. Halo's vertex shaders thicken it from its minimum distance to its maximum by depth
// along the view (vertex shader constant c6), times its maximum density clamped to 0 to 1 (c9.x).
#define WINDOW_FOG_ATMOSPHERIC_MAXIMUM_DENSITY 0x1F8
#define WINDOW_FOG_ATMOSPHERIC_MINIMUM_DISTANCE 0x1FC
#define WINDOW_FOG_ATMOSPHERIC_MAXIMUM_DISTANCE 0x200

namespace Buh {
    using PresentFunction = HRESULT (__stdcall *)(IDirect3DDevice9 *device, const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty);
    using ResetFunction = HRESULT (__stdcall *)(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *parameters);
    using SetDepthStencilSurfaceFunction = HRESULT (__stdcall *)(IDirect3DDevice9 *device, IDirect3DSurface9 *surface);
    static PresentFunction original_present = nullptr;
    static ResetFunction original_reset = nullptr;
    static SetDepthStencilSurfaceFunction original_set_depth_stencil_surface = nullptr;

    // The depth buffer Halo's own is swapped for, and its own
    static IDirect3DSurface9 *own_depth = nullptr;
    static IDirect3DSurface9 *substitute_depth = nullptr;

    static std::vector<HudDrawCallback> hud_draw_callbacks;
    static std::vector<WorldDrawCallback> world_draw_callbacks;
    static std::vector<OpaqueDrawCallback> opaque_draw_callbacks;
    static std::vector<PresentCallback> present_callbacks;
    static std::vector<ResetCallback> reset_callbacks;

    // 2 / width, which Halo scales the HUD's x by and Chimera's widescreen fix rewrites
    static const float *hud_x_scale = nullptr;

    static const std::byte *window_parameters = nullptr;
    static bool drawing_world = false;
    static double frame_seconds = 0.0;

    extern "C" {
        const void *buh_interface_draw = nullptr;
        const void *buh_transparent_geometry = nullptr;
        void buh_interface_draw_asm() noexcept;
        void buh_transparent_geometry_asm() noexcept;
    }

    void add_hud_draw_callback(HudDrawCallback callback) noexcept {
        hud_draw_callbacks.push_back(callback);
    }

    void add_world_draw_callback(WorldDrawCallback callback) noexcept {
        world_draw_callbacks.push_back(callback);
    }

    void add_opaque_draw_callback(OpaqueDrawCallback callback) noexcept {
        opaque_draw_callbacks.push_back(callback);
    }

    void set_depth_substitute(IDirect3DSurface9 *own, IDirect3DSurface9 *substitute) noexcept {
        own_depth = substitute ? own : nullptr;
        substitute_depth = substitute;
    }

    bool world_view(WorldView &view) noexcept {
        if(!drawing_world || !window_parameters || window_parameters[WINDOW_FRUSTUM_PROJECTION_VALID] == std::byte { 0 }) {
            return false;
        }
        view.world_to_view = reinterpret_cast<const float *>(window_parameters + WINDOW_FRUSTUM_WORLD_TO_VIEW);
        view.projection = reinterpret_cast<const float *>(window_parameters + WINDOW_FRUSTUM_PROJECTION);
        view.z_near = *reinterpret_cast<const float *>(window_parameters + WINDOW_CAMERA_Z_NEAR);
        view.fog_density = std::clamp(*reinterpret_cast<const float *>(window_parameters + WINDOW_FOG_ATMOSPHERIC_MAXIMUM_DENSITY), 0.0F, 1.0F);
        view.fog_start = *reinterpret_cast<const float *>(window_parameters + WINDOW_FOG_ATMOSPHERIC_MINIMUM_DISTANCE);
        view.fog_end = *reinterpret_cast<const float *>(window_parameters + WINDOW_FOG_ATMOSPHERIC_MAXIMUM_DISTANCE);
        // A perspective projection, which puts -z in w
        return view.projection[2 * 4 + 3] == -1.0F && view.z_near > 0.0F;
    }

    // Whether the window being drawn is the player's view on the screen, rather than what a mirror reflects
    static bool drawing_screen() noexcept {
        return !window_parameters || *reinterpret_cast<const std::int16_t *>(window_parameters + WINDOW_TARGET) == WINDOW_TARGET_SCREEN;
    }

    void add_present_callback(PresentCallback callback) noexcept {
        present_callbacks.push_back(callback);
    }

    void add_reset_callback(ResetCallback callback) noexcept {
        reset_callbacks.push_back(callback);
    }

    float hud_canvas_width() noexcept {
        return hud_x_scale && *hud_x_scale > 0.0F ? 2.0F / *hud_x_scale : 640.0F;
    }

    bool hud_drawing_available() noexcept {
        return buh_interface_draw != nullptr;
    }

    bool world_drawing_available() noexcept {
        return buh_interface_draw != nullptr && window_parameters != nullptr;
    }

    bool opaque_drawing_available() noexcept {
        return buh_transparent_geometry != nullptr && window_parameters != nullptr;
    }

    // The hooks are in the function table every device of Direct3D's (or a wrapper's) shares, so they're called for
    // other devices in the process too (an overlay's, say), which are left alone
    static HRESULT __stdcall present(IDirect3DDevice9 *device, const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty) {
        if(device == halo_device()) {
            for(auto *callback : present_callbacks) {
                callback(device);
            }
        }
        return original_present(device, source, destination, window, dirty);
    }

    static HRESULT __stdcall reset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *parameters) {
        if(device == halo_device()) {
            for(auto *callback : reset_callbacks) {
                callback();
            }
        }
        return original_reset(device, parameters);
    }

    static HRESULT __stdcall set_depth_stencil_surface(IDirect3DDevice9 *device, IDirect3DSurface9 *surface) {
        if(surface && surface == own_depth && device == halo_device()) {
            surface = substitute_depth;
        }
        return original_set_depth_stencil_surface(device, surface);
    }

    // Each time buh draws, as a D3DX font may have put the device's table back since (see hook_device_function)
    static void hook_device(IDirect3DDevice9 *device) noexcept {
        hook_device_function(device, DEVICE_PRESENT_INDEX, present, original_present);
        hook_device_function(device, DEVICE_RESET_INDEX, reset, original_reset);
        hook_device_function(device, DEVICE_SET_DEPTH_STENCIL_SURFACE_INDEX, set_depth_stencil_surface, original_set_depth_stencil_surface);
    }

    extern "C" void buh_before_interface_draw() noexcept {
        auto *device = halo_device();
        if(!device || !drawing_screen()) {
            return;
        }
        hook_device(device);

        static LARGE_INTEGER last = {};
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        double seconds = last.QuadPart ? static_cast<double>(now.QuadPart - last.QuadPart) / frequency.QuadPart : 0.0;
        last = now;
        if(seconds > MAXIMUM_HUD_GAP_SECONDS || game_time_stopped()) {
            seconds = 0.0;
        }
        frame_seconds = seconds;

        drawing_world = true;
        for(auto *callback : world_draw_callbacks) {
            callback(device, seconds);
        }
        drawing_world = false;
    }

    extern "C" void buh_before_transparent_geometry() noexcept {
        auto *device = halo_device();
        if(!device || !drawing_screen()) {
            return;
        }
        hook_device(device);
        drawing_world = true;
        for(auto *callback : opaque_draw_callbacks) {
            callback(device);
        }
        drawing_world = false;
    }

    extern "C" void buh_after_interface_draw() noexcept {
        auto *device = halo_device();
        if(!device || !drawing_screen()) {
            return;
        }
        hook_device(device);
        for(auto *callback : hud_draw_callbacks) {
            callback(device, frame_seconds);
        }
    }

    void set_up_render() noexcept {
        // The end of render_window: lens flares, then the HUD (interface_draw_screen), then the screen flash, then menus
        auto *hud_calls = find_signature({ 0xE8, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x8B, 0xC5, 0xE8, -1, -1, -1, -1, 0x66, 0x83, 0x3D, -1, -1, -1, -1, 0xFF, 0x75 });

        // The HUD quad's projection; Chimera rewrites the first constant, so it's left out of the pattern
        auto *hud_projection = find_signature({ 0xC7, 0x84, 0x24, 0xA8, 0x00, 0x00, 0x00, -1, -1, -1, -1, 0xC7, 0x84, 0x24, 0xAC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x84, 0x24, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x84, 0x24, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x84, 0x24, 0xBC, 0x00, 0x00, 0x00, 0x89, 0x88, 0x88, 0xBB });
        if(hud_projection) {
            hud_x_scale = reinterpret_cast<const float *>(hud_projection + 7);
        }

        // The window parameters, which this checks the render target of: cmp [target], bx; jnz; cmp [esi + 0x8C], bx
        auto *window_check = find_signature({ 0x66, 0x39, 0x1D, -1, -1, -1, -1, 0x75, 0x25, 0x66, 0x39, 0x9E, 0x8C, 0x00, 0x00, 0x00, 0x75, 0x1C });
        if(window_check) {
            std::memcpy(&window_parameters, window_check + 3, sizeof(window_parameters));
        }

        // render_window's call to start the transparent geometry, once the opaque world is drawn: after
        // mov [...], ebx; jz ... and before mov edx, [...]; mov eax, [...]; push ebx; push ebx; push <its submit function>
        auto *transparent_call = find_signature({ 0x89, 0x1D, -1, -1, -1, -1, 0x0F, 0x84, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x8B, 0x15, -1, -1, -1, -1, 0xA1, -1, -1, -1, -1, 0x53, 0x53, 0x68 });
        if(transparent_call) {
            transparent_call += 12;
            buh_transparent_geometry = branch_target(transparent_call);
            retarget_call(transparent_call, reinterpret_cast<const void *>(buh_transparent_geometry_asm));
        }

        if(!hud_calls) {
            return;
        }
        auto *hud_call = hud_calls + 5;
        std::int32_t relative;
        std::memcpy(&relative, hud_call + 1, sizeof(relative));
        buh_interface_draw = hud_call + 5 + relative;
        retarget_call(hud_call, reinterpret_cast<const void *>(buh_interface_draw_asm));
    }
}
