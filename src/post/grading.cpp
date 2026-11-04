// SPDX-License-Identifier: GPL-3.0-only
//
// Colour grading over the world, last of buh's post processes before the HUD, so the bloom is graded along with it.
// Each colour is looked up in a table that's built here whenever a setting changes: a LUT image's grade if one's
// given, then the adjustments on top. However much is set, the shader (grading_shader.hlsl) does one lookup.

#include "grading.hpp"
#include "grading_shader.h"
#include "post.hpp"
#include "../console.hpp"
#include "../render.hpp"
#include "../sprites/image.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// The table's size each way
#define TABLE_SIZE 32

// The smallest and largest LUT images, by their table's size each way
#define SMALLEST_LUT 2
#define LARGEST_LUT 64

// How much temperature 1 takes red up and blue down, or -1 the other way
#define TEMPERATURE_SHIFT 0.2F

namespace Buh {
    static bool enabled = false;

    // How much of the grade to use, and the adjustments: warmer or cooler, more or less colourful, more or less
    // contrast
    static float strength = 1.0F;
    static float temperature = 0.0F;
    static float saturation = 1.0F;
    static float contrast = 1.0F;

    // The LUT image given, as its table of colours from 0 to 1: red, green and blue, red changing fastest, then green,
    // then blue
    static std::string lut_file;
    static std::vector<float> lut;
    static int lut_size = 0;

    static IDirect3DDevice9 *grading_device = nullptr;
    static bool unavailable = false;
    static IDirect3DPixelShader9 *shader = nullptr;
    static IDirect3DVolumeTexture9 *table = nullptr;
    static bool table_stale = true;

    static float luma(const float color[3]) noexcept {
        return 0.2126F * color[0] + 0.7152F * color[1] + 0.0722F * color[2];
    }

    // A colour from the LUT image's table, between its entries
    static void look_up(float color[3]) noexcept {
        int low[3];
        int high[3];
        float along[3];
        for(int c = 0; c < 3; c++) {
            float at = std::clamp(color[c], 0.0F, 1.0F) * static_cast<float>(lut_size - 1);
            low[c] = std::min(static_cast<int>(at), lut_size - 1);
            high[c] = std::min(low[c] + 1, lut_size - 1);
            along[c] = at - static_cast<float>(low[c]);
        }
        float result[3] = {};
        for(int corner = 0; corner < 8; corner++) {
            float weight = 1.0F;
            int index[3];
            for(int c = 0; c < 3; c++) {
                bool upper = corner & (1 << c);
                index[c] = upper ? high[c] : low[c];
                weight *= upper ? along[c] : 1.0F - along[c];
            }
            const float *entry = &lut[((index[2] * lut_size + index[1]) * lut_size + index[0]) * 3];
            for(int c = 0; c < 3; c++) {
                result[c] += entry[c] * weight;
            }
        }
        std::copy(result, result + 3, color);
    }

    // An S curve about the middle that keeps black and white where they are: steeper through the middle above 1,
    // flatter below
    static float curve(float value, float steepness) noexcept {
        return value < 0.5F
            ? 0.5F * std::pow(2.0F * value, steepness)
            : 1.0F - 0.5F * std::pow(2.0F - 2.0F * value, steepness);
    }

    // The adjustments, in order: temperature (keeping white as bright), saturation, then contrast
    static void adjust(float color[3]) noexcept {
        if(temperature != 0.0F) {
            const float gains[3] = { 1.0F + TEMPERATURE_SHIFT * temperature, 1.0F, 1.0F - TEMPERATURE_SHIFT * temperature };
            float white = luma(gains);
            for(int c = 0; c < 3; c++) {
                color[c] *= gains[c] / white;
            }
        }
        float gray = luma(color);
        for(int c = 0; c < 3; c++) {
            color[c] = curve(std::clamp(gray + (color[c] - gray) * saturation, 0.0F, 1.0F), contrast);
        }
    }

    // Whether there's anything to grade, rather than every colour staying as it is
    static bool grading() noexcept {
        return strength > 0.0F && (lut_size > 0 || temperature != 0.0F || saturation != 1.0F || contrast != 1.0F);
    }

    // The table from the LUT image and the adjustments
    static bool fill_table() noexcept {
        D3DLOCKED_BOX box;
        if(table->LockBox(0, &box, nullptr, 0) < 0) {
            return false;
        }
        for(int blue = 0; blue < TABLE_SIZE; blue++) {
            for(int green = 0; green < TABLE_SIZE; green++) {
                auto *row = reinterpret_cast<std::uint32_t *>(static_cast<std::byte *>(box.pBits) + blue * box.SlicePitch + green * box.RowPitch);
                for(int red = 0; red < TABLE_SIZE; red++) {
                    float color[3] = { red / (TABLE_SIZE - 1.0F), green / (TABLE_SIZE - 1.0F), blue / (TABLE_SIZE - 1.0F) };
                    if(lut_size > 0) {
                        look_up(color);
                    }
                    adjust(color);
                    std::uint32_t pixel = 0xFF000000;
                    for(int c = 0; c < 3; c++) {
                        pixel |= static_cast<std::uint32_t>(std::lround(std::clamp(color[c], 0.0F, 1.0F) * 255.0F)) << ((2 - c) * 8);
                    }
                    row[red] = pixel;
                }
            }
        }
        table->UnlockBox(0);
        table_stale = false;
        return true;
    }

    static void release_device() noexcept {
        if(shader) {
            shader->Release();
            shader = nullptr;
        }
        if(table) {
            table->Release();
            table = nullptr;
        }
        grading_device = nullptr;
    }

