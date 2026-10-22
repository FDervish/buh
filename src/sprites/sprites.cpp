// SPDX-License-Identifier: GPL-3.0-only
//
// The idea comes from Harmony's Optic sprites.

#include "sprites.hpp"
#include "image.hpp"
#include "../console.hpp"
#include "../render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#define DEFAULT_FPS 10.0F
#define MAXIMUM_ARGUMENTS 24

namespace Buh {
    struct Texture {
        IDirect3DTexture9 *texture;
        UINT width;
        UINT height;
    };

    struct Sprite {
        std::string name;
        std::string file;
        const Texture *texture = nullptr;

        // Fractions of the view, from its top left
        float x = 0.0F;
        float y = 0.0F;
        float width = 0.0F;
        float height = 0.0F;
        float move_x = 0.0F;
        float move_y = 0.0F;
        float move_seconds = 0.0F;

        std::uint32_t columns = 1;
        std::uint32_t rows = 1;
        std::uint32_t frames = 1;
        float fps = DEFAULT_FPS;
        bool once = false;

        float seconds = 0.0F;
        float fade = 0.0F;
        float opacity = 1.0F;
        float rotation = 0.0F;
        double age = 0.0;
    };

    // Never freed, so nothing touches Direct3D while Windows unloads DLLs
    static auto *textures = new std::map<std::string, Texture>;
    static auto *sprites = new std::vector<Sprite>;
    static IDirect3DDevice9 *textures_device = nullptr;
    static IDirect3DStateBlock9 *state_block = nullptr;

    static void release_textures() noexcept {
        for(auto &texture : *textures) {
            texture.second.texture->Release();
        }
        textures->clear();
        for(auto &sprite : *sprites) {
            sprite.texture = nullptr;
        }
    }

