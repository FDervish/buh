// SPDX-License-Identifier: GPL-3.0-only

#include "sprites.hpp"
#include "image.hpp"
#include "shapes.hpp"
#include "../console.hpp"
#include "../render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define DEFAULT_FPS 10.0F
#define MAXIMUM_ARGUMENTS 32

namespace Buh {
    struct Texture {
        IDirect3DTexture9 *texture;
        UINT width;
        UINT height;
    };

    // A sprite, or a shape: everything drawn with the HUD, in one order
    struct Sprite {
        std::string name;
        std::string file;
        const Texture *texture = nullptr;
        std::shared_ptr<const Shape> shape;

        // Fractions of the view, from its top left; a shape's first point
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

    // Where it is now, gliding if it's moving
    static void position_now(const Sprite &sprite, float &x, float &y) noexcept {
        x = sprite.x;
        y = sprite.y;
        if(sprite.move_seconds > 0.0F) {
            auto t = std::clamp(static_cast<float>(sprite.age) / sprite.move_seconds, 0.0F, 1.0F);
            t = t * t * (3.0F - 2.0F * t);
            x += (sprite.move_x - x) * t;
            y += (sprite.move_y - y) * t;
        }
    }

    // How opaque it is now, fading in and out if it does
    static float opacity_now(const Sprite &sprite) noexcept {
        float alpha = sprite.opacity;
        if(sprite.fade > 0.0F) {
            alpha *= std::min(static_cast<float>(sprite.age) / sprite.fade, 1.0F);
            if(sprite.seconds > 0.0F) {
                alpha *= std::min((sprite.seconds - static_cast<float>(sprite.age)) / sprite.fade, 1.0F);
            }
        }
        return std::clamp(alpha, 0.0F, 1.0F);
    }

