// SPDX-License-Identifier: GPL-3.0-only
//
// Bloom over the world, once Halo has drawn it and before the HUD, so the HUD, menus and console stay sharp. The
// shaders in bloom_shaders.hlsl do the work, and this runs them in order: what of the world blooms is found pixel by
// pixel, taken to half size and halved down to a few texels, then built back up and screen blended over the world.
// The world itself is never blurred or resampled; the bloom only adds to it.

#include "bloom.hpp"
#include "bloom_shaders.h"
#include "post.hpp"
#include "../console.hpp"
#include "../render.hpp"

#include <algorithm>
#include <string>
#include <vector>

// The most levels, from half size down, and the smallest a level's shorter side gets past the first
#define MAXIMUM_LEVELS 7
#define SMALLEST_LEVEL 8

// The most a pixel is pushed to (at white) before the threshold, and the knee either side of the threshold as a
// fraction of it
#define MOST_PUSHED 16.0F
#define KNEE 0.5F

namespace Buh {
    static bool enabled = false;

    // How bright (its brightest channel, 0 to 1) a pixel is before it blooms, how much it blooms past that, how much
    // all of the world glows, and how far it spreads
    static float threshold = 0.8F;
    static float intensity = 0.25F;
    static float glow = 0.05F;
    static float spread = 0.7F;

    enum Pass { BRIGHT, DOWNSAMPLE, UPSAMPLE, COMPOSITE, PASS_COUNT };
    static const DWORD *const pass_shaders[PASS_COUNT] = {
        bloom_bright_shader, bloom_downsample_shader, bloom_upsample_shader, bloom_composite_shader
    };

    static IDirect3DDevice9 *bloom_device = nullptr;
    static bool unavailable = false;
    static IDirect3DPixelShader9 *shaders[PASS_COUNT] = {};

    // What of the world blooms, each level down from half size, and each but the lowest built back up
    static PostTarget blooming = {};
    static PostTarget down[MAXIMUM_LEVELS] = {};
    static PostTarget up[MAXIMUM_LEVELS - 1] = {};
    static UINT levels = 0;

    // The targets, which a device reset mustn't find
    static void release_targets() noexcept {
        release_post_target(blooming);
        for(auto &target : down) {
            release_post_target(target);
        }
        for(auto &target : up) {
            release_post_target(target);
        }
        levels = 0;
    }

    static void release_device() noexcept {
        release_targets();
        for(auto *&shader : shaders) {
            if(shader) {
                shader->Release();
                shader = nullptr;
            }
        }
        bloom_device = nullptr;
    }

    // The shaders, once per device. The levels are half floats, so they neither clip nor band, which the card has to
    // draw to and filter.
    static bool prepare(IDirect3DDevice9 *device) noexcept {
        if(bloom_device == device) {
            return true;
        }
        release_device();
        if(!post_format_supported(device, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F)
        || !create_post_shaders(device, pass_shaders, shaders, PASS_COUNT)) {
            unavailable = true;
            console_error("buh: bloom can't be drawn here (it needs pixel shader 3.0 and half float render targets)");
            return false;
        }
        bloom_device = device;
        return true;
    }

    // The targets for a world this size, made again when it changes
    static bool prepare_targets(IDirect3DDevice9 *device, UINT width, UINT height) noexcept {
        if(blooming.texture && blooming.width == width && blooming.height == height) {
            return true;
        }
        release_targets();
        bool ready = create_post_target(device, blooming, width, height, D3DFMT_A16B16G16R16F);
        UINT level_width = width;
        UINT level_height = height;
        while(ready && levels < MAXIMUM_LEVELS) {
            level_width = (level_width + 1) / 2;
            level_height = (level_height + 1) / 2;
            if(levels > 0 && std::min(level_width, level_height) < SMALLEST_LEVEL) {
                break;
            }
            ready = create_post_target(device, down[levels++], level_width, level_height, D3DFMT_A16B16G16R16F);
        }
        for(UINT i = 0; ready && i + 1 < levels; i++) {
            ready = create_post_target(device, up[i], down[i].width, down[i].height, D3DFMT_A16B16G16R16F);
        }
        if(!ready) {
            release_targets();
            unavailable = true;
            console_error("buh: bloom couldn't make its render targets, so it's off until buh_bloom true");
        }
        return ready;
    }

