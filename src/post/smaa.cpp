// SPDX-License-Identifier: GPL-3.0-only
//
// SMAA over the world, once Halo has drawn it and before the bloom and the HUD, so the HUD's text stays as sharp as it
// was. The shaders in smaa_shaders.hlsl find the world's edges, work out how much each pixel on one blends with which
// neighbour, then blend them back into Halo's render target.

#include "smaa.hpp"
#include "smaa_shaders.h"
#include "post.hpp"
#include "../console.hpp"
#include "../memory.hpp"
#include "../render.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

// SMAA's lookup textures: the area texture's two bytes a pixel are its luminance then its alpha, which the shader
// reads as .ra, and the search texture's one is its luminance
#define AREA_WIDTH 160
#define AREA_HEIGHT 560
#define SEARCH_WIDTH 64
#define SEARCH_HEIGHT 16

// Where the shaders read each texture
#define COLOR_SAMPLER 0
#define EDGES_SAMPLER 1
#define AREA_SAMPLER 2
#define SEARCH_SAMPLER 3
#define BLEND_SAMPLER 4

namespace Buh {
    static bool enabled = false;

    enum Pass { EDGES, WEIGHTS, BLEND, PASS_COUNT };
    static const DWORD *const pass_shaders[PASS_COUNT] = { smaa_edges_shader, smaa_weights_shader, smaa_blend_shader };

    static IDirect3DDevice9 *smaa_device = nullptr;
    static bool unavailable = false;
    static IDirect3DPixelShader9 *shaders[PASS_COUNT] = {};
    static IDirect3DTexture9 *area_texture = nullptr;
    static IDirect3DTexture9 *search_texture = nullptr;

    // The world's edges, and how much each pixel on one blends with its neighbours
    static PostTarget edges = {};
    static PostTarget weights = {};

    // Whether the world can be read and written as sRGB for the blend, which SMAA would have done in linear light,
    // and the format that was checked for
    static D3DFORMAT srgb_checked = D3DFMT_UNKNOWN;
    static bool srgb_blend = false;

    template<typename T> static void release(T *&object) noexcept {
        if(object) {
            object->Release();
            object = nullptr;
        }
    }

    // The targets, which a device reset mustn't find
    static void release_targets() noexcept {
        release_post_target(edges);
        release_post_target(weights);
    }

    static void release_device() noexcept {
        release_targets();
        for(auto *&shader : shaders) {
            release(shader);
        }
        release(area_texture);
        release(search_texture);
        srgb_checked = D3DFMT_UNKNOWN;
        smaa_device = nullptr;
    }

    // A lookup texture from buh's resources, its rows of pixels straight into a managed texture
    static IDirect3DTexture9 *load_lookup(IDirect3DDevice9 *device, const char *name, UINT width, UINT height, D3DFORMAT format, UINT pixel_size) noexcept {
        std::size_t size;
        auto *data = buh_resource(name, size);
        IDirect3DTexture9 *texture = nullptr;
        D3DLOCKED_RECT locked;
        if(!data || size != static_cast<std::size_t>(width) * height * pixel_size
        || device->CreateTexture(width, height, 1, 0, format, D3DPOOL_MANAGED, &texture, nullptr) < 0) {
            return nullptr;
        }
        if(texture->LockRect(0, &locked, nullptr, 0) < 0) {
            texture->Release();
            return nullptr;
        }
        for(UINT y = 0; y < height; y++) {
            std::memcpy(static_cast<std::byte *>(locked.pBits) + y * locked.Pitch, data + y * width * pixel_size, width * pixel_size);
        }
        texture->UnlockRect(0);
        return texture;
    }

    // The shaders and lookup textures, once per device
    static bool prepare(IDirect3DDevice9 *device) noexcept {
        if(smaa_device == device) {
            return true;
        }
        release_device();
        if(!create_post_shaders(device, pass_shaders, shaders, PASS_COUNT)
        || !(area_texture = load_lookup(device, "SMAA_AREA_TEXTURE", AREA_WIDTH, AREA_HEIGHT, D3DFMT_A8L8, 2))
        || !(search_texture = load_lookup(device, "SMAA_SEARCH_TEXTURE", SEARCH_WIDTH, SEARCH_HEIGHT, D3DFMT_L8, 1))) {
            release_device();
            unavailable = true;
            console_error("buh: SMAA can't be drawn here (it needs pixel shader 3.0)");
            return false;
        }
        smaa_device = device;
        return true;
    }

