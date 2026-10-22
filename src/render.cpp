// SPDX-License-Identifier: GPL-3.0-only

#include "render.hpp"
#include "halo.hpp"
#include "memory.hpp"

#include <cstring>
#include <vector>

#define DEVICE_RESET_INDEX 16
#define DEVICE_PRESENT_INDEX 17

// Longer gaps between HUD draws than this (a hitch, a menu, the window being away) don't count as game time
#define MAXIMUM_HUD_GAP_SECONDS 0.25

namespace Buh {
    using PresentFunction = HRESULT (__stdcall *)(IDirect3DDevice9 *device, const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty);
    using ResetFunction = HRESULT (__stdcall *)(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *parameters);
    static PresentFunction original_present = nullptr;
    static ResetFunction original_reset = nullptr;

    static std::vector<HudDrawCallback> hud_draw_callbacks;
    static std::vector<PresentCallback> present_callbacks;
    static std::vector<ResetCallback> reset_callbacks;

    // 2 / width, which Halo scales the HUD's x by and Chimera's widescreen fix rewrites
    static const float *hud_x_scale = nullptr;

    extern "C" {
        const void *buh_interface_draw = nullptr;
        void buh_interface_draw_asm() noexcept;
    }

    void add_hud_draw_callback(HudDrawCallback callback) noexcept {
        hud_draw_callbacks.push_back(callback);
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

    static HRESULT __stdcall present(IDirect3DDevice9 *device, const RECT *source, const RECT *destination, HWND window, const RGNDATA *dirty) {
        for(auto *callback : present_callbacks) {
            callback(device);
        }
        return original_present(device, source, destination, window, dirty);
    }

    static HRESULT __stdcall reset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *parameters) {
        for(auto *callback : reset_callbacks) {
            callback();
        }
        return original_reset(device, parameters);
    }

    static void hook_device(IDirect3DDevice9 *device) noexcept {
        if(original_present) {
            return;
        }
        auto **vtable = *reinterpret_cast<void ***>(device);
        original_present = reinterpret_cast<PresentFunction>(vtable[DEVICE_PRESENT_INDEX]);
        original_reset = reinterpret_cast<ResetFunction>(vtable[DEVICE_RESET_INDEX]);
        overwrite(vtable + DEVICE_PRESENT_INDEX, reinterpret_cast<void *>(present));
        overwrite(vtable + DEVICE_RESET_INDEX, reinterpret_cast<void *>(reset));
    }

    extern "C" void buh_after_interface_draw() noexcept {
        auto *device = halo_device();
        if(!device) {
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
        for(auto *callback : hud_draw_callbacks) {
            callback(device, seconds);
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
