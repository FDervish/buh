// SPDX-License-Identifier: GPL-3.0-only
//
// Ambient occlusion, once Halo has drawn the world's opaque surfaces and before its transparent ones, so smoke, glass,
// water and effects go over the occluded world rather than being darkened along with it. The shaders in
// ao_shaders.hlsl work it out from the world's depth at half size, blur it and multiply it into the world. Halo's
// lightmaps already shade the level, so its radius is small: it's mostly what sits things on the ground and in
// corners, characters and vehicles above all. Halo's atmospheric fog is already in the opaque world's colours, so the
// occlusion fades with it.

#include "ao.hpp"
#include "ao_shaders.h"
#include "depth.hpp"
#include "post.hpp"
#include "../console.hpp"
#include "../render.hpp"

#include <algorithm>
#include <string>
#include <vector>

#define AO_SAMPLER 0
#define WORLD_SAMPLER 1

namespace Buh {
    static bool enabled = false;

    // The radius it looks for occluders in, in world units (a tenth of one is about a foot), and how strong it is
    static float radius = 0.25F;
    static float intensity = 1.0F;

    // How far, in degrees, something has to rise over a surface before it occludes it, so the shallow creases between
    // the flat triangles Halo's levels are made of don't show
    static float bias = 15.0F;
    #define RADIANS_PER_DEGREE 0.0174532925F

    enum Pass { OCCLUSION, BLUR, APPLY, PASS_COUNT };
    static const DWORD *const pass_shaders[PASS_COUNT] = { ao_shader, ao_blur_shader, ao_apply_shader };

    static IDirect3DDevice9 *ao_device = nullptr;
    static bool unavailable = false;
    static IDirect3DPixelShader9 *shaders[PASS_COUNT] = {};
    static D3DFORMAT target_format = D3DFMT_UNKNOWN;

    // The occlusion at half size, and the other half of its blur: visibility in red, distance in green
    static PostTarget occlusion = {};
    static PostTarget blurred = {};

    static void release_targets() noexcept {
        release_post_target(occlusion);
        release_post_target(blurred);
    }

    static void release_device() noexcept {
        release_targets();
        for(auto *&shader : shaders) {
            if(shader) {
                shader->Release();
                shader = nullptr;
            }
        }
        ao_device = nullptr;
    }

    // The shaders, once per device, and the targets' format: two half floats where the card can draw to them
    static bool prepare(IDirect3DDevice9 *device) noexcept {
        if(ao_device == device) {
            return true;
        }
        release_device();
        target_format = post_format_supported(device, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_G16R16F) ? D3DFMT_G16R16F : D3DFMT_A16B16G16R16F;
        if(!post_format_supported(device, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, target_format)
        || !create_post_shaders(device, pass_shaders, shaders, PASS_COUNT)) {
            unavailable = true;
            console_error("buh: ambient occlusion can't be drawn here (it needs pixel shader 3.0 and half float render targets)");
            return false;
        }
        ao_device = device;
        return true;
    }

    static bool prepare_targets(IDirect3DDevice9 *device, UINT width, UINT height) noexcept {
        UINT half_width = (width + 1) / 2;
        UINT half_height = (height + 1) / 2;
        if(occlusion.texture && occlusion.width == half_width && occlusion.height == half_height) {
            return true;
        }
        release_targets();
        bool ready = create_post_target(device, occlusion, half_width, half_height, target_format)
            && create_post_target(device, blurred, half_width, half_height, target_format);
        if(!ready) {
            release_targets();
            unavailable = true;
            console_error("buh: ambient occlusion couldn't make its render targets, so it's off until buh_ao true");
        }
        return ready;
    }

    // One way of the blur, from one target into the other
    static void blur(IDirect3DDevice9 *device, const PostTarget &from, const PostTarget &into, float x, float y) noexcept {
        const float step[4] = { x, y, 0.0F, 0.0F };
        device->SetPixelShaderConstantF(2, step, 1);
        device->SetTexture(AO_SAMPLER, from.texture);
        draw_post_pass(device, into);
    }

    static void draw_ao(IDirect3DDevice9 *device) noexcept {
        if(!enabled || unavailable) {
            return;
        }
        want_world_depth();
        PostFrame frame;
        if(!prepare(device) || !begin_post_frame(device, frame)) {
            return;
        }
        if(!prepare_targets(device, frame.viewport.Width, frame.viewport.Height) || !bind_world_depth(device, frame)) {
            end_post_frame(device, frame);
            return;
        }
        WorldView view;
        world_view(view);
        float texel_x = 1.0F / static_cast<float>(occlusion.width);
        float texel_y = 1.0F / static_cast<float>(occlusion.height);
        const float constants[2][4] = {
            { texel_x, texel_y, static_cast<float>(frame.viewport.Width), static_cast<float>(frame.viewport.Height) },
            { radius, intensity, bias * RADIANS_PER_DEGREE, 0.0F }
        };
        const float fog[4] = { view.fog_density, view.fog_start, 1.0F / std::max(view.fog_end - view.fog_start, 0.0001F), 0.0F };
        device->SetPixelShaderConstantF(0, constants[0], 2);
        device->SetPixelShaderConstantF(3, fog, 1);
        device->SetSamplerState(AO_SAMPLER, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(AO_SAMPLER, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

        device->SetPixelShader(shaders[OCCLUSION]);
        draw_post_pass(device, occlusion);
        device->SetPixelShader(shaders[BLUR]);
        blur(device, occlusion, blurred, texel_x, 0.0F);
        blur(device, blurred, occlusion, 0.0F, texel_y);

        device->SetPixelShader(shaders[APPLY]);
        device->SetTexture(AO_SAMPLER, occlusion.texture);
        device->SetTexture(WORLD_SAMPLER, frame.world->texture);
        draw_post_world(device, frame);
        end_post_frame(device, frame);
    }

    static void ao_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], enabled)) {
            console_error("usage: buh_ao [true|false]");
            return;
        }
        console_print(enabled ? "buh_ao: true" : "buh_ao: false");
        if(enabled && !opaque_drawing_available()) {
            console_error("buh: ambient occlusion can't be drawn with this haloce.exe");
        }
        // Turned on again, it tries again
        if(enabled) {
            unavailable = false;
            retry_world_depth();
        }
    }

    static void radius_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_ao_radius", "usage: buh_ao_radius [world units], 0.05 to 2", radius, 0.05F, 2.0F, arguments);
    }

    static void intensity_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_ao_intensity", "usage: buh_ao_intensity [amount], 0 to 4", intensity, 0.0F, 4.0F, arguments);
    }

    static void bias_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_ao_bias", "usage: buh_ao_bias [degrees], 0 to 60", bias, 0.0F, 60.0F, arguments);
    }

    void set_up_ao() noexcept {
        add_opaque_draw_callback(draw_ao);
        add_reset_callback(release_targets);
        add_console_command("buh_ao", "[true|false]", 0, 1, ao_command);
        add_console_command("buh_ao_radius", "[world units]", 0, 1, radius_command);
        add_console_command("buh_ao_intensity", "[amount]", 0, 1, intensity_command);
        add_console_command("buh_ao_bias", "[degrees]", 0, 1, bias_command);
    }
}
