// SPDX-License-Identifier: GPL-3.0-only

#ifndef BUH_SPRITES_SHAPES_HPP
#define BUH_SPRITES_SHAPES_HPP

#include <windows.h>
#include <d3d9.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Buh {
    struct ShapeColor {
        float red = 1.0F;
        float green = 1.0F;
        float blue = 1.0F;
        float alpha = 1.0F;
    };

    /**
     * A shape on the HUD. Its points are fractions of the view, relative to its first point, which is where it's
     * placed; lengths that have to stay round (a circle's radius, a rectangle's corners) are fractions of the view's
     * height, and thicknesses are pixels.
     */
    struct Shape {
        enum Kind { RECTANGLE, LINE, QUAD, CIRCLE } kind = RECTANGLE;

        // A rectangle's width and height, a line's other end, a quad's other three corners, or a circle's radius
        float points[6] = {};

        ShapeColor color;
        ShapeColor gradient;
        bool has_gradient = false;
        float thickness = 2.0F;

        // The thickness is in 480ths of the view's height (the HUD's units) rather than pixels
        bool thickness_scaled = false;

        bool outline = false;
        float corner_radius = 0.0F;

        // Degrees clockwise from the top, for a circle that's only an arc (or with no outline, a slice)
        float arc_from = 0.0F;
        float arc_to = 360.0F;
    };

    /**
     * A shape in the world, in world units
     */
    struct WorldShape {
        std::string name;
        enum Kind { LINE, BOX, CIRCLE } kind = LINE;

        // A line's ends; a box's centre and size; a circle's centre, and its radius in b[0]
        float a[3] = {};
        float b[3] = {};

        ShapeColor color;
        ShapeColor gradient;
        bool has_gradient = false;
        float thickness = 2.0F;
        bool thickness_scaled = false;
        bool through_walls = false;
        bool fill = false;

        // An object it's placed relative to, and goes with
        std::uint32_t object = 0xFFFFFFFF;

        float seconds = 0.0F;
        float fade = 0.0F;
        float opacity = 1.0F;
        double age = 0.0;
    };

    /**
     * Read one of a shape's options (color, gradient, thickness, outline, round, arc) at arguments[at]
     * @return how many arguments it took, or 0 if it isn't one or its values are wrong
     */
    std::size_t parse_shape_option(const std::vector<std::string> &arguments, std::size_t at, Shape &shape) noexcept;

    /**
     * Set the states draw_shape needs, on top of set_image_states
     */
    void set_shape_states(IDirect3DDevice9 *device) noexcept;

    /**
     * Draw a shape with its first point at x, y (fractions of the viewport), turned that many degrees about its
     * middle, at that opacity
     */
    void draw_shape(IDirect3DDevice9 *device, const D3DVIEWPORT9 &viewport, const Shape &shape, float x, float y, float rotation, float opacity) noexcept;

    /**
     * Take away a shape in the world, or all of them
     */
    void remove_world_shape(const std::string &name) noexcept;
    void clear_world_shapes() noexcept;

    /**
     * A shape in the world for the next frame drawn only, as scripts draw: put up again each frame, drawn once the next
     * time the world is and then let go of, or sooner with clear_queued_world_shapes()
     */
    void queue_world_shape(const WorldShape &shape) noexcept;
    void clear_queued_world_shapes() noexcept;

    /**
     * Where a point in the world was on the screen when the world was last drawn, in the HUD's 640 by 480 (whatever
     * the screen's shape), with its depth from 0 at the near plane to 1 at the far one
     * @return false if it was behind the camera, or nothing's been drawn yet
     */
    bool world_to_screen(float x, float y, float z, float &screen_x, float &screen_y, float &depth) noexcept;

    void set_up_shapes() noexcept;
}

#endif
