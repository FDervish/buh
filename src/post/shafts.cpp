// SPDX-License-Identifier: GPL-3.0-only
//
// Sun shafts: the sky around the sun blurred out from it, so what's in front of the sun casts shafts of light across
// the screen. The sun is the map's: the brightest light of its first sky, which is also where Halo puts the sun's lens
// flare. The shaders in shafts_shaders.hlsl do the work at half size, from the world's depth, which tells the sky from
// what's in front of it.

#include "shafts.hpp"
#include "shafts_shaders.h"
#include "depth.hpp"
#include "post.hpp"
#include "../console.hpp"
#include "../halo.hpp"
#include "../render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The scenario's skies, a block of tag references, and in a sky tag its lights: their colour, power and direction
// (yaw and pitch in radians)
#define SCENARIO_SKIES 0x30
#define SKY_LIGHTS 0xC4
#define SKY_LIGHT_SIZE 0x74
#define SKY_LIGHT_COLOR 0x50
#define SKY_LIGHT_POWER 0x5C
#define SKY_LIGHT_DIRECTION 0x68
#define TAG_REFERENCE_ID 0xC
#define MOST_SKIES 32
#define MOST_SKY_LIGHTS 8

// How far from the sun the sky counts toward the shafts, in screen heights, and how fast they fade along their length
#define SKY_RADIUS 0.6F
#define FADE 1.5F

// The samples each blur takes, as in the shader: the first reaches that much less far than the second, the second's step
#define SAMPLES 12.0F

#define SOURCE_SAMPLER 0

namespace Buh {
    static bool enabled = false;
    static float intensity = 1.0F;
    static float length = 0.75F;

    // The sun's direction given with buh_sun_shafts_direction, in degrees, rather than the map's
    static bool given_direction = false;
    static float given_yaw = 0.0F;
    static float given_pitch = 0.0F;

    enum Pass { MASK, RADIAL, COMPOSITE, PASS_COUNT };
    static const DWORD *const pass_shaders[PASS_COUNT] = { shafts_mask_shader, shafts_radial_shader, shafts_composite_shader };

    static IDirect3DDevice9 *shafts_device = nullptr;
    static bool unavailable = false;
    static IDirect3DPixelShader9 *shaders[PASS_COUNT] = {};
    static D3DFORMAT target_format = D3DFMT_UNKNOWN;
    static PostTarget first = {};
    static PostTarget second = {};

    template<typename T> static T read(const std::byte *data, std::size_t offset) noexcept {
        T value;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    }

    static void direction_from(float yaw, float pitch, float direction[3]) noexcept {
        direction[0] = std::cos(pitch) * std::cos(yaw);
        direction[1] = std::cos(pitch) * std::sin(yaw);
        direction[2] = std::sin(pitch);
    }

    // The map's sun: the brightest light of its first sky, by power and colour
    static bool map_sun(float direction[3], float color[3]) noexcept {
        auto *scenario = scenario_data();
        if(!scenario) {
            return false;
        }
        auto sky_count = read<std::uint32_t>(scenario, SCENARIO_SKIES);
        auto *skies = read<std::byte *>(scenario, SCENARIO_SKIES + 4);
        if(sky_count == 0 || sky_count > MOST_SKIES || !skies) {
            return false;
        }
        auto *sky = get_tag(read<std::uint32_t>(skies, TAG_REFERENCE_ID));
        if(!sky || !sky->data) {
            return false;
        }
        auto light_count = read<std::uint32_t>(sky->data, SKY_LIGHTS);
        auto *lights = read<std::byte *>(sky->data, SKY_LIGHTS + 4);
        if(light_count == 0 || light_count > MOST_SKY_LIGHTS || !lights) {
            return false;
        }
        float brightest = 0.0F;
        for(std::uint32_t i = 0; i < light_count; i++) {
            auto *light = lights + i * SKY_LIGHT_SIZE;
            float rgb[3];
            std::memcpy(rgb, light + SKY_LIGHT_COLOR, sizeof(rgb));
            float brightness = read<float>(light, SKY_LIGHT_POWER) * (0.2126F * rgb[0] + 0.7152F * rgb[1] + 0.0722F * rgb[2]);
            if(brightness > brightest) {
                brightest = brightness;
                direction_from(read<float>(light, SKY_LIGHT_DIRECTION), read<float>(light, SKY_LIGHT_DIRECTION + 4), direction);
                std::copy(rgb, rgb + 3, color);
            }
        }
        return brightest > 0.0F;
    }