    static const Texture *get_texture(IDirect3DDevice9 *device, const std::string &file) noexcept {
        auto found = textures->find(file);
        if(found != textures->end()) {
            return &found->second;
        }
        auto length = MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, nullptr, 0);
        std::wstring path(length > 0 ? length : 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, path.data(), length);
        Texture texture;
        texture.texture = load_image(device, path.c_str(), texture.width, texture.height);
        if(!texture.texture) {
            return nullptr;
        }
        return &((*textures)[file] = texture);
    }

    static void draw_sprite(IDirect3DDevice9 *device, const D3DVIEWPORT9 &viewport, const Sprite &sprite) noexcept {
        float x = sprite.x;
        float y = sprite.y;
        if(sprite.move_seconds > 0.0F) {
            auto t = std::clamp(static_cast<float>(sprite.age) / sprite.move_seconds, 0.0F, 1.0F);
            t = t * t * (3.0F - 2.0F * t);
            x += (sprite.move_x - x) * t;
            y += (sprite.move_y - y) * t;
        }

        float alpha = sprite.opacity;
        if(sprite.fade > 0.0F) {
            alpha *= std::min(static_cast<float>(sprite.age) / sprite.fade, 1.0F);
            if(sprite.seconds > 0.0F) {
                alpha *= std::min((sprite.seconds - static_cast<float>(sprite.age)) / sprite.fade, 1.0F);
            }
        }
        alpha = std::clamp(alpha, 0.0F, 1.0F);

        auto frame = static_cast<std::uint32_t>(sprite.age * sprite.fps);
        frame = sprite.once ? std::min(frame, sprite.frames - 1) : frame % sprite.frames;
        // Half a texel in from the frame's edges, so filtering doesn't bleed the next frame in
        float inset_u = 0.5F / sprite.texture->width;
        float inset_v = 0.5F / sprite.texture->height;
        float u0 = static_cast<float>(frame % sprite.columns) / sprite.columns + inset_u;
        float v0 = static_cast<float>(frame / sprite.columns) / sprite.rows + inset_v;
        float u1 = u0 + 1.0F / sprite.columns - 2.0F * inset_u;
        float v1 = v0 + 1.0F / sprite.rows - 2.0F * inset_v;

        // Turned about its centre; the half pixel lines texels up with pixels
        float half_width = sprite.width * viewport.Width / 2.0F;
        float half_height = sprite.height * viewport.Height / 2.0F;
        float center_x = viewport.X + x * viewport.Width + half_width - 0.5F;
        float center_y = viewport.Y + y * viewport.Height + half_height - 0.5F;
        float radians = sprite.rotation * 3.14159265F / 180.0F;
        float cosine = std::cos(radians);
        float sine = std::sin(radians);
        auto corner = [&](float dx, float dy, float u, float v) {
            return ImageCorner { center_x + dx * cosine - dy * sine, center_y + dx * sine + dy * cosine, u, v };
        };
        const ImageCorner corners[4] = {
            corner(-half_width, -half_height, u0, v0),
            corner(half_width, -half_height, u1, v0),
            corner(half_width, half_height, u1, v1),
            corner(-half_width, half_height, u0, v1)
        };
        draw_image(device, sprite.texture->texture, corners, alpha);
    }

    static void draw(IDirect3DDevice9 *device, double seconds) noexcept {
        if(sprites->empty()) {
            return;
        }
        if(device != textures_device) {
            release_textures();
            textures_device = device;
        }
        if(!state_block && device->CreateStateBlock(D3DSBT_ALL, &state_block) < 0) {
            state_block = nullptr;
            return;
        }
        D3DVIEWPORT9 viewport;
        device->GetViewport(&viewport);
        state_block->Capture();
        set_image_states(device);
        for(auto sprite = sprites->begin(); sprite != sprites->end();) {
            if(!sprite->texture) {
                sprite->texture = get_texture(device, sprite->file);
                if(!sprite->texture) {
                    console_error(("buh: couldn't load " + sprite->file).c_str());
                    sprite = sprites->erase(sprite);
                    continue;
                }
            }
            draw_sprite(device, viewport, *sprite);
            sprite->age += seconds;
            sprite = sprite->seconds > 0.0F && sprite->age >= sprite->seconds ? sprites->erase(sprite) : std::next(sprite);
        }
        state_block->Apply();
    }

    static void reset() noexcept {
        if(state_block) {
            state_block->Release();
            state_block = nullptr;
        }
    }

    static bool parse_count(const std::string &text, std::uint32_t &value) noexcept {
        float number;
        if(!parse_float(text, number) || number < 1.0F || number > 4096.0F || number != std::floor(number)) {
            return false;
        }
        value = static_cast<std::uint32_t>(number);
        return true;
    }

    // Options after the position and size, as keywords followed by their values
    static bool parse_options(const std::vector<std::string> &arguments, Sprite &sprite) noexcept {
        bool frames_given = false;
        for(std::size_t i = 6; i < arguments.size(); i++) {
            auto &option = arguments[i];
            auto values = arguments.size() - i - 1;
            auto &next = arguments[std::min(i + 1, arguments.size() - 1)];
            if(option == "once") {
                sprite.once = true;
            }
            else if(option == "sheet" && values >= 2 && parse_count(arguments[i + 1], sprite.columns) && parse_count(arguments[i + 2], sprite.rows)) {
                i += 2;
                if(i + 1 < arguments.size() && parse_count(arguments[i + 1], sprite.frames)) {
                    frames_given = true;
                    i++;
                }
            }
            else if(option == "fps" && values >= 1 && parse_float(next, sprite.fps) && sprite.fps > 0.0F) {
                i++;
            }
            else if(option == "time" && values >= 1 && parse_float(next, sprite.seconds) && sprite.seconds > 0.0F) {
                i++;
            }
            else if(option == "fade" && values >= 1 && parse_float(next, sprite.fade) && sprite.fade >= 0.0F) {
                i++;
            }
            else if(option == "opacity" && values >= 1 && parse_float(next, sprite.opacity)) {
                i++;
            }
            else if(option == "rotate" && values >= 1 && parse_float(next, sprite.rotation)) {
                i++;
            }
            else if(option == "move" && values >= 3 && parse_float(arguments[i + 1], sprite.move_x) && parse_float(arguments[i + 2], sprite.move_y) && parse_float(arguments[i + 3], sprite.move_seconds) && sprite.move_seconds > 0.0F) {
                i += 3;
            }
            else {
                return false;
            }
        }
        if(!frames_given) {
            sprite.frames = sprite.columns * sprite.rows;
        }
        return sprite.frames <= sprite.columns * sprite.rows;
    }

    static void sprite_command(const std::vector<std::string> &arguments) {
        Sprite sprite;
        sprite.name = arguments[0];
        sprite.file = arguments[1];
        if(!parse_float(arguments[2], sprite.x) || !parse_float(arguments[3], sprite.y) || !parse_float(arguments[4], sprite.width) || !parse_float(arguments[5], sprite.height)
        || sprite.width <= 0.0F || sprite.height <= 0.0F || !parse_options(arguments, sprite)) {
            console_error("usage: buh_sprite <name> <file> <x> <y> <width> <height> [sheet <columns> <rows> [frames]] [fps <fps>] [once] [time <seconds>] [fade <seconds>] [opacity <opacity>] [rotate <degrees>] [move <x> <y> <seconds>]");
            return;
        }
        // The same name replaces a sprite where it is in the drawing order
        auto existing = std::find_if(sprites->begin(), sprites->end(), [&sprite](const Sprite &other) { return other.name == sprite.name; });
        if(existing != sprites->end()) {
            *existing = sprite;
        }
        else {
            sprites->push_back(sprite);
        }
    }

    static void remove_command(const std::vector<std::string> &arguments) {
        sprites->erase(std::remove_if(sprites->begin(), sprites->end(), [&arguments](const Sprite &sprite) { return sprite.name == arguments[0]; }), sprites->end());
    }

    static void clear_command(const std::vector<std::string> &) {
        sprites->clear();
        release_textures();
    }

    void set_up_sprites() noexcept {
        if(!hud_drawing_available()) {
            return;
        }
        add_hud_draw_callback(draw);
        add_reset_callback(reset);
        add_console_command("buh_sprite", "<name> <file> <x> <y> <width> <height> [options]", 6, MAXIMUM_ARGUMENTS, sprite_command);
        add_console_command("buh_sprite_remove", "<name>", 1, 1, remove_command);
        add_console_command("buh_sprite_clear", "", 0, 0, clear_command);
    }
}
