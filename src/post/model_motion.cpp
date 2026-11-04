// SPDX-License-Identifier: GPL-3.0-only
//
// The motion of Halo's models, for motion blur. While it's wanted, each model part Halo draws before the world's
// transparent surfaces is noted: its vertex and index buffers, what of them it drew, and its bones. Halo uploads a
// model's bones (or a part's own, in its own order) to its vertex shader's register 29 on, three registers each, so
// whatever is in those registers when a part is drawn is what it was skinned with; they're read back then. (Hooking
// the uploads themselves doesn't hold: Direct3D 9 puts its own SetVertexShaderConstantF back in the device's table of
// functions whenever a state block is used.) A part drawn last frame too (the same object, the same buffers, into the
// same target) is drawn again into the motion buffer with the shaders in motion_blur_shaders.hlsl, its vertices
// skinned with both frames' bones, read from a vertex texture.

#include "model_motion.hpp"
#include "motion_blur_shaders.h"
#include "post.hpp"
#include "../halo.hpp"
#include "../memory.hpp"
#include "../render.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Where Halo's vertex shader has the bones, and the most it uploads (63 nodes)
#define BONE_REGISTER 29
#define BONE_REGISTERS (63 * 3)

// Halo's model vertex: position, normal, binormal, tangent, texture coordinates, two bone indices and two weights
#define MODEL_VERTEX_SIZE 68

// The most parts noted a frame, a row of the bone texture each, as wide as two frames of the most bones
#define MOST_PARTS 1024
#define BONE_TEXTURE_WIDTH 512

#define DEVICE_DRAW_INDEXED_PRIMITIVE_INDEX 82

namespace Buh {
    using DrawIndexedPrimitiveFunction = HRESULT (__stdcall *)(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT count, UINT start, UINT primitives);
    static DrawIndexedPrimitiveFunction original_draw_indexed_primitive = nullptr;

    struct PartKey {
        std::uint32_t object;
        const void *vertices;
        const void *target;
        UINT start;
        INT base;

        bool operator==(const PartKey &other) const noexcept {
            return object == other.object && vertices == other.vertices && target == other.target && start == other.start && base == other.base;
        }
    };

    struct PartKeyHash {
        std::size_t operator()(const PartKey &key) const noexcept {
            auto hash = std::hash<const void *>()(key.vertices) ^ (std::hash<const void *>()(key.target) << 1);
            return hash ^ (static_cast<std::size_t>(key.object) * 0x9E3779B1U) ^ (static_cast<std::size_t>(key.start) << 7) ^ static_cast<std::size_t>(key.base);
        }
    };

    // A part as Halo drew it this frame. Its buffers are Halo's, kept by Halo, and only used this frame.
    struct Part {
        PartKey key;
        IDirect3DVertexBuffer9 *vertices;
        UINT offset;
        IDirect3DIndexBuffer9 *indices;
        D3DPRIMITIVETYPE type;
        INT base;
        UINT minimum;
        UINT count;
        UINT start;
        UINT primitives;
        std::size_t bones;
    };

    static bool wanted = false;
    static bool noting = false;
    static bool finished = true;
    static std::vector<Part> parts;
    static std::unordered_set<PartKey, PartKeyHash> noted;
    static std::vector<float> bones;

    // Where each part's bones were last frame
    static std::unordered_map<PartKey, std::size_t, PartKeyHash> last_parts;
    static std::vector<float> last_bones;

    static IDirect3DDevice9 *motion_device = nullptr;
    static bool unavailable = false;
    static IDirect3DVertexShader9 *vertex_shader = nullptr;
    static IDirect3DPixelShader9 *pixel_shader = nullptr;
    static IDirect3DVertexDeclaration9 *declaration = nullptr;
    static IDirect3DTexture9 *bone_texture = nullptr;