    static void draw_sprite(IDirect3DDevice9 *device, const D3DVIEWPORT9 &viewport, const Sprite &sprite) noexcept {
        float x, y;
        position_now(sprite, x, y);
        float alpha = opacity_now(sprite);

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
        enum { NO_STATES, IMAGE_STATES, SHAPE_STATES } states = NO_STATES;
        for(auto sprite = sprites->begin(); sprite != sprites->end();) {
            if(sprite->shape) {
                if(states != SHAPE_STATES) {
                    set_image_states(device);
                    set_shape_states(device);
                    states = SHAPE_STATES;
                }
                float x, y;
                position_now(*sprite, x, y);
                draw_shape(device, viewport, *sprite->shape, x, y, sprite->rotation, opacity_now(*sprite));
            }
            else {
                if(!sprite->texture) {
                    sprite->texture = get_texture(device, sprite->file);
                    if(!sprite->texture) {
                        console_error(("buh: couldn't load " + sprite->file).c_str());
                        sprite = sprites->erase(sprite);
                        continue;
                    }
                }
                if(states != IMAGE_STATES) {
                    set_image_states(device);
                    states = IMAGE_STATES;
                }
                draw_sprite(device, viewport, *sprite);
            }
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

    // An option sprites and shapes both take, at arguments[at]: how many arguments it took, or 0 if it isn't one or
    // its values are wrong
    static std::size_t parse_common_option(const std::vector<std::string> &arguments, std::size_t at, Sprite &sprite) noexcept {
        auto &option = arguments[at];
        auto values = arguments.size() - at - 1;
        auto &next = arguments[std::min(at + 1, arguments.size() - 1)];
        if(option == "time" && values >= 1 && parse_float(next, sprite.seconds) && sprite.seconds > 0.0F) {
            return 2;
        }
        if(option == "fade" && values >= 1 && parse_float(next, sprite.fade) && sprite.fade >= 0.0F) {
            return 2;
        }
        if(option == "opacity" && values >= 1 && parse_float(next, sprite.opacity)) {
            return 2;
        }
        if(option == "rotate" && values >= 1 && parse_float(next, sprite.rotation)) {
            return 2;
        }
        if(option == "move" && values >= 3 && parse_float(arguments[at + 1], sprite.move_x) && parse_float(arguments[at + 2], sprite.move_y) && parse_float(arguments[at + 3], sprite.move_seconds) && sprite.move_seconds > 0.0F) {
            return 4;
        }
        return 0;
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
            else if(auto taken = parse_common_option(arguments, i, sprite)) {
                i += taken - 1;
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

    // The same name replaces a sprite or shape where it is in the drawing order, and anything in the world by it
    static void place(Sprite &&sprite) {
        remove_world_shape(sprite.name);
        auto existing = std::find_if(sprites->begin(), sprites->end(), [&sprite](const Sprite &other) { return other.name == sprite.name; });
        if(existing != sprites->end()) {
            *existing = std::move(sprite);
        }
        else {
            sprites->push_back(std::move(sprite));
        }
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
        place(std::move(sprite));
    }

    // A shape command: its name, its first point, so many more numbers for the rest of it, then its options
    static bool shape_from(const std::vector<std::string> &arguments, std::size_t numbers, Shape &shape, Sprite &sprite) noexcept {
        sprite.name = arguments[0];
        if(arguments.size() < 3 + numbers || !parse_float(arguments[1], sprite.x) || !parse_float(arguments[2], sprite.y)) {
            return false;
        }
        for(std::size_t i = 0; i < numbers; i++) {
            if(!parse_float(arguments[3 + i], shape.points[i])) {
                return false;
            }
        }
        for(std::size_t i = 3 + numbers; i < arguments.size(); i++) {
            auto taken = parse_shape_option(arguments, i, shape);
            if(!taken) {
                taken = parse_common_option(arguments, i, sprite);
            }
            if(!taken) {
                return false;
            }
            i += taken - 1;
        }
        return true;
    }

    // Points after the first, relative to it
    static void relative(Shape &shape, const Sprite &sprite, std::size_t count) noexcept {
        for(std::size_t i = 0; i < count; i++) {
            shape.points[i] -= i % 2 == 0 ? sprite.x : sprite.y;
        }
    }

    #define SHAPE_OPTIONS "[color <r> <g> <b>|#rrggbb] [gradient <color>] [opacity <opacity>] [time <seconds>] [fade <seconds>] [move <x> <y> <seconds>] [rotate <degrees>]"

    static void rect_command(const std::vector<std::string> &arguments) {
        Sprite sprite;
        Shape shape;
        shape.kind = Shape::RECTANGLE;
        if(!shape_from(arguments, 2, shape, sprite) || shape.points[0] <= 0.0F || shape.points[1] <= 0.0F) {
            console_error("usage: buh_rect <name> <x> <y> <width> <height> [outline [thickness]] [round <radius>] " SHAPE_OPTIONS);
            return;
        }
        sprite.shape = std::make_shared<const Shape>(shape);
        place(std::move(sprite));
    }

    static void line_command(const std::vector<std::string> &arguments) {
        Sprite sprite;
        Shape shape;
        shape.kind = Shape::LINE;
        if(!shape_from(arguments, 2, shape, sprite)) {
            console_error("usage: buh_line <name> <x1> <y1> <x2> <y2> [thickness <pixels>] " SHAPE_OPTIONS);
            return;
        }
        relative(shape, sprite, 2);
        sprite.shape = std::make_shared<const Shape>(shape);
        place(std::move(sprite));
    }

    static void quad_command(const std::vector<std::string> &arguments) {
        Sprite sprite;
        Shape shape;
        shape.kind = Shape::QUAD;
        if(!shape_from(arguments, 6, shape, sprite)) {
            console_error("usage: buh_quad <name> <x1> <y1> <x2> <y2> <x3> <y3> <x4> <y4> [outline [thickness]] " SHAPE_OPTIONS);
            return;
        }
        relative(shape, sprite, 6);
        sprite.shape = std::make_shared<const Shape>(shape);
        place(std::move(sprite));
    }

    static void circle_command(const std::vector<std::string> &arguments) {
        Sprite sprite;
        Shape shape;
        shape.kind = Shape::CIRCLE;
        if(!shape_from(arguments, 1, shape, sprite) || shape.points[0] <= 0.0F) {
            console_error("usage: buh_circle <name> <x> <y> <radius> [outline [thickness]] [arc <from> <to>] " SHAPE_OPTIONS);
            return;
        }
        sprite.shape = std::make_shared<const Shape>(shape);
        place(std::move(sprite));
    }

    void remove_overlay(const std::string &name) noexcept {
        sprites->erase(std::remove_if(sprites->begin(), sprites->end(), [&name](const Sprite &sprite) { return sprite.name == name; }), sprites->end());
    }

    static void remove_command(const std::vector<std::string> &arguments) {
        remove_overlay(arguments[0]);
        remove_world_shape(arguments[0]);
    }

    static void clear_command(const std::vector<std::string> &) {
        sprites->clear();
        clear_world_shapes();
        release_textures();
    }

    void set_up_sprites() noexcept {
        if(!hud_drawing_available()) {
            return;
        }
        add_hud_draw_callback(draw);
        add_reset_callback(reset);
        add_console_command("buh_sprite", "<name> <file> <x> <y> <width> <height> [options]", 6, MAXIMUM_ARGUMENTS, sprite_command);
        add_console_command("buh_rect", "<name> <x> <y> <width> <height> [options]", 5, MAXIMUM_ARGUMENTS, rect_command);
        add_console_command("buh_line", "<name> <x1> <y1> <x2> <y2> [options]", 5, MAXIMUM_ARGUMENTS, line_command);
        add_console_command("buh_quad", "<name> <x1> <y1> <x2> <y2> <x3> <y3> <x4> <y4> [options]", 9, MAXIMUM_ARGUMENTS, quad_command);
        add_console_command("buh_circle", "<name> <x> <y> <radius> [options]", 4, MAXIMUM_ARGUMENTS, circle_command);
        add_console_command("buh_sprite_remove", "<name>", 1, 1, remove_command);
        add_console_command("buh_sprite_clear", "", 0, 0, clear_command);
    }
}
