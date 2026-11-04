// SPDX-License-Identifier: GPL-3.0-only
//
// The world's depth, for the post processes that read it (ambient occlusion, sun shafts and motion blur). Halo draws
// into a depth buffer that can't be read, so while one of them is on, it draws into one that can: an INTZ texture
// (a depth format the cards since Direct3D 10 or so read as a texture), set in place of its own at the end of each
// frame, and wherever Halo sets its own again. Once nothing has asked for it for a while, Halo's own goes back.
//
// Halo draws first person things (the weapon and arms) first, with near and far clip distances of their own, and
// marks them 1 in the stencil buffer so the world doesn't draw over them. So once a frame, that's drawn into a mask the
// shaders read, telling them which depths are first person, to be turned into distance their own way.

#include "depth.hpp"
#include "depth_shaders.h"
#include "../console.hpp"
#include "../halo.hpp"
#include "../render.hpp"

#include <cstdio>
#include <string>
#include <vector>

// How many frames without anything asking for the depth before Halo's own depth buffer goes back
#define UNWANTED_FRAMES 10

// The stencil bit Halo marks first person things with (stencil mode 1: always, replacing with 1, a write mask of 1)
#define FIRST_PERSON_STENCIL 1

#define MASK_SAMPLER 6
#define DEPTH_SAMPLER 7
#define DEPTH_CONSTANTS 4

namespace Buh {
    static const D3DFORMAT INTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

    static bool wanted = false;
    static int unwanted_frames = 0;
    static bool unavailable = false;

    // The readable depth buffer, Halo's own (a reference kept to put it back), and whether the readable one has been
    // set since the last frame was presented, so it has all of this one
    static IDirect3DDevice9 *depth_device = nullptr;
    static IDirect3DTexture9 *depth_texture = nullptr;
    static IDirect3DSurface9 *depth_surface = nullptr;
    static IDirect3DSurface9 *own_depth = nullptr;
    static bool bound = false;

    // Whether Present has come through buh's hook, where the depth buffer is swapped, and what's kept the world's depth
    // from being read lately
    static bool presented = false;
    static PostTrouble trouble = {};

    // What setting the readable depth buffer last gave, and why it was last let go, if it hasn't been set again since
    static HRESULT set_result = D3D_OK;
    static const char *let_go = nullptr;

    // The first person mask, the size of the depth buffer, and whether it's been drawn this frame
    static PostTarget mask = {};
    static bool masked = false;

    // The mask's shader and buh_depth_view's, once per device
    enum Shader { MASK, VIEW, SHADER_COUNT };
    static IDirect3DDevice9 *shader_device = nullptr;
    static IDirect3DPixelShader9 *shaders[SHADER_COUNT] = {};

    // buh_depth_view
    static bool viewing = false;

    template<typename T> static void release(T *&object) noexcept {
        if(object) {
            object->Release();
            object = nullptr;
        }
    }

    // Everything of the device's, which a reset mustn't find; Halo's own depth buffer is set again by the reset
    static void release_depth() noexcept {
        set_depth_substitute(nullptr, nullptr);
        release(depth_surface);
        release(depth_texture);
        release(own_depth);
        release_post_target(mask);
        bound = false;
    }

    // Halo's own depth buffer back, and the readable one let go
    static void restore(IDirect3DDevice9 *device) noexcept {
        set_depth_substitute(nullptr, nullptr);
        if(own_depth) {
            device->SetDepthStencilSurface(own_depth);
        }
        release_depth();
        let_go = "nothing had asked for it for a while";
    }

    static void reset() noexcept {
        release_depth();
        let_go = "the device was reset";
    }

    // The readable depth buffer, the size of Halo's own, which it can't be in place of if that's multisampled
    static bool create(IDirect3DDevice9 *device, IDirect3DSurface9 *own) noexcept {
        D3DSURFACE_DESC description;
        if(own->GetDesc(&description) < 0 || description.MultiSampleType != D3DMULTISAMPLE_NONE
        || !post_format_supported(device, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, INTZ)) {
            return false;
        }
        if(device->CreateTexture(description.Width, description.Height, 1, D3DUSAGE_DEPTHSTENCIL, INTZ, D3DPOOL_DEFAULT, &depth_texture, nullptr) < 0) {
            depth_texture = nullptr;
            return false;
        }
        if(depth_texture->GetSurfaceLevel(0, &depth_surface) < 0) {
            depth_surface = nullptr;
            release(depth_texture);
            return false;
        }
        return true;
    }

