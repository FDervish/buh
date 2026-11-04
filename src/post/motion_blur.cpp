// SPDX-License-Identifier: GPL-3.0-only
//
// Motion blur, for the camera's motion and for whatever moves by itself. Once Halo has drawn the world's opaque
// surfaces, how far each pixel moved since the last frame goes into a motion buffer: the camera's motion, from the
// world's depth, and then each model's own over it (model_motion.cpp). Before the HUD, the world is blurred along it by
// McGuire's reconstruction filter (motion_blur_shaders.hlsl), which blurs what moves over what's behind it rather than
// smearing the two together. It's scaled to a shutter time rather than to the frame, so it looks the same at any frame
// rate.

#include "motion_blur.hpp"
#include "motion_blur_shaders.h"
#include "depth.hpp"
#include "model_motion.hpp"
#include "post.hpp"
#include "../console.hpp"
#include "../render.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// The frame time amount 1 blurs over: a 30 frames a second frame, as Reach's
#define SHUTTER_SECONDS (1.0 / 30.0)

// The most it blurs, as much of the screen's height, which is also how wide its tiles are, and the most the shutter is
// past the frame
#define MOST_BLUR 0.05F
#define SMALLEST_TILE 8
#define MOST_SHUTTER 3.0F

// A frame whose camera turned or moved more than this is a cut (a respawn or a cutscene), and isn't blurred
#define CUT_RADIANS 0.6F
#define CUT_DISTANCE 3.0F

#define SOURCE_SAMPLER 0
#define MOTION_SAMPLER 1
#define NEIGHBOUR_SAMPLER 2

namespace Buh {
    static bool enabled = false;
    static float amount = 0.5F;
    static bool objects = true;
    static bool viewing = false;

    enum Pass { CAMERA, TILE_MAX, NEIGHBOUR_MAX, GATHER, VIEW, PASS_COUNT };
    static const DWORD *const pass_shaders[PASS_COUNT] = {
        camera_motion_shader, tile_max_shader, neighbour_max_shader, gather_shader, motion_view_shader
    };

    static IDirect3DDevice9 *blur_device = nullptr;
    static bool unavailable = false;
    static IDirect3DPixelShader9 *shaders[PASS_COUNT] = {};

    // How far each pixel moved (in pixels; blue where a model's own motion is), its longest in each tile along a row,
    // in each tile, and around each tile
    static PostTarget motion = {};
    static PostTarget tile_rows = {};
    static PostTarget tile_max = {};
    static PostTarget neighbour_max = {};
    static bool motion_ready = false;
    static bool models_unavailable = false;

    // Whether the opaque world has come through buh's hook since motion blur last drew, and what's kept the motion
    // from being worked out lately, other than the world's depth, which says so itself
    static bool opaque_seen = false;
    static PostTrouble trouble = {};

    // The camera the last frame was drawn with
    static struct {
        float world_to_view[13];
        float projection[16];
        bool valid;
    } last = {};

    // Halo's world to view transform: a scale, then a rotation (its columns where the world's x, y and z go), then a
    // translation
    struct Transform {
        float scale;
        float rotation[3][3];
        float translation[3];
    };

    static Transform transform_of(const float *m) noexcept {
        Transform t;
        t.scale = m[0];
        for(int row = 0; row < 3; row++) {
            for(int column = 0; column < 3; column++) {
                t.rotation[row][column] = m[1 + column * 3 + row];
            }
            t.translation[row] = m[10 + row];
        }
        return t;
    }