    // As the shader pushes a pixel whose brightest channel is this
    static float push(float brightness) noexcept {
        return brightness / (1.0F - brightness + brightness / MOST_PUSHED);
    }

    // The shader's texel constant: one texel of what it samples with its filter
    static void set_texel(IDirect3DDevice9 *device, const PostTarget &sampled) noexcept {
        const float texel[4] = { 1.0F / static_cast<float>(sampled.width), 1.0F / static_cast<float>(sampled.height), 0.0F, 0.0F };
        device->SetPixelShaderConstantF(0, texel, 1);
    }

    // A pass drawn all over a target, from source (and the level below, upsampling)
    static void run_pass(IDirect3DDevice9 *device, Pass pass, const PostTarget &into, const PostTarget &source, const PostTarget *lower = nullptr) noexcept {
        device->SetPixelShader(shaders[pass]);
        set_texel(device, lower ? *lower : source);
        device->SetTexture(0, source.texture);
        device->SetTexture(1, lower ? lower->texture : nullptr);
        draw_post_pass(device, into);
    }

    static void draw_bloom(IDirect3DDevice9 *device, double) noexcept {
        PostFrame frame;
        if(!enabled || unavailable || !prepare(device) || !begin_post_frame(device, frame)) {
            return;
        }
        if(!prepare_targets(device, frame.viewport.Width, frame.viewport.Height)) {
            end_post_frame(device, frame);
            return;
        }
        float pushed = push(threshold);
        const float bright[4] = { pushed, pushed * KNEE, MOST_PUSHED, intensity };
        const float settings[2][4] = { { glow, 0.0F, 0.0F, 0.0F }, { spread, 0.0F, 0.0F, 0.0F } };
        device->SetPixelShaderConstantF(1, bright, 1);
        device->SetPixelShaderConstantF(2, settings[0], 2);

        run_pass(device, BRIGHT, blooming, *frame.world);
        run_pass(device, DOWNSAMPLE, down[0], blooming);
        for(UINT i = 1; i < levels; i++) {
            run_pass(device, DOWNSAMPLE, down[i], down[i - 1]);
        }
        for(UINT i = levels - 1; i-- > 0;) {
            run_pass(device, UPSAMPLE, up[i], down[i], i + 2 < levels ? &up[i + 1] : &down[i + 1]);
        }
        const auto &bloom = levels > 1 ? up[0] : down[0];

        // Screen blended: the bloom times what the world is short of white, so it never clips
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        device->SetPixelShader(shaders[COMPOSITE]);
        set_texel(device, bloom);
        device->SetTexture(0, bloom.texture);
        device->SetTexture(1, nullptr);
        draw_post_world(device, frame);
        end_post_frame(device, frame);
    }

    static void bloom_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], enabled)) {
            console_error("usage: buh_bloom [true|false]");
            return;
        }
        console_print(enabled ? "buh_bloom: true" : "buh_bloom: false");
        if(enabled && !hud_drawing_available()) {
            console_error("buh: bloom can't be drawn with this haloce.exe");
        }
        // Turned on again, it tries again
        if(enabled) {
            unavailable = false;
        }
    }

    static void threshold_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_bloom_threshold", "usage: buh_bloom_threshold [brightness], 0 to 1", threshold, 0.0F, 1.0F, arguments);
    }

    static void intensity_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_bloom_intensity", "usage: buh_bloom_intensity [amount], 0 to 4", intensity, 0.0F, 4.0F, arguments);
    }

    static void glow_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_bloom_glow", "usage: buh_bloom_glow [amount], 0 to 1", glow, 0.0F, 1.0F, arguments);
    }

    static void spread_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_bloom_spread", "usage: buh_bloom_spread [amount], 0 to 1", spread, 0.0F, 1.0F, arguments);
    }

    void set_up_bloom() noexcept {
        add_world_draw_callback(draw_bloom);
        add_reset_callback(release_targets);
        add_console_command("buh_bloom", "[true|false]", 0, 1, bloom_command);
        add_console_command("buh_bloom_threshold", "[brightness]", 0, 1, threshold_command);
        add_console_command("buh_bloom_intensity", "[amount]", 0, 1, intensity_command);
        add_console_command("buh_bloom_glow", "[amount]", 0, 1, glow_command);
        add_console_command("buh_bloom_spread", "[amount]", 0, 1, spread_command);
    }
}
