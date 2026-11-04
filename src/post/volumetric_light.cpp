// SPDX-License-Identifier: GPL-3.0-only
//
// Volumetric light: the light the air scatters toward the camera, from the sun and from Halo's dynamic lights (the
// flashlight, muzzle flashes, glowing projectiles, explosions). So shafts of sunlight show in the air from any side,
// where the sun shafts only show looking toward the sun. The sun's is shadowed by the level: its collision geometry,
// the polygons Halo's bullets hit, all but the sky's, invisible walls' and what light goes through (glass, water,
// energy), drawn from the sun into a shadow map around the camera, and again once the camera has gone a way or the sun
// has moved. The sun's direction is the sun shafts'.
// The dynamic lights' isn't shadowed, but the world in front of a light hides its light past it. The shaders in
// volumetric_light_shaders.hlsl do the work, at half size from the world's depth.

#include "volumetric_light.hpp"
#include "volumetric_light_shaders.h"
#include "depth.hpp"
#include "post.hpp"
#include "shafts.hpp"
#include "../console.hpp"
#include "../halo.hpp"
#include "../render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The structure BSP's collision materials (a shader reference each) and its collision geometry
#define STRUCTURE_BSP_COLLISION_MATERIALS 0xA4
#define STRUCTURE_BSP_COLLISION_BSP 0xB0
#define COLLISION_MATERIAL_SIZE 0x14
#define COLLISION_MATERIAL_SHADER_ID 0xC
#define COLLISION_BSP_SIZE 0x60
#define MOST_COLLISION_MATERIALS 512

// The collision geometry's polygons, each going round its edges: an edge has two vertices, the next edge round each
// polygon either side, and those polygons. A polygon on an edge's second side goes round it the other way.
#define COLLISION_BSP_SURFACES 0x3C
#define COLLISION_BSP_EDGES 0x48
#define COLLISION_BSP_VERTICES 0x54
#define SURFACE_SIZE 0xC
#define SURFACE_FIRST_EDGE 0x4
#define SURFACE_FLAGS 0x8
#define SURFACE_MATERIAL 0xA
#define SURFACE_INVISIBLE 0x2
#define EDGE_SIZE 0x18
#define EDGE_VERTICES 0x0
#define EDGE_EDGES 0x8
#define EDGE_SURFACES 0x10
#define VERTEX_SIZE 0x10
#define MOST_SURFACES 131072
#define MOST_EDGES 262144
#define MOST_VERTICES 131072
#define MOST_POLYGON_VERTICES 8

#define TAG_CLASS_SHADER_ENVIRONMENT 0x73656E76

// A light tag's cone: the cosines of the angles off its axis it's all there within (-1 for an omnidirectional light)
// and none of it is past
#define LIGHT_COSINE_FALLOFF 0x1C
#define LIGHT_COSINE_CUTOFF 0x20
#define LIGHT_DEFINITION_SIZE 0x24

// The shadow map's size; how far from the camera the sun's light is gathered, in world units (a tenth of one is about
// a foot); and how far the shadow map reaches each way from its middle: that far, and as far again as the camera can
// go from its middle before it's drawn again
#define SHADOW_SIZE 2048
#define REACH 48.0F
#define SHADOW_MARGIN 16.0F
#define SHADOW_REACH (REACH + SHADOW_MARGIN)

// How much further from the sun than the level a place can be and be lit, in world units
#define SHADOW_BIAS 0.1F

// The dynamic lights worked out, in three arrays of registers from c16; and how much of a light's light a world unit of
// air scatters, which is more than the sun's in the shaders, for a light's colour is far dimmer than the sun's light
#define MOST_LIGHTS 16
#define LIGHT_REGISTER 16
#define LIGHT_DENSITY 0.05F

// A light further than this past its radius isn't worked out
#define FARTHEST_LIGHT 200.0F

#define SHADOW_SAMPLER 0
#define LIGHT_SAMPLER 1

namespace Buh {
    static bool enabled = false;
    static float intensity = 1.0F;
    static float light_intensity = 1.0F;