    // The shader and the table, once per device; both survive a reset
    static bool prepare(IDirect3DDevice9 *device) noexcept {
        if(grading_device == device) {
            return true;
        }
        release_device();
        D3DCAPS9 caps;
        const DWORD *const code[1] = { grading_shader };
        if(device->GetDeviceCaps(&caps) < 0 || !(caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP)
        || !create_post_shaders(device, code, &shader, 1)
        || device->CreateVolumeTexture(TABLE_SIZE, TABLE_SIZE, TABLE_SIZE, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &table, nullptr) < 0) {
            table = nullptr;
            release_device();
            unavailable = true;
            console_error("buh: colour grading can't be drawn here (it needs pixel shader 3.0 and volume textures)");
            return false;
        }
        table_stale = true;
        grading_device = device;
        return true;
    }

    static void draw_grading(IDirect3DDevice9 *device, double) noexcept {
        if(!enabled || unavailable || !grading() || !prepare(device) || (table_stale && !fill_table())) {
            return;
        }
        PostFrame frame;
        if(!begin_post_frame(device, frame)) {
            return;
        }
        const float constants[4] = { (TABLE_SIZE - 1.0F) / TABLE_SIZE, 0.5F / TABLE_SIZE, strength, 0.0F };
        device->SetPixelShaderConstantF(0, constants, 1);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        device->SetPixelShader(shader);
        device->SetTexture(0, frame.world->texture);
        device->SetTexture(1, table);
        draw_post_world(device, frame);
        end_post_frame(device, frame);
    }

    // A LUT image: a strip of squares side by side, one for each step of blue from left to right, with red going
    // across each square and green going down. A table of 32 each way is 1024 by 32.
    static bool load_lut(const std::string &file) {
        auto length = MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, nullptr, 0);
        std::wstring path(length > 0 ? length : 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, path.data(), length);
        std::vector<std::uint32_t> pixels;
        UINT width, height;
        if(!decode_image(path.c_str(), pixels, width, height)) {
            console_error(("buh: " + file + " couldn't be read").c_str());
            return false;
        }
        int size = static_cast<int>(height);
        if(size < SMALLEST_LUT || size > LARGEST_LUT || width != height * height) {
            console_error(("buh: " + file + " isn't a LUT strip: its width has to be its height squared (1024 by 32, say)").c_str());
            return false;
        }
        lut.assign(static_cast<std::size_t>(size) * size * size * 3, 0.0F);
        for(int blue = 0; blue < size; blue++) {
            for(int green = 0; green < size; green++) {
                for(int red = 0; red < size; red++) {
                    auto pixel = pixels[static_cast<std::size_t>(green) * width + blue * size + red];
                    float *entry = &lut[((blue * size + green) * size + red) * 3];
                    for(int c = 0; c < 3; c++) {
                        entry[c] = static_cast<float>((pixel >> ((2 - c) * 8)) & 0xFF) / 255.0F;
                    }
                }
            }
        }
        lut_size = size;
        lut_file = file;
        return true;
    }

    static void grading_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty() && !parse_bool(arguments[0], enabled)) {
            console_error("usage: buh_color_grading [true|false]");
            return;
        }
        console_print(enabled ? "buh_color_grading: true" : "buh_color_grading: false");
        if(enabled && !hud_drawing_available()) {
            console_error("buh: colour grading can't be drawn with this haloce.exe");
        }
        // Turned on again, it tries again
        if(enabled) {
            unavailable = false;
        }
    }

    static void lut_command(const std::vector<std::string> &arguments) {
        if(!arguments.empty()) {
            if(arguments[0] == "none") {
                lut.clear();
                lut_size = 0;
                lut_file.clear();
            }
            else if(!load_lut(arguments[0])) {
                return;
            }
            table_stale = true;
        }
        console_print(("buh_color_grading_lut: " + (lut_size > 0 ? lut_file : std::string("none"))).c_str());
    }

    static void adjustment(const char *name, const char *usage, float &value, float least, float most, const std::vector<std::string> &arguments) {
        post_setting_command(name, usage, value, least, most, arguments);
        table_stale = true;
    }

    static void strength_command(const std::vector<std::string> &arguments) {
        post_setting_command("buh_color_grading_strength", "usage: buh_color_grading_strength [amount], 0 to 1", strength, 0.0F, 1.0F, arguments);
    }

    static void temperature_command(const std::vector<std::string> &arguments) {
        adjustment("buh_color_grading_temperature", "usage: buh_color_grading_temperature [amount], -1 (cooler) to 1 (warmer)", temperature, -1.0F, 1.0F, arguments);
    }

    static void saturation_command(const std::vector<std::string> &arguments) {
        adjustment("buh_color_grading_saturation", "usage: buh_color_grading_saturation [amount], 0 (gray) to 2", saturation, 0.0F, 2.0F, arguments);
    }

    static void contrast_command(const std::vector<std::string> &arguments) {
        adjustment("buh_color_grading_contrast", "usage: buh_color_grading_contrast [amount], 0.5 to 2", contrast, 0.5F, 2.0F, arguments);
    }

    void set_up_grading() noexcept {
        add_world_draw_callback(draw_grading);
        add_console_command("buh_color_grading", "[true|false]", 0, 1, grading_command);
        add_console_command("buh_color_grading_lut", "[file|none]", 0, 1, lut_command);
        add_console_command("buh_color_grading_strength", "[amount]", 0, 1, strength_command);
        add_console_command("buh_color_grading_temperature", "[amount]", 0, 1, temperature_command);
        add_console_command("buh_color_grading_saturation", "[amount]", 0, 1, saturation_command);
        add_console_command("buh_color_grading_contrast", "[amount]", 0, 1, contrast_command);
    }
}