    static bool invert(const float (&m)[3][3], float (&inverse)[3][3]) noexcept {
        float determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if(std::fabs(determinant) < 1e-12F) {
            return false;
        }
        for(int row = 0; row < 3; row++) {
            for(int column = 0; column < 3; column++) {
                int r0 = (column + 1) % 3, r1 = (column + 2) % 3, c0 = (row + 1) % 3, c1 = (row + 2) % 3;
                inverse[row][column] = (m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0]) / determinant;
            }
        }
        return true;
    }

    // From this frame's view space to the last's (rows of a 3x4 matrix), if the camera didn't cut between them
    static bool reprojection(const float *now_matrix, const float *last_matrix, float (&rows)[3][4]) noexcept {
        auto now = transform_of(now_matrix);
        auto before = transform_of(last_matrix);
        float inverse[3][3];
        float before_inverse[3][3];
        if(now.scale == 0.0F || before.scale == 0.0F || !invert(now.rotation, inverse) || !invert(before.rotation, before_inverse)) {
            return false;
        }
        // The turn between them, and the camera's place in the world each time
        float turn[3][3];
        float trace = 0.0F;
        float position_now[3];
        float position_before[3];
        for(int row = 0; row < 3; row++) {
            for(int column = 0; column < 3; column++) {
                turn[row][column] = 0.0F;
                for(int k = 0; k < 3; k++) {
                    turn[row][column] += before.rotation[row][k] * inverse[k][column];
                }
            }
            trace += turn[row][row];
            position_now[row] = 0.0F;
            position_before[row] = 0.0F;
            for(int k = 0; k < 3; k++) {
                position_now[row] -= inverse[row][k] * now.translation[k] / now.scale;
                position_before[row] -= before_inverse[row][k] * before.translation[k] / before.scale;
            }
        }
        float moved = std::sqrt(std::pow(position_now[0] - position_before[0], 2.0F) + std::pow(position_now[1] - position_before[1], 2.0F)
            + std::pow(position_now[2] - position_before[2], 2.0F));
        float turned = std::acos(std::clamp((trace - 1.0F) / 2.0F, -1.0F, 1.0F));
        if(turned > CUT_RADIANS || moved > CUT_DISTANCE) {
            return false;
        }
        // last view = (s' / s) R' R^-1 (view - t) + t'
        float ratio = before.scale / now.scale;
        for(int row = 0; row < 3; row++) {
            float offset = before.translation[row];
            for(int column = 0; column < 3; column++) {
                rows[row][column] = ratio * turn[row][column];
                offset -= rows[row][column] * now.translation[column];
            }
            rows[row][3] = offset;
        }
        return true;
    }

    // From the world to clip space with a camera, a column each (a row vector times it): the world to view as a 4x4,
    // times the projection
    static void world_to_clip(const float *world_to_view, const float *projection, float (&columns)[4][4]) noexcept {
        auto t = transform_of(world_to_view);
        float view[4][4] = {};
        for(int row = 0; row < 3; row++) {
            for(int column = 0; column < 3; column++) {
                view[row][column] = t.scale * t.rotation[column][row];
            }
            view[3][row] = t.translation[row];
        }
        view[3][3] = 1.0F;
        for(int column = 0; column < 4; column++) {
            for(int row = 0; row < 4; row++) {
                columns[column][row] = 0.0F;
                for(int k = 0; k < 4; k++) {
                    columns[column][row] += view[row][k] * projection[k * 4 + column];
                }
            }
        }
    }

    static void release_targets() noexcept {
        release_post_target(motion);
        release_post_target(tile_rows);
        release_post_target(tile_max);
        release_post_target(neighbour_max);
        motion_ready = false;
    }

    static void release_device() noexcept {
        release_targets();
        for(auto *&shader : shaders) {
            if(shader) {
                shader->Release();
                shader = nullptr;
            }
        }
        blur_device = nullptr;
    }

    // The shaders, once per device. The motion is half floats, which the card has to draw to.
    static bool prepare(IDirect3DDevice9 *device) noexcept {
        if(blur_device == device) {
            return true;
        }
        release_device();
        if(!post_format_supported(device, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F)
        || !create_post_shaders(device, pass_shaders, shaders, PASS_COUNT)) {
            unavailable = true;
            console_error("buh: motion blur can't be drawn here (it needs pixel shader 3.0 and half float render targets)");
            return false;
        }
        blur_device = device;
        return true;
    }

    static UINT tile_size(UINT height) noexcept {
        return std::max<UINT>(static_cast<UINT>(std::ceil(MOST_BLUR * static_cast<float>(height))), SMALLEST_TILE);
    }

    // The targets for a world this size, made again when it changes
    static bool prepare_targets(IDirect3DDevice9 *device, UINT width, UINT height) noexcept {
        if(motion.texture && motion.width == width && motion.height == height) {
            return true;
        }
        release_targets();
        UINT tile = tile_size(height);
        UINT tiles_across = (width + tile - 1) / tile;
        UINT tiles_down = (height + tile - 1) / tile;
        bool ready = create_post_target(device, motion, width, height, D3DFMT_A16B16G16R16F)
            && create_post_target(device, tile_rows, tiles_across, height, D3DFMT_A16B16G16R16F)
            && create_post_target(device, tile_max, tiles_across, tiles_down, D3DFMT_A16B16G16R16F)
            && create_post_target(device, neighbour_max, tiles_across, tiles_down, D3DFMT_A16B16G16R16F);
        if(!ready) {
            release_targets();
            unavailable = true;
            console_error("buh: motion blur couldn't make its render targets, so it's off until buh_motion_blur true");
        }
        return ready;
    }

    static void set_sizes(IDirect3DDevice9 *device, const PostTarget &of) noexcept {
        float width = static_cast<float>(of.width);
        float height = static_cast<float>(of.height);
        const float sizes[4] = { 1.0F / width, 1.0F / height, width, height };
        device->SetPixelShaderConstantF(0, sizes, 1);
    }

    static void point_sampled(IDirect3DDevice9 *device, DWORD sampler) noexcept {
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }

    // Once the world's opaque surfaces are drawn: how far each pixel moved, for the camera and then for the models
    static void build_motion(IDirect3DDevice9 *device) noexcept {
        motion_ready = false;
        if(!enabled || unavailable) {
            last.valid = false;
            finish_model_motion();
            return;
        }
        opaque_seen = true;
        want_world_depth();
        if(objects && !models_unavailable) {
            want_model_motion();
        }
        WorldView view;
        if(!world_view(view)) {
            post_trouble(trouble, "buh: motion blur can't work out the motion: Halo's camera can't be read as the world is drawn");
            last.valid = false;
            finish_model_motion();
            return;
        }
        float rows[3][4];
        PostFrame frame;
        bool followed = last.valid && reprojection(view.world_to_view, last.world_to_view, rows);
        if(last.valid && !followed) {
            post_trouble(trouble, "buh: motion blur can't work out the motion: the camera jumps too far from each frame to the next");
        }
        if(followed && prepare(device) && begin_post_frame(device, frame)) {
            if(prepare_targets(device, frame.viewport.Width, frame.viewport.Height) && bind_world_depth(device, frame)) {
                auto *p = last.projection;
                const float projection[3][4] = {
                    { p[0], p[4], p[8], p[12] },
                    { p[1], p[5], p[9], p[13] },
                    { p[3], p[7], p[11], p[15] }
                };
                const float settings[4] = { 0.0F, 0.0F, 0.0F, 1.0F };
                set_sizes(device, motion);
                device->SetPixelShaderConstantF(1, rows[0], 3);
                device->SetPixelShaderConstantF(7, projection[0], 3);
                device->SetPixelShaderConstantF(10, settings, 1);
                device->SetPixelShader(shaders[CAMERA]);
                draw_post_pass(device, motion);
                if(objects && !models_unavailable) {
                    float now_clip[4][4];
                    float last_clip[4][4];
                    world_to_clip(view.world_to_view, view.projection, now_clip);
                    world_to_clip(last.world_to_view, last.projection, last_clip);
                    if(!draw_model_motion(device, frame.target, now_clip, last_clip)) {
                        models_unavailable = true;
                        console_error("buh: motion blur can't blur models for their own motion here (it needs vertex shader 3.0 reading float textures), so it's the camera's alone");
                    }
                }
                motion_ready = true;
            }
            end_post_frame(device, frame);
        }
        finish_model_motion();
        std::copy(view.world_to_view, view.world_to_view + 13, last.world_to_view);
        std::copy(view.projection, view.projection + 16, last.projection);
        last.valid = true;
    }

    // A tile pass, one way along a tile, from one target into another
    static void tile_pass(IDirect3DDevice9 *device, const PostTarget &from, const PostTarget &into, float across, float down) noexcept {
        const float way[4] = { across, down, static_cast<float>(from.width), static_cast<float>(from.height) };
        device->SetPixelShaderConstantF(11, way, 1);
        device->SetTexture(SOURCE_SAMPLER, from.texture);
        draw_post_pass(device, into);
    }

    // Before the HUD: the world blurred along the motion, or the motion shown in its place
    static void draw_motion_blur(IDirect3DDevice9 *device, double seconds) noexcept {
        if(!enabled || unavailable) {
            return;
        }
        // The motion is worked out as the opaque world is drawn, which has to come through buh's hook
        if(!motion_ready) {
            if(!opaque_seen) {
                post_trouble(trouble, "buh: motion blur can't work out the motion: Halo's opaque world never comes through buh's hook");
            }
            opaque_seen = false;
            return;
        }
        opaque_seen = false;
        trouble.times = 0;
        motion_ready = false;
        float shutter = seconds > 0.0 ? static_cast<float>(std::min(amount * SHUTTER_SECONDS / seconds, static_cast<double>(MOST_SHUTTER))) : 0.0F;
        if(shutter <= 0.0F && !viewing) {
            return;
        }
        PostFrame frame;
        if(!begin_post_frame(device, frame)) {
            return;
        }
        if(frame.viewport.Width != motion.width || frame.viewport.Height != motion.height || !bind_world_depth(device, frame)) {
            end_post_frame(device, frame);
            return;
        }
        float height = static_cast<float>(frame.viewport.Height);
        UINT tile = tile_size(frame.viewport.Height);
        const float settings[4] = { viewing ? 1.0F : shutter, MOST_BLUR * height, static_cast<float>(tile), 0.0F };
        device->SetPixelShaderConstantF(10, settings, 1);
        for(DWORD sampler : { SOURCE_SAMPLER, MOTION_SAMPLER, NEIGHBOUR_SAMPLER }) {
            point_sampled(device, sampler);
        }

        if(viewing) {
            device->SetPixelShader(shaders[VIEW]);
            device->SetTexture(MOTION_SAMPLER, motion.texture);
            draw_post_world(device, frame);
            end_post_frame(device, frame);
            return;
        }

        // The longest motion in each tile, along its rows then down, and around each tile
        set_sizes(device, motion);
        device->SetPixelShader(shaders[TILE_MAX]);
        tile_pass(device, motion, tile_rows, 1.0F, 0.0F);
        tile_pass(device, tile_rows, tile_max, 0.0F, 1.0F);
        set_sizes(device, tile_max);
        device->SetPixelShader(shaders[NEIGHBOUR_MAX]);
        device->SetTexture(SOURCE_SAMPLER, tile_max.texture);
        draw_post_pass(device, neighbour_max);

        // Each pixel blurred along its neighbourhood's motion
        set_sizes(device, motion);
        const float tiles[4] = { 1.0F / static_cast<float>(neighbour_max.width), 1.0F / static_cast<float>(neighbour_max.height), 0.0F, 0.0F };
        device->SetPixelShaderConstantF(11, tiles, 1);
        device->SetPixelShader(shaders[GATHER]);
        device->SetTexture(SOURCE_SAMPLER, frame.world->texture);
        device->SetTexture(MOTION_SAMPLER, motion.texture);
        device->SetTexture(NEIGHBOUR_SAMPLER, neighbour_max.texture);
        draw_post_world(device, frame);
        end_post_frame(device, frame);
    }

    static void motion_blur_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], enabled)) {
            console_error("usage: buh_motion_blur [true|false]");
            return;
        }
        console_print(enabled ? "buh_motion_blur: true" : "buh_motion_blur: false");
        if(enabled && !opaque_drawing_available()) {
            console_error("buh: motion blur can't be drawn with this haloce.exe");
        }
        // Turned on again, it tries again
        if(enabled) {
            unavailable = false;
            models_unavailable = false;
            trouble = {};
            retry_world_depth();
        }
    }

    static void amount_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_motion_blur_amount", "usage: buh_motion_blur_amount [amount], 0 to 1", amount, 0.0F, 1.0F, arguments);
    }

    static void objects_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], objects)) {
            console_error("usage: buh_motion_blur_objects [true|false]");
            return;
        }
        console_print(objects ? "buh_motion_blur_objects: true" : "buh_motion_blur_objects: false");
    }

    static void view_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], viewing)) {
            console_error("usage: buh_motion_view [true|false]");
            return;
        }
        console_print(viewing ? "buh_motion_view: true" : "buh_motion_view: false");
        if(viewing && !enabled) {
            console_error("buh: the motion view shows what motion blur works out, so it needs buh_motion_blur true");
        }
    }

    void set_up_motion_blur() noexcept {
        set_up_model_motion();
        add_opaque_draw_callback(build_motion);
        add_world_draw_callback(draw_motion_blur);
        add_reset_callback(release_targets);
        add_console_command("buh_motion_blur", "[true|false]", 0, 1, motion_blur_command);
        add_console_command("buh_motion_blur_amount", "[amount]", 0, 1, amount_command);
        add_console_command("buh_motion_blur_objects", "[true|false]", 0, 1, objects_command);
        add_console_command("buh_motion_view", "[true|false]", 0, 1, view_command);
    }
}