    // At the end of each frame: the readable depth buffer set for the next while anything wants it
    static void present(IDirect3DDevice9 *device) noexcept {
        presented = true;
        masked = false;
        if(device != depth_device) {
            if(depth_device) {
                release_depth();
                let_go = "Halo's device changed";
            }
            depth_device = device;
        }
        if(!wanted) {
            if(depth_texture && ++unwanted_frames > UNWANTED_FRAMES) {
                restore(device);
            }
            return;
        }
        wanted = false;
        unwanted_frames = 0;
        if(unavailable) {
            return;
        }
        IDirect3DSurface9 *current = nullptr;
        if(device->GetDepthStencilSurface(&current) < 0) {
            current = nullptr;
        }
        if(current && current != depth_surface && current != own_depth) {
            release(own_depth);
            own_depth = current;
            current = nullptr;
        }
        release(current);
        if(!own_depth || (!depth_texture && !create(device, own_depth))) {
            release_depth();
            unavailable = true;
            console_error("buh: the world's depth can't be read here (it needs INTZ depth textures, without multisampling), so ambient occlusion, sun shafts and motion blur are off");
            return;
        }
        set_depth_substitute(own_depth, depth_surface);
        set_result = device->SetDepthStencilSurface(depth_surface);
        bound = set_result >= 0;
        if(bound) {
            let_go = nullptr;
        }
    }

    void want_world_depth() noexcept {
        wanted = true;
    }

    // What's set in place of buh's depth buffer, for saying what it is
    static std::string depth_text(IDirect3DSurface9 *depth) noexcept {
        D3DSURFACE_DESC description;
        if(depth->GetDesc(&description) < 0) {
            return "one";
        }
        return "format " + std::to_string(description.Format) + ", " + std::to_string(description.Width) + "x" + std::to_string(description.Height);
    }

    static bool prepare_shaders(IDirect3DDevice9 *device) noexcept {
        if(shader_device == device) {
            return true;
        }
        for(auto *&shader : shaders) {
            release(shader);
        }
        shader_device = nullptr;
        const DWORD *const code[SHADER_COUNT] = { mask_shader, depth_view_shader };
        if(!create_post_shaders(device, code, shaders, SHADER_COUNT)) {
            return false;
        }
        shader_device = device;
        return true;
    }