    // How much the air hazes where the sun lights all of the way through it, against the shafts where shadow breaks it
    // up, which show in full
    static float haze = 0.1F;

    enum Pass { MARCH, BLUR, LIGHTS, APPLY, PASS_COUNT };
    static const DWORD *const pass_shaders[PASS_COUNT] = { volumetric_march_shader, volumetric_blur_shader, volumetric_lights_shader, volumetric_apply_shader };

    static IDirect3DDevice9 *volumetric_device = nullptr;
    static bool unavailable = false;
    static IDirect3DPixelShader9 *shaders[PASS_COUNT] = {};

    // The light at half size, and the other half of its blur: its colour in rgb, the distance it's in front of in a
    static PostTarget gathered = {};
    static PostTarget blurred = {};

    // The sun's shadow map, how far toward the sun the level is in each texel, and its depth buffer; and what draws it,
    // which a card that can't draw 32 bit floats does without, and does without the sun's light
    static bool shadows_unavailable = false;
    static IDirect3DVertexShader9 *shadow_vertex_shader = nullptr;
    static IDirect3DPixelShader9 *shadow_pixel_shader = nullptr;
    static IDirect3DVertexDeclaration9 *declaration = nullptr;
    static PostTarget shadow = {};
    static IDirect3DSurface9 *shadow_depth = nullptr;

    // The level's polygons that block the sun, as triangles in a managed vertex buffer, which a reset keeps, and the
    // corners of the box they're in. They're found again when the level is another.
    struct LevelKey {
        const std::byte *structure_bsp;
        const std::byte *surfaces;
        const std::byte *vertices;
        std::uint32_t surface_count;
        std::uint32_t vertex_count;
        float first_vertex[3];

        bool operator==(const LevelKey &other) const noexcept {
            return std::memcmp(this, &other, sizeof(*this)) == 0;
        }
    };
    static LevelKey level_key = {};
    static bool level_read = false;
    static IDirect3DVertexBuffer9 *level = nullptr;
    static UINT level_triangles = 0;
    static float level_low[3] = {};
    static float level_high[3] = {};

    // Counted up each time the level's polygons are read, and what the shadow map was last drawn for: that level, the
    // sun's way, and the middle across and up it
    static unsigned level_generation = 0;
    static struct {
        bool drawn;
        unsigned level;
        float toward[3];
        float middle_across;
        float middle_up;
    } shadow_drawn = {};

    // What's kept the sun's light out lately
    static PostTrouble trouble = {};

    template<typename T> static void release(T *&object) noexcept {
        if(object) {
            object->Release();
            object = nullptr;
        }
    }

    template<typename T> static T read(const std::byte *data, std::size_t offset) noexcept {
        T value;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    }

    static void release_targets() noexcept {
        release_post_target(gathered);
        release_post_target(blurred);
        release_post_target(shadow);
        release(shadow_depth);
        shadow_drawn = {};
    }

    static void release_level() noexcept {
        release(level);
        level_triangles = 0;
        level_key = {};
        level_read = false;
        level_generation++;
    }

    static void release_device() noexcept {
        release_targets();
        release_level();
        for(auto *&shader : shaders) {
            release(shader);
        }
        release(shadow_vertex_shader);
        release(shadow_pixel_shader);
        release(declaration);
        volumetric_device = nullptr;
    }

