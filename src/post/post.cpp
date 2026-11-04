// SPDX-License-Identifier: GPL-3.0-only
//
// What buh's post processes (SMAA, bloom and colour grading) share: the world copied out of Halo's render target,
// the states they draw with, Halo's own kept in a state block to put back, and quads over a whole target.

#include "post.hpp"
#include "../console.hpp"
#include "../render.hpp"

// The smallest viewport worth post processing
#define SMALLEST_VIEWPORT 16

// The most samplers a post process reads from
#define POST_SAMPLERS 8

// How many times in a row something keeps a post process from drawing before it's said: a second or so of frames
#define TROUBLE_TIMES 60

namespace Buh {
    struct QuadVertex {
        float x;
        float y;
        float z;
        float rhw;
        float u;
        float v;
    };
    #define QUAD_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

    static IDirect3DDevice9 *post_device = nullptr;
    static IDirect3DStateBlock9 *state_block = nullptr;

    // The world as it is so far, copied out of the viewport, in the format it's drawn in
    static PostTarget world = {};
    static D3DFORMAT world_format = D3DFMT_UNKNOWN;

    template<typename T> static void release(T *&object) noexcept {
        if(object) {
            object->Release();
            object = nullptr;
        }
    }

    bool create_post_target(IDirect3DDevice9 *device, PostTarget &target, UINT width, UINT height, D3DFORMAT format) noexcept {
        target.width = width;
        target.height = height;
        if(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &target.texture, nullptr) >= 0
        && target.texture->GetSurfaceLevel(0, &target.surface) >= 0) {
            return true;
        }
        release_post_target(target);
        return false;
    }

    void release_post_target(PostTarget &target) noexcept {
        release(target.surface);
        release(target.texture);
        target.width = 0;
        target.height = 0;
    }

    // The world's copy and the state block, which a device reset mustn't find
    static void release_frame() noexcept {
        release_post_target(world);
        release(state_block);
    }

    static bool prepare_world(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format) noexcept {
        if(world.texture && world.width == width && world.height == height && world_format == format) {
            return true;
        }
        release_post_target(world);
        world_format = format;
        return create_post_target(device, world, width, height, format);
    }

    static void set_states(IDirect3DDevice9 *device) noexcept {
        device->SetVertexShader(nullptr);
        device->SetFVF(QUAD_FVF);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetRenderState(D3DRS_WRAP0, 0);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        for(DWORD sampler = 0; sampler < POST_SAMPLERS; sampler++) {
            device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(sampler, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
            device->SetSamplerState(sampler, D3DSAMP_SRGBTEXTURE, FALSE);
            device->SetTexture(sampler, nullptr);
        }
    }

    bool begin_post_frame(IDirect3DDevice9 *device, PostFrame &frame) noexcept {
        if(post_device != device) {
            release_frame();
            post_device = device;
        }
        frame = {};
        if(device->GetRenderTarget(0, &frame.target) < 0) {
            frame.target = nullptr;
            return false;
        }
        D3DSURFACE_DESC description;
        bool ready = device->GetViewport(&frame.viewport) >= 0
            && frame.viewport.Width >= SMALLEST_VIEWPORT && frame.viewport.Height >= SMALLEST_VIEWPORT
            && frame.target->GetDesc(&description) >= 0
            && prepare_world(device, frame.viewport.Width, frame.viewport.Height, description.Format);
        frame.format = description.Format;
        if(ready && !state_block && device->CreateStateBlock(D3DSBT_ALL, &state_block) < 0) {
            state_block = nullptr;
            ready = false;
        }
        if(ready) {
            RECT rect = {
                static_cast<LONG>(frame.viewport.X), static_cast<LONG>(frame.viewport.Y),
                static_cast<LONG>(frame.viewport.X + frame.viewport.Width), static_cast<LONG>(frame.viewport.Y + frame.viewport.Height)
            };
            ready = device->StretchRect(frame.target, &rect, world.surface, nullptr, D3DTEXF_NONE) >= 0;
        }
        if(!ready) {
            release(frame.target);
            return false;
        }
        frame.world = &world;

        // Halo keeps track of the states it set, so they all go back. The render target and depth buffer aren't
        // states, so they go back by hand. Post processes' targets can be smaller than the depth buffer, or not match
        // it multisampled, so there's none while they draw.
        state_block->Capture();
        if(device->GetDepthStencilSurface(&frame.depth) < 0) {
            frame.depth = nullptr;
        }
        device->SetDepthStencilSurface(nullptr);
        set_states(device);
        return true;
    }

    void end_post_frame(IDirect3DDevice9 *device, PostFrame &frame) noexcept {
        device->SetRenderTarget(0, frame.target);
        device->SetDepthStencilSurface(frame.depth);
        state_block->Apply();
        device->SetViewport(&frame.viewport);
        release(frame.depth);
        release(frame.target);
    }

    // A quad over a rectangle of the render target, its texels lined up with the pixels
    static void draw_quad(IDirect3DDevice9 *device, float x, float y, float width, float height) noexcept {
        float left = x - 0.5F;
        float top = y - 0.5F;
        const QuadVertex vertices[4] = {
            { left, top, 0.0F, 1.0F, 0.0F, 0.0F },
            { left + width, top, 0.0F, 1.0F, 1.0F, 0.0F },
            { left + width, top + height, 0.0F, 1.0F, 1.0F, 1.0F },
            { left, top + height, 0.0F, 1.0F, 0.0F, 1.0F }
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, vertices, sizeof(QuadVertex));
    }

    void draw_post_pass(IDirect3DDevice9 *device, const PostTarget &into) noexcept {
        device->SetRenderTarget(0, into.surface);
        draw_quad(device, 0.0F, 0.0F, static_cast<float>(into.width), static_cast<float>(into.height));
    }

    // Halo's alpha is left as it was: it keeps masks there that later passes blend with (how shiny each surface is, for
    // one), which ambient occlusion, drawn before them, would otherwise wipe
    void draw_post_world(IDirect3DDevice9 *device, const PostFrame &frame) noexcept {
        device->SetRenderTarget(0, frame.target);
        device->SetViewport(&frame.viewport);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        const auto &v = frame.viewport;
        draw_quad(device, static_cast<float>(v.X), static_cast<float>(v.Y), static_cast<float>(v.Width), static_cast<float>(v.Height));
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    }

    void reset_post_drawing(IDirect3DDevice9 *device) noexcept {
        device->SetDepthStencilSurface(nullptr);
        set_states(device);
    }

    bool post_format_supported(IDirect3DDevice9 *device, DWORD usage, D3DRESOURCETYPE type, D3DFORMAT format) noexcept {
        IDirect3D9 *direct3d = nullptr;
        D3DDEVICE_CREATION_PARAMETERS creation;
        D3DDISPLAYMODE mode;
        if(device->GetDirect3D(&direct3d) < 0) {
            return false;
        }
        bool supported = device->GetCreationParameters(&creation) >= 0
            && device->GetDisplayMode(0, &mode) >= 0
            && direct3d->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, mode.Format, usage, type, format) >= 0;
        direct3d->Release();
        return supported;
    }

    bool create_post_shaders(IDirect3DDevice9 *device, const DWORD *const *code, IDirect3DPixelShader9 **shaders, int count) noexcept {
        D3DCAPS9 caps;
        bool ready = device->GetDeviceCaps(&caps) >= 0 && caps.PixelShaderVersion >= D3DPS_VERSION(3, 0);
        for(int i = 0; ready && i < count; i++) {
            ready = device->CreatePixelShader(code[i], &shaders[i]) >= 0;
        }
        if(!ready) {
            for(int i = 0; i < count; i++) {
                release(shaders[i]);
            }
        }
        return ready;
    }

    std::string post_number_text(float value) {
        auto text = std::to_string(value);
        text.erase(text.find_last_not_of('0') + 1);
        if(text.back() == '.') {
            text.pop_back();
        }
        return text;
    }

    void post_setting_command(const char *name, const char *usage, float &value, float least, float most, const std::vector<std::string> &arguments) {
        if(!arguments.empty()) {
            float given;
            if(!parse_float(arguments[0], given) || !(given >= least && given <= most)) {
                console_error(usage);
                return;
            }
            value = given;
        }
        console_print((std::string(name) + ": " + post_number_text(value)).c_str());
    }

    void post_trouble(PostTrouble &trouble, const char *message) noexcept {
        if(!trouble.told && ++trouble.times >= TROUBLE_TIMES) {
            trouble.told = true;
            console_error(message);
        }
    }

    void set_up_post() noexcept {
        add_reset_callback(release_frame);
    }
}