    // The first person mask: 1 where Halo's stencil buffer marks first person things, from a pass that only draws there.
    // The depth texture is the depth buffer while it's drawn, so it's not read then.
    static bool draw_mask(IDirect3DDevice9 *device, const D3DSURFACE_DESC &depth) noexcept {
        if(!prepare_shaders(device)) {
            return false;
        }
        if(!mask.texture || mask.width != depth.Width || mask.height != depth.Height) {
            release_post_target(mask);
            if(!create_post_target(device, mask, depth.Width, depth.Height, D3DFMT_X8R8G8B8)) {
                return false;
            }
        }
        device->SetTexture(DEPTH_SAMPLER, nullptr);
        device->SetTexture(MASK_SAMPLER, nullptr);
        device->SetRenderTarget(0, mask.surface);
        device->SetDepthStencilSurface(depth_surface);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0F, 0);
        device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
        device->SetRenderState(D3DRS_STENCILREF, FIRST_PERSON_STENCIL);
        device->SetRenderState(D3DRS_STENCILMASK, FIRST_PERSON_STENCIL);
        device->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
        device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
        device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
        device->SetPixelShader(shaders[MASK]);
        draw_post_pass(device, mask);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetDepthStencilSurface(nullptr);
        return true;
    }

    bool bind_world_depth(IDirect3DDevice9 *device, const PostFrame &frame) noexcept {
        // It's been said why already
        if(unavailable) {
            return false;
        }
        WorldView view;
        D3DSURFACE_DESC description;
        if(!presented) {
            post_trouble(trouble, "buh: the world's depth can't be read: Present never comes through buh, which swaps Halo's depth buffer there");
            return false;
        }
        if(!bound || !depth_surface) {
            char error[16];
            std::snprintf(error, sizeof(error), "0x%08lX", static_cast<unsigned long>(set_result));
            std::string why = set_result < 0 ? std::string("Direct3D won't set buh's depth buffer in place of Halo's (error ") + error + ")"
                : let_go ? std::string("buh's depth buffer was let go, as ") + let_go
                : std::string("buh's depth buffer hasn't been set since it was asked for");
            post_trouble(trouble, ("buh: the world's depth can't be read: " + why).c_str());
            return false;
        }
        if(frame.depth != depth_surface) {
            std::string why = !frame.depth ? "no depth buffer is set as the world is drawn"
                : frame.depth == own_depth ? "the depth buffer buh swapped out (" + depth_text(frame.depth) + ") is set back as the world is drawn, without going through buh"
                : "another depth buffer (" + depth_text(frame.depth) + ") is set in place of buh's as the world is drawn";
            post_trouble(trouble, ("buh: the world's depth can't be read: " + why).c_str());
            return false;
        }
        if(!world_view(view)) {
            post_trouble(trouble, "buh: the world's depth can't be read: Halo's camera can't be read");
            return false;
        }
        if(depth_surface->GetDesc(&description) < 0) {
            return false;
        }
        if(!masked) {
            masked = true;
            if(!draw_mask(device, description)) {
                unavailable = true;
                console_error("buh: the world's depth can't be read here (it needs pixel shader 3.0 to tell first person things apart), so ambient occlusion, sun shafts and motion blur are off");
                return false;
            }
        }
        trouble.times = 0;

        // The projection's terms that turn depth into distance, the world's and first person's: Halo rebuilds them for
        // first person things from their near and far clip distances, n and f, as -(f + n) / (f - n) and
        // -2nf / (f - n)
        auto *p = view.projection;
        float first_person_terms[2] = { p[14], p[10] };
        float near_distance;
        float far_distance;
        if(first_person_clip(near_distance, far_distance)) {
            first_person_terms[0] = -2.0F * near_distance * far_distance / (far_distance - near_distance);
            first_person_terms[1] = -(far_distance + near_distance) / (far_distance - near_distance);
        }
        float width = static_cast<float>(description.Width);
        float height = static_cast<float>(description.Height);
        const auto &v = frame.viewport;
        const float constants[3][4] = {
            { p[14], p[10], 1.0F / p[0], 1.0F / p[5] },
            { p[8], p[9], first_person_terms[0], first_person_terms[1] },
            { static_cast<float>(v.Width) / width, static_cast<float>(v.Height) / height, static_cast<float>(v.X) / width, static_cast<float>(v.Y) / height }
        };
        device->SetPixelShaderConstantF(DEPTH_CONSTANTS, constants[0], 3);
        for(DWORD sampler : { MASK_SAMPLER, DEPTH_SAMPLER }) {
            device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        }
        device->SetTexture(MASK_SAMPLER, mask.texture);
        device->SetTexture(DEPTH_SAMPLER, depth_texture);
        return true;
    }

    // buh_depth_view, in place of the world
    static void draw_view(IDirect3DDevice9 *device, double) noexcept {
        if(!viewing) {
            return;
        }
        want_world_depth();
        if(!prepare_shaders(device)) {
            viewing = false;
            console_error("buh: the depth view can't be drawn here (it needs pixel shader 3.0)");
            return;
        }
        PostFrame frame;
        if(!begin_post_frame(device, frame)) {
            return;
        }
        if(bind_world_depth(device, frame)) {
            device->SetPixelShader(shaders[VIEW]);
            draw_post_world(device, frame);
        }
        end_post_frame(device, frame);
    }

    static void depth_view_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], viewing)) {
            console_error("usage: buh_depth_view [true|false]");
            return;
        }
        console_print(viewing ? "buh_depth_view: true" : "buh_depth_view: false");
        if(viewing) {
            retry_world_depth();
        }
    }

    void retry_world_depth() noexcept {
        unavailable = false;
        trouble = {};
    }

    void set_up_depth() noexcept {
        add_present_callback(present);
        add_reset_callback(reset);
        add_console_command("buh_depth_view", "[true|false]", 0, 1, depth_view_command);
    }

    // The depth view goes over everything else, so it's set up last
    void set_up_depth_view() noexcept {
        add_world_draw_callback(draw_view);
    }
}