    // The shaders, once per device: pixel shader 3.0 and half float targets for the light, and vertex shader 3.0 and
    // 32 bit float targets for the sun's shadow map
    static bool prepare(IDirect3DDevice9 *device) noexcept {
        if(volumetric_device == device) {
            return true;
        }
        release_device();
        if(!post_format_supported(device, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F)
        || !create_post_shaders(device, pass_shaders, shaders, PASS_COUNT)) {
            unavailable = true;
            console_error("buh: volumetric light can't be drawn here (it needs pixel shader 3.0 and half float render targets)");
            return false;
        }
        static const D3DVERTEXELEMENT9 position[] = {
            { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            D3DDECL_END()
        };
        D3DCAPS9 caps;
        shadows_unavailable = device->GetDeviceCaps(&caps) < 0 || caps.VertexShaderVersion < D3DVS_VERSION(3, 0)
            || !post_format_supported(device, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_R32F)
            || device->CreateVertexShader(volumetric_shadow_vertex_shader, &shadow_vertex_shader) < 0
            || device->CreatePixelShader(volumetric_shadow_shader, &shadow_pixel_shader) < 0
            || device->CreateVertexDeclaration(position, &declaration) < 0;
        if(shadows_unavailable) {
            console_error("buh: the sun's volumetric light can't be shadowed here (it needs vertex shader 3.0 and 32 bit float render targets), so it's left out");
        }
        volumetric_device = device;
        return true;
    }

    static bool prepare_targets(IDirect3DDevice9 *device, UINT width, UINT height) noexcept {
        UINT half_width = (width + 1) / 2;
        UINT half_height = (height + 1) / 2;
        if(gathered.texture && gathered.width == half_width && gathered.height == half_height) {
            return true;
        }
        release_post_target(gathered);
        release_post_target(blurred);
        bool ready = create_post_target(device, gathered, half_width, half_height, D3DFMT_A16B16G16R16F)
            && create_post_target(device, blurred, half_width, half_height, D3DFMT_A16B16G16R16F);
        if(!ready) {
            release_targets();
            unavailable = true;
            console_error("buh: volumetric light couldn't make its render targets, so it's off until buh_volumetric_light true");
        }
        return ready;
    }

    static bool prepare_shadow_targets(IDirect3DDevice9 *device) noexcept {
        if(shadow.texture && shadow_depth) {
            return true;
        }
        release_post_target(shadow);
        release(shadow_depth);
        bool ready = create_post_target(device, shadow, SHADOW_SIZE, SHADOW_SIZE, D3DFMT_R32F)
            && device->CreateDepthStencilSurface(SHADOW_SIZE, SHADOW_SIZE, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &shadow_depth, nullptr) >= 0;
        if(!ready) {
            release_post_target(shadow);
            shadow_depth = nullptr;
            shadows_unavailable = true;
            console_error("buh: the sun's volumetric light couldn't make its shadow map, so it's left out");
        }
        return ready;
    }

    // A tag block's count and elements, if they can be read and there aren't more than most
    static bool read_block(const std::byte *at, std::uint32_t most, std::size_t size, std::uint32_t &count, const std::byte *&elements) noexcept {
        if(!readable(at, 8)) {
            return false;
        }
        count = block_count(at);
        elements = block_elements(at);
        return count <= most && (count == 0 || readable(elements, count * size));
    }

    // The level's polygons that block the sun, fanned into triangles: those that aren't invisible and whose material
    // is an environment shader. The sky's polygons have no material, and glass, water and energy other shaders.
    static bool level_polygons(const std::byte *structure_bsp, const std::byte *surfaces, std::uint32_t surface_count, const std::byte *edges,
        std::uint32_t edge_count, const std::byte *vertices, std::uint32_t vertex_count, std::vector<float> &positions) {
        std::uint32_t material_count;
        const std::byte *materials;
        if(!read_block(structure_bsp + STRUCTURE_BSP_COLLISION_MATERIALS, MOST_COLLISION_MATERIALS, COLLISION_MATERIAL_SIZE, material_count, materials)) {
            return false;
        }
        std::vector<bool> blocking(material_count);
        for(std::uint32_t i = 0; i < material_count; i++) {
            auto *tag = get_tag(read<std::uint32_t>(materials, i * COLLISION_MATERIAL_SIZE + COLLISION_MATERIAL_SHADER_ID));
            blocking[i] = tag && tag->primary_class == TAG_CLASS_SHADER_ENVIRONMENT;
        }
        for(std::uint32_t surface = 0; surface < surface_count; surface++) {
            auto *at = surfaces + surface * SURFACE_SIZE;
            auto material = read<std::int16_t>(at, SURFACE_MATERIAL);
            if((std::to_integer<std::uint8_t>(at[SURFACE_FLAGS]) & SURFACE_INVISIBLE) || material < 0
            || static_cast<std::uint32_t>(material) >= material_count || !blocking[material]) {
                continue;
            }
            std::uint32_t corners[MOST_POLYGON_VERTICES];
            std::size_t corner_count = 0;
            auto first = read<std::uint32_t>(at, SURFACE_FIRST_EDGE);
            auto edge = first;
            bool closed = false;
            while(edge < edge_count && corner_count < MOST_POLYGON_VERTICES) {
                auto *e = edges + edge * EDGE_SIZE;
                int side = read<std::uint32_t>(e, EDGE_SURFACES + 4) == surface ? 1 : 0;
                corners[corner_count++] = read<std::uint32_t>(e, EDGE_VERTICES + side * 4);
                edge = read<std::uint32_t>(e, EDGE_EDGES + side * 4);
                if(edge == first) {
                    closed = true;
                    break;
                }
            }
            if(!closed || corner_count < 3 || std::any_of(corners, corners + corner_count, [&](std::uint32_t v) { return v >= vertex_count; })) {
                continue;
            }
            for(std::size_t i = 1; i + 1 < corner_count; i++) {
                for(auto corner : { corners[0], corners[i], corners[i + 1] }) {
                    float point[3];
                    std::memcpy(point, vertices + corner * VERTEX_SIZE, sizeof(point));
                    positions.insert(positions.end(), point, point + 3);
                }
            }
        }
        return true;
    }

    // The loaded level's polygons that block the sun, in the vertex buffer, read again whenever the level is another
    // @return false if there are none, or they can't be read
    static bool prepare_level(IDirect3DDevice9 *device) noexcept {
        auto *structure_bsp = level_structure_bsp();
        std::uint32_t bsp_count, surface_count, edge_count, vertex_count;
        const std::byte *bsp, *surfaces, *edges, *vertices;
        if(!structure_bsp || !read_block(structure_bsp + STRUCTURE_BSP_COLLISION_BSP, 1, COLLISION_BSP_SIZE, bsp_count, bsp) || bsp_count != 1
        || !read_block(bsp + COLLISION_BSP_SURFACES, MOST_SURFACES, SURFACE_SIZE, surface_count, surfaces)
        || !read_block(bsp + COLLISION_BSP_EDGES, MOST_EDGES, EDGE_SIZE, edge_count, edges)
        || !read_block(bsp + COLLISION_BSP_VERTICES, MOST_VERTICES, VERTEX_SIZE, vertex_count, vertices) || vertex_count == 0) {
            release_level();
            return false;
        }
        LevelKey key = { structure_bsp, surfaces, vertices, surface_count, vertex_count, {} };
        std::memcpy(key.first_vertex, vertices, sizeof(key.first_vertex));
        if(level_read && key == level_key) {
            return level != nullptr;
        }
        release_level();
        level_key = key;
        level_read = true;

        std::vector<float> positions;
        try {
            if(!level_polygons(structure_bsp, surfaces, surface_count, edges, edge_count, vertices, vertex_count, positions) || positions.empty()) {
                return false;
            }
        }
        catch(...) {
            return false;
        }
        std::copy(positions.begin(), positions.begin() + 3, level_low);
        std::copy(positions.begin(), positions.begin() + 3, level_high);
        for(std::size_t i = 0; i < positions.size(); i++) {
            level_low[i % 3] = std::min(level_low[i % 3], positions[i]);
            level_high[i % 3] = std::max(level_high[i % 3], positions[i]);
        }
        void *data;
        UINT size = static_cast<UINT>(positions.size() * sizeof(float));
        if(device->CreateVertexBuffer(size, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &level, nullptr) < 0) {
            level = nullptr;
            return false;
        }
        if(level->Lock(0, 0, &data, 0) < 0) {
            release(level);
            return false;
        }
        std::memcpy(data, positions.data(), size);
        level->Unlock();
        level_triangles = static_cast<UINT>(positions.size() / 9);
        return true;
    }

    // Halo's world to view: view = scale * rotation * world + translation, the rotation's columns where the world's x,
    // y and z go
    struct Camera {
        float scale;
        float rotation[3][3];
        float translation[3];
        float position[3];
    };

    static Camera camera_of(const WorldView &view) noexcept {
        Camera camera;
        auto *m = view.world_to_view;
        camera.scale = m[0] != 0.0F ? m[0] : 1.0F;
        for(int row = 0; row < 3; row++) {
            for(int column = 0; column < 3; column++) {
                camera.rotation[row][column] = m[1 + column * 3 + row];
            }
            camera.translation[row] = m[10 + row];
        }
        // The rotation is orthonormal, so its inverse is its transpose
        for(int column = 0; column < 3; column++) {
            camera.position[column] = 0.0F;
            for(int row = 0; row < 3; row++) {
                camera.position[column] -= camera.rotation[row][column] * camera.translation[row] / camera.scale;
            }
        }
        return camera;
    }

    // A way in the world, in view space
    static void turned(const Camera &camera, const float (&way)[3], float (&into)[3]) noexcept {
        for(int row = 0; row < 3; row++) {
            into[row] = camera.rotation[row][0] * way[0] + camera.rotation[row][1] * way[1] + camera.rotation[row][2] * way[2];
        }
    }

    static float dot(const float *a, const float *b) noexcept {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    static void normalize(float (&v)[3]) noexcept {
        float length = std::sqrt(dot(v, v));
        for(auto &c : v) {
            c /= length;
        }
    }

    // The level drawn from the sun into the shadow map, around the camera: across it, up it, and toward the sun. The
    // level doesn't move, so it's only drawn again for another sun or level, or once the camera has gone the margin
    // from the middle it was drawn around, snapped to whole texels. Gives the rows from view space to the shadow map,
    // for MarchPS.
    static bool draw_shadow_map(IDirect3DDevice9 *device, const Camera &camera, const float (&sun)[3], float (&to_shadow)[3][4], float &bias) noexcept {
        if(!prepare_shadow_targets(device)) {
            return false;
        }
        float toward[3] = { sun[0], sun[1], sun[2] };
        normalize(toward);
        float across[3];
        float pick[3] = { 0.0F, 0.0F, 1.0F };
        if(std::fabs(toward[2]) > 0.99F) {
            pick[0] = 1.0F;
            pick[2] = 0.0F;
        }
        across[0] = toward[1] * pick[2] - toward[2] * pick[1];
        across[1] = toward[2] * pick[0] - toward[0] * pick[2];
        across[2] = toward[0] * pick[1] - toward[1] * pick[0];
        normalize(across);
        float up[3] = {
            across[1] * toward[2] - across[2] * toward[1],
            across[2] * toward[0] - across[0] * toward[2],
            across[0] * toward[1] - across[1] * toward[0]
        };

        float camera_across = dot(camera.position, across);
        float camera_up = dot(camera.position, up);
        bool redraw = !shadow_drawn.drawn || shadow_drawn.level != level_generation || std::memcmp(shadow_drawn.toward, toward, sizeof(toward)) != 0
            || std::fabs(camera_across - shadow_drawn.middle_across) > SHADOW_MARGIN || std::fabs(camera_up - shadow_drawn.middle_up) > SHADOW_MARGIN;
        if(redraw) {
            float texel = 2.0F * SHADOW_REACH / SHADOW_SIZE;
            shadow_drawn.middle_across = std::round(camera_across / texel) * texel;
            shadow_drawn.middle_up = std::round(camera_up / texel) * texel;
        }
        float middle_across = shadow_drawn.middle_across;
        float middle_up = shadow_drawn.middle_up;

        // How far from the sun the level's box goes: depth 0 at its nearest, 1 at its furthest
        float nearest = 0.0F, furthest = 0.0F;
        for(int corner = 0; corner < 8; corner++) {
            float point[3] = { corner & 1 ? level_high[0] : level_low[0], corner & 2 ? level_high[1] : level_low[1], corner & 4 ? level_high[2] : level_low[2] };
            float depth = -dot(point, toward);
            nearest = corner == 0 ? depth : std::min(nearest, depth);
            furthest = corner == 0 ? depth : std::max(furthest, depth);
        }
        nearest -= 1.0F;
        furthest += 1.0F;
        float depth_scale = 1.0F / (furthest - nearest);
        bias = SHADOW_BIAS * depth_scale;

        // From view space, as 0 to 1 texture coordinates (v going down) and the depth: each a way in the world times a
        // scale plus an offset, where world = rotation^T (view - translation) / scale
        const float ways[3][3] = { { across[0], across[1], across[2] }, { up[0], up[1], up[2] }, { -toward[0], -toward[1], -toward[2] } };
        const float scales[3] = { 0.5F / SHADOW_REACH, -0.5F / SHADOW_REACH, depth_scale };
        const float offsets[3] = { 0.5F - 0.5F * middle_across / SHADOW_REACH, 0.5F + 0.5F * middle_up / SHADOW_REACH, -nearest * depth_scale };
        for(int row = 0; row < 3; row++) {
            float way[3];
            turned(camera, ways[row], way);
            float scale = scales[row] / camera.scale;
            for(int c = 0; c < 3; c++) {
                to_shadow[row][c] = way[c] * scale;
            }
            to_shadow[row][3] = offsets[row] - dot(camera.translation, way) * scale;
        }
        if(!redraw) {
            return true;
        }
        shadow_drawn.drawn = true;
        shadow_drawn.level = level_generation;
        std::copy(toward, toward + 3, shadow_drawn.toward);

        // From the world: across and up from -1 to 1, moved half a texel so texels line up with where they're read
        const float rows[3][4] = {
            { across[0] / SHADOW_REACH, across[1] / SHADOW_REACH, across[2] / SHADOW_REACH, -middle_across / SHADOW_REACH - 1.0F / SHADOW_SIZE },
            { up[0] / SHADOW_REACH, up[1] / SHADOW_REACH, up[2] / SHADOW_REACH, -middle_up / SHADOW_REACH + 1.0F / SHADOW_SIZE },
            { -toward[0] * depth_scale, -toward[1] * depth_scale, -toward[2] * depth_scale, -nearest * depth_scale }
        };
        device->SetRenderTarget(0, shadow.surface);
        device->SetDepthStencilSurface(shadow_depth);
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0F, 0);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetVertexDeclaration(declaration);
        device->SetVertexShader(shadow_vertex_shader);
        device->SetPixelShader(shadow_pixel_shader);
        device->SetVertexShaderConstantF(0, rows[0], 3);
        device->SetStreamSource(0, level, 0, 3 * sizeof(float));
        D3DCAPS9 caps;
        UINT most = device->GetDeviceCaps(&caps) >= 0 && caps.MaxPrimitiveCount > 0 ? std::min<UINT>(caps.MaxPrimitiveCount, 65535) : 65535;
        for(UINT first = 0; first < level_triangles; first += most) {
            device->DrawPrimitive(D3DPT_TRIANGLELIST, first * 3, std::min(most, level_triangles - first));
        }
        device->SetStreamSource(0, nullptr, 0, 0);
        device->SetVertexShader(nullptr);
        return true;
    }

    // The dynamic lights most worth working out, up to MOST_LIGHTS, in view space, into the registers MarchPS reads:
    // where each is, its light and cone scale, and its axis and cone offset
    struct LightRegisters {
        float places[MOST_LIGHTS][4];
        float colors[MOST_LIGHTS][4];
        float axes[MOST_LIGHTS][4];
    };

    static int gather_lights(const Camera &camera, LightRegisters &registers) noexcept {
        const RenderedLight *list;
        std::size_t count = light_intensity > 0.0F ? rendered_lights(list) : 0;
        struct Candidate {
            const RenderedLight *light;
            float view[3];
            float worth;
        };
        Candidate candidates[128];
        std::size_t found = 0;
        for(std::size_t i = 0; i < count && found < 128; i++) {
            auto &light = list[i];
            float brightness = 0.2126F * light.color[0] + 0.7152F * light.color[1] + 0.0722F * light.color[2];
            if(!(light.radius > 0.0F) || !(brightness > 0.0F) || !readable(light.definition, LIGHT_DEFINITION_SIZE)) {
                continue;
            }
            float view[3];
            turned(camera, light.position, view);
            for(int c = 0; c < 3; c++) {
                view[c] = view[c] * camera.scale + camera.translation[c];
            }
            float distance = std::sqrt(dot(view, view));
            // Wholly behind the camera (which looks down -z), or too far to matter
            if(view[2] > light.radius || distance - light.radius > FARTHEST_LIGHT) {
                continue;
            }
            float r2 = light.radius * light.radius;
            candidates[found++] = { &light, { view[0], view[1], view[2] }, brightness * r2 / std::max(distance * distance, r2) };
        }
        auto used = std::min<std::size_t>(found, MOST_LIGHTS);
        std::partial_sort(candidates, candidates + used, candidates + found, [](const Candidate &a, const Candidate &b) { return a.worth > b.worth; });
        for(std::size_t i = 0; i < used; i++) {
            auto &light = *candidates[i].light;
            float axis[3];
            turned(camera, light.forward, axis);
            float axis_length = std::sqrt(dot(axis, axis));
            float falloff = read<float>(light.definition, LIGHT_COSINE_FALLOFF);
            float cutoff = read<float>(light.definition, LIGHT_COSINE_CUTOFF);
            float cone_scale = 0.0F;
            float cone_offset = 1.0F;
            if(falloff > -0.999F && falloff > cutoff && axis_length > 0.0F) {
                cone_scale = 1.0F / (falloff - cutoff);
                cone_offset = -cutoff * cone_scale;
            }
            float strength = LIGHT_DENSITY * light_intensity;
            for(int c = 0; c < 3; c++) {
                registers.places[i][c] = candidates[i].view[c];
                registers.colors[i][c] = std::clamp(light.color[c], 0.0F, 1.0F) * strength;
                registers.axes[i][c] = axis_length > 0.0F ? axis[c] / axis_length : 0.0F;
            }
            registers.places[i][3] = 1.0F / (light.radius * light.radius);
            registers.colors[i][3] = cone_scale;
            registers.axes[i][3] = cone_offset;
        }
        return static_cast<int>(used);
    }

    // One way of the blur, from one target into the other
    static void blur(IDirect3DDevice9 *device, const PostTarget &from, const PostTarget &into, float x, float y) noexcept {
        const float step[4] = { x, y, 0.0F, 0.0F };
        device->SetPixelShaderConstantF(10, step, 1);
        device->SetTexture(LIGHT_SAMPLER, from.texture);
        draw_post_pass(device, into);
    }

    static void draw_volumetric_light(IDirect3DDevice9 *device, double) noexcept {
        if(!enabled || unavailable) {
            return;
        }
        want_world_depth();
        WorldView view;
        if(!world_view(view) || !prepare(device)) {
            return;
        }
        auto camera = camera_of(view);
        float sun[3];
        float sun_color[3];
        bool sun_shines = intensity > 0.0F && current_sun(sun, sun_color);
        if(sun_shines && !shadows_unavailable) {
            if(prepare_level(device)) {
                trouble.times = 0;
            }
            else {
                sun_shines = false;
                post_trouble(trouble, "buh: volumetric light can't read the level's geometry to shadow the sun's light with, so it's left out");
            }
        }
        else {
            sun_shines = false;
        }
        LightRegisters light_registers;
        int light_count = gather_lights(camera, light_registers);
        if(!sun_shines && light_count == 0) {
            return;
        }

        PostFrame frame;
        if(!begin_post_frame(device, frame)) {
            return;
        }
        if(!prepare_targets(device, frame.viewport.Width, frame.viewport.Height)) {
            end_post_frame(device, frame);
            return;
        }
        float to_shadow[3][4] = {};
        float bias = 0.0F;
        if(sun_shines) {
            sun_shines = draw_shadow_map(device, camera, sun, to_shadow, bias);
            reset_post_drawing(device);
        }
        if(!bind_world_depth(device, frame)) {
            end_post_frame(device, frame);
            return;
        }

        float sun_view[3];
        turned(camera, sun, sun_view);
        normalize(sun_view);
        float strength = sun_shines ? intensity : 0.0F;
        const float constants[4] = { 1.0F / static_cast<float>(gathered.width), 1.0F / static_cast<float>(gathered.height), static_cast<float>(frame.viewport.Width), static_cast<float>(frame.viewport.Height) };
        const float sun_constants[3][4] = {
            { sun_color[0] * strength, sun_color[1] * strength, sun_color[2] * strength, bias },
            { sun_view[0], sun_view[1], sun_view[2], REACH },
            { 0.0F, haze, 0.0F, 0.0F }
        };
        // The lights' count as a loop counts: how many times, from 0, by 1
        const int loop[4] = { light_count, 0, 1, 0 };
        device->SetPixelShaderConstantF(0, constants, 1);
        device->SetPixelShaderConstantF(1, to_shadow[0], 3);
        device->SetPixelShaderConstantF(7, sun_constants[0], 3);
        device->SetPixelShaderConstantI(0, loop, 1);
        if(light_count > 0) {
            device->SetPixelShaderConstantF(LIGHT_REGISTER, light_registers.places[0], light_count);
            device->SetPixelShaderConstantF(LIGHT_REGISTER + MOST_LIGHTS, light_registers.colors[0], light_count);
            device->SetPixelShaderConstantF(LIGHT_REGISTER + 2 * MOST_LIGHTS, light_registers.axes[0], light_count);
        }
        device->SetTexture(SHADOW_SAMPLER, sun_shines ? shadow.texture : nullptr);
        device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
        device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
        device->SetSamplerState(SHADOW_SAMPLER, D3DSAMP_BORDERCOLOR, 0xFFFFFFFF);
        device->SetSamplerState(LIGHT_SAMPLER, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(LIGHT_SAMPLER, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

        device->SetPixelShader(shaders[MARCH]);
        draw_post_pass(device, gathered);
        device->SetTexture(SHADOW_SAMPLER, nullptr);
        // The sun's light blurred, and then the dynamic lights' added, which has no noise to blur
        device->SetPixelShader(shaders[BLUR]);
        blur(device, gathered, blurred, constants[0], 0.0F);
        device->SetPixelShader(shaders[LIGHTS]);
        blur(device, blurred, gathered, 0.0F, constants[1]);

        // Screen blended, as the bloom is
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        device->SetPixelShader(shaders[APPLY]);
        device->SetTexture(LIGHT_SAMPLER, gathered.texture);
        draw_post_world(device, frame);
        end_post_frame(device, frame);
    }

    static void volumetric_light_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], enabled)) {
            console_error("usage: buh_volumetric_light [true|false]");
            return;
        }
        console_print(enabled ? "buh_volumetric_light: true" : "buh_volumetric_light: false");
        if(enabled && !world_drawing_available()) {
            console_error("buh: volumetric light can't be drawn with this haloce.exe");
        }
        // Turned on again, it tries again
        if(enabled) {
            unavailable = false;
            trouble = {};
            retry_world_depth();
        }
    }

    static void intensity_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_volumetric_light_intensity", "usage: buh_volumetric_light_intensity [amount], 0 to 4", intensity, 0.0F, 4.0F, arguments);
    }

    static void lights_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_volumetric_light_lights", "usage: buh_volumetric_light_lights [amount], 0 to 4", light_intensity, 0.0F, 4.0F, arguments);
    }

    static void haze_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_volumetric_light_haze", "usage: buh_volumetric_light_haze [amount], 0 to 1", haze, 0.0F, 1.0F, arguments);
    }

    void set_up_volumetric_light() noexcept {
        add_world_draw_callback(draw_volumetric_light);
        add_reset_callback(release_targets);
        add_console_command("buh_volumetric_light", "[true|false]", 0, 1, volumetric_light_command);
        add_console_command("buh_volumetric_light_intensity", "[amount]", 0, 1, intensity_command);
        add_console_command("buh_volumetric_light_lights", "[amount]", 0, 1, lights_command);
        add_console_command("buh_volumetric_light_haze", "[amount]", 0, 1, haze_command);
    }
}