    bool current_sun(float direction[3], float color[3]) noexcept {
        if(given_direction) {
            direction_from(given_yaw * 3.14159265F / 180.0F, given_pitch * 3.14159265F / 180.0F, direction);
            float map_direction[3];
            if(!map_sun(map_direction, color)) {
                std::fill(color, color + 3, 1.0F);
            }
            return true;
        }
        return map_sun(direction, color);
    }

    static void release_targets() noexcept {
        release_post_target(first);
        release_post_target(second);
    }

    static void release_device() noexcept {
        release_targets();
        for(auto *&shader : shaders) {
            if(shader) {
                shader->Release();
                shader = nullptr;
            }
        }
        shafts_device = nullptr;
    }

    // The shaders, once per device, and the targets' format: half floats where the card can, so faint shafts don't band
    static bool prepare(IDirect3DDevice9 *device) noexcept {
        if(shafts_device == device) {
            return true;
        }
        release_device();
        target_format = post_format_supported(device, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F)
            ? D3DFMT_A16B16G16R16F : D3DFMT_A8R8G8B8;
        if(!create_post_shaders(device, pass_shaders, shaders, PASS_COUNT)) {
            unavailable = true;
            console_error("buh: sun shafts can't be drawn here (they need pixel shader 3.0)");
            return false;
        }
        shafts_device = device;
        return true;
    }

    static bool prepare_targets(IDirect3DDevice9 *device, UINT width, UINT height) noexcept {
        UINT half_width = (width + 1) / 2;
        UINT half_height = (height + 1) / 2;
        if(first.texture && first.width == half_width && first.height == half_height) {
            return true;
        }
        release_targets();
        bool ready = create_post_target(device, first, half_width, half_height, target_format)
            && create_post_target(device, second, half_width, half_height, target_format);
        if(!ready) {
            release_targets();
            unavailable = true;
            console_error("buh: sun shafts couldn't make their render targets, so they're off until buh_sun_shafts true");
        }
        return ready;
    }

    // Where the sun is on screen, and how much of it shows for facing it: none behind, all within about 45 degrees
    static bool sun_on_screen(const float direction[3], float &u, float &v, float &facing) noexcept {
        WorldView view;
        if(!world_view(view)) {
            return false;
        }
        auto *m = view.world_to_view;
        float x = m[1] * direction[0] + m[4] * direction[1] + m[7] * direction[2];
        float y = m[2] * direction[0] + m[5] * direction[1] + m[8] * direction[2];
        float z = m[3] * direction[0] + m[6] * direction[1] + m[9] * direction[2];
        auto *p = view.projection;
        float clip_x = x * p[0] + y * p[4] + z * p[8];
        float clip_y = x * p[1] + y * p[5] + z * p[9];
        float clip_w = x * p[3] + y * p[7] + z * p[11];
        float size = std::sqrt(x * x + y * y + z * z);
        if(clip_w <= 0.0F || size <= 0.0F) {
            return false;
        }
        u = clip_x / clip_w * 0.5F + 0.5F;
        v = 0.5F - clip_y / clip_w * 0.5F;
        facing = std::clamp((-z / size - 0.3F) / 0.4F, 0.0F, 1.0F);
        return facing > 0.0F;
    }

    // A radial pass, reaching that far toward the sun
    static void radial(IDirect3DDevice9 *device, const PostTarget &from, const PostTarget &into, float reach) noexcept {
        const float pass[4] = { reach, FADE, 0.0F, 0.0F };
        device->SetPixelShaderConstantF(2, pass, 1);
        device->SetTexture(SOURCE_SAMPLER, from.texture);
        draw_post_pass(device, into);
    }