    // What of a model vertex the motion is worked out from
    static const D3DVERTEXELEMENT9 model_vertex[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 56, D3DDECLTYPE_SHORT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
        { 0, 60, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
        D3DDECL_END()
    };

    template<typename T> static void release(T *&object) noexcept {
        if(object) {
            object->Release();
            object = nullptr;
        }
    }

    // A part Halo drew, noted once a frame however many passes it's drawn in. First person things (the weapon and arms)
    // aren't: they move with the camera, and stay sharp as it turns.
    static void note(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT count, UINT start, UINT primitives) noexcept {
        std::uint32_t object;
        if(parts.size() >= MOST_PARTS || !drawing_model(object) || drawing_first_person_model()) {
            return;
        }
        IDirect3DVertexBuffer9 *vertices = nullptr;
        IDirect3DIndexBuffer9 *indices = nullptr;
        IDirect3DSurface9 *target = nullptr;
        UINT offset = 0;
        UINT stride = 0;
        bool model = device->GetStreamSource(0, &vertices, &offset, &stride) >= 0 && vertices && stride == MODEL_VERTEX_SIZE
            && device->GetIndices(&indices) >= 0 && indices
            && device->GetRenderTarget(0, &target) >= 0 && target;
        // Halo keeps them, so only where they are is kept, for this frame
        auto *kept_vertices = vertices;
        auto *kept_indices = indices;
        auto *kept_target = target;
        release(vertices);
        release(indices);
        release(target);
        if(!model) {
            return;
        }
        PartKey key = { object, kept_vertices, kept_target, start, base };
        try {
            if(!noted.insert(key).second) {
                return;
            }
            auto at = bones.size();
            bones.resize(at + BONE_REGISTERS * 4);
            if(device->GetVertexShaderConstantF(BONE_REGISTER, &bones[at], BONE_REGISTERS) < 0) {
                bones.resize(at);
                return;
            }
            parts.push_back({ key, kept_vertices, offset, kept_indices, type, base, minimum, count, start, primitives, at });
        }
        catch(...) {
        }
    }

    static HRESULT __stdcall draw_indexed_primitive(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT count, UINT start, UINT primitives) {
        auto result = original_draw_indexed_primitive(device, type, base, minimum, count, start, primitives);
        // Other devices share the table of functions
        if(noting && result >= 0 && device == halo_device()) {
            note(device, type, base, minimum, count, start, primitives);
        }
        return result;
    }

    // Into the device's table of functions, where the draws go on as they were, and again each frame noted if Direct3D
    // put its own back
    static void hook(IDirect3DDevice9 *device) noexcept {
        hook_device_function(device, DEVICE_DRAW_INDEXED_PRIMITIVE_INDEX, draw_indexed_primitive, original_draw_indexed_primitive);
    }

    void want_model_motion() noexcept {
        wanted = true;
    }

    // At the end of each frame: the next one's parts noted if they're wanted. A frame whose parts weren't finished
    // with, or that won't be noted, leaves nothing for the one after to have moved from.
    static void present(IDirect3DDevice9 *device) noexcept {
        if(!finished || !wanted) {
            last_parts.clear();
            last_bones.clear();
        }
        parts.clear();
        noted.clear();
        bones.clear();
        noting = wanted;
        finished = !wanted;
        wanted = false;
        if(noting) {
            hook(device);
        }
    }

    void finish_model_motion() noexcept {
        if(finished) {
            return;
        }
        noting = false;
        finished = true;
        last_parts.clear();
        last_bones.swap(bones);
        bones.clear();
        try {
            for(auto &part : parts) {
                last_parts[part.key] = part.bones;
            }
        }
        catch(...) {
            last_parts.clear();
        }
        parts.clear();
        noted.clear();
    }

    static void release_device() noexcept {
        release(vertex_shader);
        release(pixel_shader);
        release(declaration);
        release(bone_texture);
        motion_device = nullptr;
    }

    // The shaders, the vertex declaration and the bone texture, once per device: a vertex shader 3.0 card that can
    // read 32 bit float textures in it
    static bool prepare(IDirect3DDevice9 *device) noexcept {
        if(motion_device == device && bone_texture) {
            return true;
        }
        if(unavailable) {
            return false;
        }
        if(motion_device != device) {
            release_device();
        }
        D3DCAPS9 caps;
        bool ready = device->GetDeviceCaps(&caps) >= 0 && caps.VertexShaderVersion >= D3DVS_VERSION(3, 0)
            && post_format_supported(device, D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F);
        ready = ready && (vertex_shader || device->CreateVertexShader(model_motion_vertex_shader, &vertex_shader) >= 0);
        ready = ready && (pixel_shader || device->CreatePixelShader(model_motion_shader, &pixel_shader) >= 0);
        ready = ready && (declaration || device->CreateVertexDeclaration(model_vertex, &declaration) >= 0);
        ready = ready && device->CreateTexture(BONE_TEXTURE_WIDTH, MOST_PARTS, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &bone_texture, nullptr) >= 0;
        if(!ready) {
            release_device();
            unavailable = true;
            return false;
        }
        motion_device = device;
        return true;
    }

    bool draw_model_motion(IDirect3DDevice9 *device, IDirect3DSurface9 *target, const float (&now)[4][4], const float (&last)[4][4]) noexcept {
        // The motion's own draws aren't Halo's
        noting = false;
        if(!prepare(device)) {
            return false;
        }

        // The parts drawn into the world's target last frame too whose bones moved (or whose registers past theirs
        // changed, which only costs a draw that finds them still)
        std::vector<std::pair<const Part *, std::size_t>> moving;
        try {
            for(auto &part : parts) {
                if(part.key.target != target) {
                    continue;
                }
                auto found = last_parts.find(part.key);
                if(found == last_parts.end() || std::memcmp(&bones[part.bones], &last_bones[found->second], BONE_REGISTERS * 4 * sizeof(float)) == 0) {
                    continue;
                }
                moving.emplace_back(&part, found->second);
            }
        }
        catch(...) {
            return true;
        }
        if(moving.empty()) {
            return true;
        }

        // A row of bones each: this frame's, then the last's
        D3DLOCKED_RECT locked;
        if(bone_texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD) < 0) {
            return true;
        }
        for(std::size_t i = 0; i < moving.size(); i++) {
            auto *row = reinterpret_cast<float *>(static_cast<std::byte *>(locked.pBits) + i * locked.Pitch);
            std::memcpy(row, &bones[moving[i].first->bones], BONE_REGISTERS * 4 * sizeof(float));
            std::memcpy(row + BONE_REGISTERS * 4, &last_bones[moving[i].second], BONE_REGISTERS * 4 * sizeof(float));
        }
        bone_texture->UnlockRect(0);

        device->SetVertexShader(vertex_shader);
        device->SetPixelShader(pixel_shader);
        device->SetVertexDeclaration(declaration);
        device->SetTexture(D3DVERTEXTEXTURESAMPLER0, bone_texture);
        device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetVertexShaderConstantF(1, now[0], 4);
        device->SetVertexShaderConstantF(5, last[0], 4);
        for(std::size_t i = 0; i < moving.size(); i++) {
            auto &part = *moving[i].first;
            const float row[4] = {
                (static_cast<float>(i) + 0.5F) / MOST_PARTS, 1.0F / BONE_TEXTURE_WIDTH, static_cast<float>(BONE_REGISTERS), 0.0F
            };
            device->SetVertexShaderConstantF(0, row, 1);
            device->SetStreamSource(0, part.vertices, part.offset, MODEL_VERTEX_SIZE);
            device->SetIndices(part.indices);
            device->DrawIndexedPrimitive(part.type, part.base, part.minimum, part.count, part.start, part.primitives);
        }
        device->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
        device->SetVertexShader(nullptr);
        device->SetVertexDeclaration(nullptr);
        device->SetStreamSource(0, nullptr, 0, 0);
        device->SetIndices(nullptr);
        return true;
    }

    void set_up_model_motion() noexcept {
        add_present_callback(present);
        add_reset_callback([]() noexcept {
            release(bone_texture);
            unavailable = false;
        });
    }
}