    // The targets for a world this size, made again when it changes. SMAA's are 8 bits a channel.
    static bool prepare_targets(IDirect3DDevice9 *device, UINT width, UINT height) noexcept {
        if(edges.texture && edges.width == width && edges.height == height) {
            return true;
        }
        release_targets();
        bool ready = create_post_target(device, edges, width, height, D3DFMT_A8R8G8B8)
            && create_post_target(device, weights, width, height, D3DFMT_A8R8G8B8);
        if(!ready) {
            release_targets();
            unavailable = true;
            console_error("buh: SMAA couldn't make its render targets, so it's off until buh_smaa true");
        }
        return ready;
    }

    // Whether the blend can be in linear light: the world read as sRGB from a texture, and written as sRGB
    static bool linear_blend(IDirect3DDevice9 *device, D3DFORMAT format) noexcept {
        if(srgb_checked != format) {
            srgb_checked = format;
            srgb_blend = post_format_supported(device, D3DUSAGE_QUERY_SRGBREAD, D3DRTYPE_TEXTURE, format)
                && post_format_supported(device, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_SURFACE, format);
        }
        return srgb_blend;
    }

    // A pass into a target, cleared first: the edge detection leaves what isn't an edge, and the weights only go
    // where there are edges
    static void run_pass(IDirect3DDevice9 *device, Pass pass, const PostTarget &into) noexcept {
        device->SetRenderTarget(0, into.surface);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0F, 0);
        device->SetPixelShader(shaders[pass]);
        draw_post_pass(device, into);
    }

    static void draw_smaa(IDirect3DDevice9 *device, double) noexcept {
        PostFrame frame;
        if(!enabled || unavailable || !prepare(device) || !begin_post_frame(device, frame)) {
            return;
        }
        auto width = frame.viewport.Width;
        auto height = frame.viewport.Height;
        if(!prepare_targets(device, width, height)) {
            end_post_frame(device, frame);
            return;
        }
        const float metrics[4] = { 1.0F / static_cast<float>(width), 1.0F / static_cast<float>(height), static_cast<float>(width), static_cast<float>(height) };
        device->SetPixelShaderConstantF(0, metrics, 1);
        device->SetTexture(COLOR_SAMPLER, frame.world->texture);
        run_pass(device, EDGES, edges);
        device->SetTexture(EDGES_SAMPLER, edges.texture);
        device->SetTexture(AREA_SAMPLER, area_texture);
        device->SetTexture(SEARCH_SAMPLER, search_texture);
        run_pass(device, WEIGHTS, weights);

        // Back into Halo's render target. Only the blend is in linear light; edges are found in the world as it is.
        bool linear = linear_blend(device, frame.format);
        device->SetSamplerState(COLOR_SAMPLER, D3DSAMP_SRGBTEXTURE, linear);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, linear);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        device->SetPixelShader(shaders[BLEND]);
        device->SetTexture(BLEND_SAMPLER, weights.texture);
        draw_post_world(device, frame);
        end_post_frame(device, frame);
    }

    static void smaa_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], enabled)) {
            console_error("usage: buh_smaa [true|false]");
            return;
        }
        console_print(enabled ? "buh_smaa: true" : "buh_smaa: false");
        if(enabled && !hud_drawing_available()) {
            console_error("buh: SMAA can't be drawn with this haloce.exe");
        }
        // Turned on again, it tries again
        if(enabled) {
            unavailable = false;
        }
    }

    void set_up_smaa() noexcept {
        add_world_draw_callback(draw_smaa);
        add_reset_callback(release_targets);
        add_console_command("buh_smaa", "[true|false]", 0, 1, smaa_command);
    }
}