    static void draw_shafts(IDirect3DDevice9 *device, double) noexcept {
        if(!enabled || unavailable) {
            return;
        }
        want_world_depth();
        float direction[3];
        float color[3];
        float u, v, facing;
        if(!current_sun(direction, color) || !sun_on_screen(direction, u, v, facing) || !prepare(device)) {
            return;
        }
        PostFrame frame;
        if(!begin_post_frame(device, frame)) {
            return;
        }
        if(!prepare_targets(device, frame.viewport.Width, frame.viewport.Height) || !bind_world_depth(device, frame)) {
            end_post_frame(device, frame);
            return;
        }
        float width = static_cast<float>(frame.viewport.Width);
        float height = static_cast<float>(frame.viewport.Height);
        const float constants[2][4] = {
            { 1.0F / static_cast<float>(first.width), 1.0F / static_cast<float>(first.height), 0.0F, 0.0F },
            { u, v, 1.0F / SKY_RADIUS, width / height }
        };
        device->SetPixelShaderConstantF(0, constants[0], 2);

        device->SetPixelShader(shaders[MASK]);
        device->SetTexture(SOURCE_SAMPLER, frame.world->texture);
        draw_post_pass(device, first);
        device->SetPixelShader(shaders[RADIAL]);
        radial(device, first, second, length / SAMPLES);
        radial(device, second, first, length);

        // Screen blended, as the bloom is
        float strength = intensity * facing;
        const float light[4] = { color[0] * strength, color[1] * strength, color[2] * strength, 0.0F };
        device->SetPixelShaderConstantF(2, light, 1);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        device->SetPixelShader(shaders[COMPOSITE]);
        device->SetTexture(SOURCE_SAMPLER, first.texture);
        draw_post_world(device, frame);
        end_post_frame(device, frame);
    }

    static void shafts_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], enabled)) {
            console_error("usage: buh_sun_shafts [true|false]");
            return;
        }
        console_print(enabled ? "buh_sun_shafts: true" : "buh_sun_shafts: false");
        if(enabled && !world_drawing_available()) {
            console_error("buh: sun shafts can't be drawn with this haloce.exe");
        }
        // Turned on again, they try again
        if(enabled) {
            unavailable = false;
            retry_world_depth();
        }
    }

    static void intensity_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_sun_shafts_intensity", "usage: buh_sun_shafts_intensity [amount], 0 to 4", intensity, 0.0F, 4.0F, arguments);
    }

    static void length_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_sun_shafts_length", "usage: buh_sun_shafts_length [amount], 0 to 1", length, 0.0F, 1.0F, arguments);
    }

    static void direction_command(const std::vector<std::string> &arguments) {
        if(arguments.size() == 1 && arguments[0] == "map") {
            given_direction = false;
        }
        else if(arguments.size() == 2) {
            float yaw, pitch;
            if(!parse_float(arguments[0], yaw) || !parse_float(arguments[1], pitch) || !(pitch >= -90.0F && pitch <= 90.0F)) {
                console_error("usage: buh_sun_shafts_direction [<yaw> <pitch>|map], in degrees");
                return;
            }
            given_direction = true;
            given_yaw = yaw;
            given_pitch = pitch;
        }
        else if(!arguments.empty()) {
            console_error("usage: buh_sun_shafts_direction [<yaw> <pitch>|map], in degrees");
            return;
        }
        if(given_direction) {
            console_print(("buh_sun_shafts_direction: " + post_number_text(given_yaw) + " " + post_number_text(given_pitch)).c_str());
            return;
        }
        float direction[3];
        float color[3];
        if(!map_sun(direction, color)) {
            console_print("buh_sun_shafts_direction: map (this map's sky has no light)");
            return;
        }
        float yaw = std::atan2(direction[1], direction[0]) * 180.0F / 3.14159265F;
        float pitch = std::asin(std::clamp(direction[2], -1.0F, 1.0F)) * 180.0F / 3.14159265F;
        console_print(("buh_sun_shafts_direction: map (" + post_number_text(yaw) + " " + post_number_text(pitch) + ")").c_str());
    }

    void set_up_shafts() noexcept {
        add_world_draw_callback(draw_shafts);
        add_reset_callback(release_targets);
        add_console_command("buh_sun_shafts", "[true|false]", 0, 1, shafts_command);
        add_console_command("buh_sun_shafts_intensity", "[amount]", 0, 1, intensity_command);
        add_console_command("buh_sun_shafts_length", "[amount]", 0, 1, length_command);
        add_console_command("buh_sun_shafts_direction", "[<yaw> <pitch>|map]", 0, 2, direction_command);
    }
}
