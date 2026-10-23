// SPDX-License-Identifier: GPL-3.0-only
//
// Shapes: rectangles, lines, quads and circles on the HUD, drawn in the same order as sprites, and lines, boxes and
// circles in the world, drawn just before the HUD with the camera and depth buffer the world was drawn with.

#include "shapes.hpp"
#include "image.hpp"
#include "sprites.hpp"
#include "../console.hpp"
#include "../halo.hpp"
#include "../render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>

#define SHAPE_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define PI 3.14159265F

// How far edges fade out, in pixels, so diagonal edges and curves don't stair-step
#define FEATHER 1.0F

// A sharp corner's miter is held to this many times the line's width
#define MITER_LIMIT 4.0F

// The most segments a circle gets, and the fewest a whole one does
#define MAXIMUM_CIRCLE_SEGMENTS 256
#define MINIMUM_CIRCLE_SEGMENTS 24

// Pixels of circumference per segment
#define PIXELS_PER_SEGMENT 3.0F

// Shapes in the world are drawn this fraction of the way from the eye to where they are. It changes nothing on screen
// (a point's projection doesn't change along the line to the eye) but keeps a line lying on a wall in front of it.
#define DEPTH_PULL 0.9995F

#define MAXIMUM_ARGUMENTS 32

namespace Buh {
    struct ShapeVertex {
        float x, y, z, rhw;
        D3DCOLOR color;
    };

    // A point of a shape on the screen: pixels, depth and 1/w, and its colour
    struct ScreenPoint {
        float x, y, z, rhw;
        ShapeColor color;
    };

    static D3DCOLOR to_d3d(const ShapeColor &color, float opacity) noexcept {
        auto channel = [](float value) { return static_cast<D3DCOLOR>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F); };
        return channel(color.alpha * opacity) << 24 | channel(color.red) << 16 | channel(color.green) << 8 | channel(color.blue);
    }

    static ShapeColor mix(const ShapeColor &a, const ShapeColor &b, float t) noexcept {
        return { a.red + (b.red - a.red) * t, a.green + (b.green - a.green) * t, a.blue + (b.blue - a.blue) * t, a.alpha + (b.alpha - a.alpha) * t };
    }

    static ShapeColor transparent(ShapeColor color) noexcept {
        color.alpha = 0.0F;
        return color;
    }

    // A thickness in pixels, from one in pixels or in 480ths of the view's height
    static float pixels(float thickness, bool scaled, float view_height) noexcept {
        return scaled ? thickness * view_height / 480.0F : thickness;
    }

    // Triangles, drawn in one go
    class Mesh {
    public:
        explicit Mesh(float opacity) noexcept : opacity(opacity) {}

        void triangle(const ScreenPoint &a, const ScreenPoint &b, const ScreenPoint &c) {
            for(auto *point : { &a, &b, &c }) {
                vertices.push_back({ point->x, point->y, point->z, point->rhw, to_d3d(point->color, opacity) });
            }
        }

        // Corners in order around it
        void quad(const ScreenPoint &a, const ScreenPoint &b, const ScreenPoint &c, const ScreenPoint &d) {
            triangle(a, b, c);
            triangle(a, c, d);
        }

        void draw(IDirect3DDevice9 *device) const noexcept {
            if(!vertices.empty()) {
                device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(vertices.size() / 3), vertices.data(), sizeof(ShapeVertex));
            }
        }

        bool empty() const noexcept {
            return vertices.empty();
        }

    private:
        float opacity;
        std::vector<ShapeVertex> vertices;
    };

    static ScreenPoint offset(const ScreenPoint &point, float dx, float dy, const ShapeColor &color) noexcept {
        return { point.x + dx, point.y + dy, point.z, point.rhw, color };
    }

    // Consecutive points that are the same (or closed loops that end where they start) make no edge
    static std::vector<ScreenPoint> without_repeats(const std::vector<ScreenPoint> &points, bool closed) {
        std::vector<ScreenPoint> out;
        for(auto &point : points) {
            if(out.empty() || std::hypot(point.x - out.back().x, point.y - out.back().y) > 0.01F) {
                out.push_back(point);
            }
        }
        if(closed && out.size() > 2 && std::hypot(out.front().x - out.back().x, out.front().y - out.back().y) <= 0.01F) {
            out.pop_back();
        }
        return out;
    }

    // The unit normal to the left of the edge from a to b
    static void edge_normal(const ScreenPoint &a, const ScreenPoint &b, float &nx, float &ny) noexcept {
        float dx = b.x - a.x, dy = b.y - a.y;
        float length = std::hypot(dx, dy);
        nx = length > 0.0F ? -dy / length : 0.0F;
        ny = length > 0.0F ? dx / length : 0.0F;
    }

    // Where a joint between two edges with these normals puts the edge's sides: along the bisector, far enough out
    // that each side stays parallel to its edge, but no further than the miter limit
    static void miter(float n1x, float n1y, float n2x, float n2y, float &mx, float &my) noexcept {
        float sx = n1x + n2x, sy = n1y + n2y;
        float length = std::hypot(sx, sy);
        if(length < 0.001F) {
            mx = n1x;
            my = n1y;
            return;
        }
        sx /= length;
        sy /= length;
        float scale = 1.0F / std::max(sx * n1x + sy * n1y, 1.0F / MITER_LIMIT);
        mx = sx * scale;
        my = sy * scale;
    }

    /**
     * A filled area: triangles fanned from center to each pair of neighbouring boundary points, which has to see every
     * one of them (true of anything convex, and of a slice of a circle from its centre). Unless crisp, its edge fades
     * out over a pixel straddling the boundary, so it covers just as much as it would sharp.
     */
    static void fill(Mesh &mesh, const ScreenPoint &center, const std::vector<ScreenPoint> &boundary, bool crisp) {
        auto points = without_repeats(boundary, true);
        auto count = points.size();
        if(count < 3) {
            return;
        }
        if(crisp) {
            for(std::size_t i = 0; i < count; i++) {
                mesh.triangle(center, points[i], points[(i + 1) % count]);
            }
            return;
        }

        // Which way round the boundary goes decides which side of each edge is outside
        float area = 0.0F;
        for(std::size_t i = 0; i < count; i++) {
            auto &a = points[i], &b = points[(i + 1) % count];
            area += a.x * b.y - b.x * a.y;
        }
        float outside = (area > 0.0F ? -1.0F : 1.0F) * FEATHER / 2.0F;

        std::vector<ScreenPoint> inner(count), faded(count);
        for(std::size_t i = 0; i < count; i++) {
            float n1x, n1y, n2x, n2y, mx, my;
            edge_normal(points[(i + count - 1) % count], points[i], n1x, n1y);
            edge_normal(points[i], points[(i + 1) % count], n2x, n2y);
            miter(n1x, n1y, n2x, n2y, mx, my);
            inner[i] = offset(points[i], -mx * outside, -my * outside, points[i].color);
            faded[i] = offset(points[i], mx * outside, my * outside, transparent(points[i].color));
        }
        for(std::size_t i = 0; i < count; i++) {
            auto next = (i + 1) % count;
            mesh.triangle(center, inner[i], inner[next]);
            mesh.quad(inner[i], inner[next], faded[next], faded[i]);
        }
    }

    /**
     * A line through the points, width pixels wide, with mitered corners (so a translucent one isn't darker where its
     * edges meet) and unless crisp, edges that fade out over a pixel straddling each side, so it covers just its width
     */
    static void stroke(Mesh &mesh, const std::vector<ScreenPoint> &line, bool closed, float width, bool crisp) {
        auto points = without_repeats(line, closed);
        auto count = points.size();
        if(count < 2) {
            return;
        }
        closed = closed && count > 2;

        // A line thinner than a pixel is drawn a pixel wide and fainter, rather than flickering in and out
        float strength = std::min(width, 1.0F);
        width = std::max(width, 1.0F);
        float half = crisp ? width / 2.0F : std::max(width - FEATHER, 0.0F) / 2.0F;
        float feather = crisp ? 0.0F : FEATHER;

        struct Sides {
            ScreenPoint outer_fade, outer, inner, inner_fade;
        };
        std::vector<Sides> sides(count);
        for(std::size_t i = 0; i < count; i++) {
            bool has_previous = closed || i > 0;
            bool has_next = closed || i + 1 < count;
            float n1x = 0.0F, n1y = 0.0F, n2x = 0.0F, n2y = 0.0F, mx, my;
            if(has_previous) {
                edge_normal(points[(i + count - 1) % count], points[i], n1x, n1y);
            }
            if(has_next) {
                edge_normal(points[i], points[(i + 1) % count], n2x, n2y);
            }
            if(!has_previous) {
                n1x = n2x;
                n1y = n2y;
            }
            if(!has_next) {
                n2x = n1x;
                n2y = n1y;
            }
            miter(n1x, n1y, n2x, n2y, mx, my);
            auto color = points[i].color;
            color.alpha *= strength;
            auto faded = transparent(color);
            sides[i].outer_fade = offset(points[i], mx * (half + feather), my * (half + feather), faded);
            sides[i].outer = offset(points[i], mx * half, my * half, color);
            sides[i].inner = offset(points[i], -mx * half, -my * half, color);
            sides[i].inner_fade = offset(points[i], -mx * (half + feather), -my * (half + feather), faded);
        }

        auto segments = closed ? count : count - 1;
        for(std::size_t i = 0; i < segments; i++) {
            auto &a = sides[i];
            auto &b = sides[(i + 1) % count];
            if(feather > 0.0F) {
                mesh.quad(a.outer_fade, b.outer_fade, b.outer, a.outer);
                mesh.quad(a.inner, b.inner, b.inner_fade, a.inner_fade);
            }
            mesh.quad(a.outer, b.outer, b.inner, a.inner);
        }
    }

    // ---------------------------------------------------------------- options

    static bool parse_color(const std::vector<std::string> &arguments, std::size_t at, ShapeColor &color, std::size_t &taken) noexcept {
        if(at >= arguments.size()) {
            return false;
        }
        auto &first = arguments[at];
        if(!first.empty() && first[0] == '#') {
            // #RRGGBB or #RRGGBBAA
            if(first.size() != 7 && first.size() != 9) {
                return false;
            }
            char *end = nullptr;
            auto value = std::strtoul(first.c_str() + 1, &end, 16);
            if(*end != '\0') {
                return false;
            }
            if(first.size() == 7) {
                value = value << 8 | 0xFF;
            }
            color = { (value >> 24 & 0xFF) / 255.0F, (value >> 16 & 0xFF) / 255.0F, (value >> 8 & 0xFF) / 255.0F, (value & 0xFF) / 255.0F };
            taken = 1;
            return true;
        }
        // Red, green and blue from 0 to 1
        ShapeColor parsed;
        if(at + 2 >= arguments.size() || !parse_float(arguments[at], parsed.red) || !parse_float(arguments[at + 1], parsed.green) || !parse_float(arguments[at + 2], parsed.blue)) {
            return false;
        }
        color = parsed;
        taken = 3;
        return true;
    }

    std::size_t parse_shape_option(const std::vector<std::string> &arguments, std::size_t at, Shape &shape) noexcept {
        auto &option = arguments[at];
        auto values = arguments.size() - at - 1;
        std::size_t taken = 0;
        float value;
        if((option == "color" || option == "colour") && parse_color(arguments, at + 1, shape.color, taken)) {
            return taken + 1;
        }
        if(option == "gradient" && parse_color(arguments, at + 1, shape.gradient, taken)) {
            shape.has_gradient = true;
            return taken + 1;
        }
        if(option == "thickness" && values >= 1 && parse_float(arguments[at + 1], value) && value > 0.0F) {
            shape.thickness = value;
            return 2;
        }
        if(option == "outline") {
            shape.outline = true;
            if(values >= 1 && parse_float(arguments[at + 1], value) && value > 0.0F) {
                shape.thickness = value;
                return 2;
            }
            return 1;
        }
        if(option == "round" && shape.kind == Shape::RECTANGLE && values >= 1 && parse_float(arguments[at + 1], value) && value >= 0.0F) {
            shape.corner_radius = value;
            return 2;
        }
        if(option == "arc" && shape.kind == Shape::CIRCLE && values >= 2 && parse_float(arguments[at + 1], shape.arc_from) && parse_float(arguments[at + 2], shape.arc_to)) {
            return 3;
        }
        return 0;
    }

    // ---------------------------------------------------------------- on the HUD

    void set_shape_states(IDirect3DDevice9 *device) noexcept {
        device->SetFVF(SHAPE_FVF);
        device->SetTexture(0, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    }

    // A point of a HUD shape, in pixels from its first point before it's turned, with its colour
    struct LocalPoint {
        float x, y;
        ShapeColor color;
    };

    // The arc's points, clockwise from the top, and the angle it covers
    static std::vector<LocalPoint> arc_points(float radius, float from, float to, const ShapeColor &start, const ShapeColor &end) {
        if(to < from) {
            std::swap(from, to);
        }
        float span = std::min(to - from, 360.0F);
        auto full = std::clamp(static_cast<int>(2.0F * PI * radius / PIXELS_PER_SEGMENT), MINIMUM_CIRCLE_SEGMENTS, MAXIMUM_CIRCLE_SEGMENTS);
        int segments = std::max(2, static_cast<int>(std::ceil(full * span / 360.0F)));
        std::vector<LocalPoint> points;
        for(int i = 0; i <= segments; i++) {
            float t = static_cast<float>(i) / segments;
            float radians = (from + span * t) * PI / 180.0F;
            points.push_back({ radius * std::sin(radians), -radius * std::cos(radians), mix(start, end, t) });
        }
        return points;
    }

    // A rectangle's outline, with its corners rounded if they are, and coloured from top to bottom
    static std::vector<LocalPoint> rectangle_points(float width, float height, float radius, const ShapeColor &top, const ShapeColor &bottom) {
        std::vector<LocalPoint> points;
        radius = std::clamp(radius, 0.0F, std::min(width, height) / 2.0F);
        auto at = [&](float x, float y) {
            points.push_back({ x, y, mix(top, bottom, height > 0.0F ? y / height : 0.0F) });
        };
        if(radius < 0.5F) {
            at(0.0F, 0.0F);
            at(width, 0.0F);
            at(width, height);
            at(0.0F, height);
            return points;
        }
        int segments = std::clamp(static_cast<int>(radius * PI / 2.0F / PIXELS_PER_SEGMENT), 3, 32);
        const float corners[4][3] = {
            { width - radius, radius, 0.0F },
            { width - radius, height - radius, 90.0F },
            { radius, height - radius, 180.0F },
            { radius, radius, 270.0F }
        };
        for(auto &corner : corners) {
            for(int i = 0; i <= segments; i++) {
                float radians = (corner[2] + 90.0F * i / segments) * PI / 180.0F;
                at(corner[0] + radius * std::sin(radians), corner[1] - radius * std::cos(radians));
            }
        }
        return points;
    }

    void draw_shape(IDirect3DDevice9 *device, const D3DVIEWPORT9 &viewport, const Shape &shape, float x, float y, float rotation, float opacity) noexcept {
        float width = static_cast<float>(viewport.Width), height = static_cast<float>(viewport.Height);
        auto start = shape.color, end = shape.has_gradient ? shape.gradient : shape.color;

        std::vector<LocalPoint> points;
        bool closed = true, filled = !shape.outline, slice = false;
        switch(shape.kind) {
            case Shape::RECTANGLE:
                points = rectangle_points(shape.points[0] * width, shape.points[1] * height, shape.corner_radius * height, start, end);
                break;
            case Shape::LINE:
                points = { { 0.0F, 0.0F, start }, { shape.points[0] * width, shape.points[1] * height, end } };
                closed = false;
                filled = false;
                break;
            case Shape::QUAD:
                points = { { 0.0F, 0.0F, start }, { shape.points[0] * width, shape.points[1] * height, start },
                           { shape.points[2] * width, shape.points[3] * height, end }, { shape.points[4] * width, shape.points[5] * height, end } };
                break;
            case Shape::CIRCLE: {
                float radius = shape.points[0] * height;
                bool whole = std::fabs(shape.arc_to - shape.arc_from) >= 360.0F;
                // Filled, it's centre to edge; outlined, along the arc
                points = filled ? arc_points(radius, shape.arc_from, shape.arc_to, end, end) : arc_points(radius, shape.arc_from, shape.arc_to, start, end);
                closed = whole;
                slice = filled && !whole;
                if(whole) {
                    points.pop_back();
                }
                break;
            }
        }

        // Turned about the middle of the shape's extent, then placed; whole pixels' edges are half a pixel off their
        // centres, which is where Direct3D puts pixels
        float min_x = 0.0F, max_x = 0.0F, min_y = 0.0F, max_y = 0.0F;
        for(auto &point : points) {
            min_x = std::min(min_x, point.x);
            max_x = std::max(max_x, point.x);
            min_y = std::min(min_y, point.y);
            max_y = std::max(max_y, point.y);
        }
        float pivot_x = shape.kind == Shape::CIRCLE ? 0.0F : (min_x + max_x) / 2.0F;
        float pivot_y = shape.kind == Shape::CIRCLE ? 0.0F : (min_y + max_y) / 2.0F;
        float radians = rotation * PI / 180.0F;
        float cosine = std::cos(radians), sine = std::sin(radians);
        float origin_x = viewport.X + x * width - 0.5F, origin_y = viewport.Y + y * height - 0.5F;
        auto place = [&](float px, float py, const ShapeColor &color) {
            float dx = px - pivot_x, dy = py - pivot_y;
            return ScreenPoint { origin_x + pivot_x + dx * cosine - dy * sine, origin_y + pivot_y + dx * sine + dy * cosine, 0.0F, 1.0F, color };
        };
        std::vector<ScreenPoint> screen;
        for(auto &point : points) {
            screen.push_back(place(point.x, point.y, point.color));
        }

        // A plain rectangle lines up with the pixels, so its edges are sharp rather than faded
        bool crisp = shape.kind == Shape::RECTANGLE && shape.corner_radius * height < 0.5F && std::fmod(std::fabs(rotation), 90.0F) == 0.0F;
        Mesh mesh(opacity);
        if(!filled) {
            stroke(mesh, screen, closed, pixels(shape.thickness, shape.thickness_scaled, height), crisp);
        }
        else if(shape.kind == Shape::CIRCLE) {
            auto center = place(0.0F, 0.0F, start);
            if(slice) {
                screen.insert(screen.begin(), center);
            }
            fill(mesh, center, screen, false);
        }
        else {
            float cx = 0.0F, cy = 0.0F;
            for(auto &point : points) {
                cx += point.x / points.size();
                cy += point.y / points.size();
            }
            fill(mesh, place(cx, cy, mix(start, end, 0.5F)), screen, crisp);
        }
        mesh.draw(device);
    }

    // ---------------------------------------------------------------- in the world

    // Never freed, so nothing touches Direct3D while Windows unloads DLLs
    static auto *world_shapes = new std::vector<WorldShape>;
    static auto *queued_world_shapes = new std::vector<WorldShape>;
    static IDirect3DStateBlock9 *state_block = nullptr;

    // Scripts can put up this many shapes in the world for a frame
    #define MAXIMUM_QUEUED_WORLD_SHAPES 16384

    struct Camera {
        WorldView view;
        D3DVIEWPORT9 viewport;

        // Points nearer than this are behind the near plane, a little in front of it so pulling them towards the eye
        // doesn't put them behind it
        float clip_z;
    };

    // The camera the world was last drawn with, kept for world_to_screen: Halo's own only lasts while it draws
    static struct {
        float world_to_view[13];
        float projection[16];
        float z_near;
        D3DVIEWPORT9 viewport;
        bool valid;
    } last_camera = {};

    using Point3 = std::array<float, 3>;

    // Halo's own point transform: scaled, then turned onto forward, left and up, then moved
    static Point3 to_view(const Camera &camera, const Point3 &world) noexcept {
        auto *m = camera.view.world_to_view;
        float x = world[0] * m[0], y = world[1] * m[0], z = world[2] * m[0];
        return {
            m[1] * x + m[4] * y + m[7] * z + m[10],
            m[2] * x + m[5] * y + m[8] * z + m[11],
            m[3] * x + m[6] * y + m[9] * z + m[12]
        };
    }

    static bool in_front(const Camera &camera, const Point3 &view) noexcept {
        return view[2] <= camera.clip_z;
    }

    // Where a point in view space, in front of the near plane, is on the screen: the projection (a row vector times
    // it), then the viewport. Drawn, it's pulled towards the eye.
    static ScreenPoint project(const Camera &camera, const Point3 &view, const ShapeColor &color, float pull = DEPTH_PULL) noexcept {
        float x = view[0] * pull, y = view[1] * pull, z = view[2] * pull;
        auto *p = camera.view.projection;
        float clip_x = x * p[0] + y * p[4] + z * p[8] + p[12];
        float clip_y = x * p[1] + y * p[5] + z * p[9] + p[13];
        float clip_z = x * p[2] + y * p[6] + z * p[10] + p[14];
        float clip_w = x * p[3] + y * p[7] + z * p[11] + p[15];
        float inverse = 1.0F / clip_w;
        auto &viewport = camera.viewport;
        return {
            viewport.X + (clip_x * inverse * 0.5F + 0.5F) * viewport.Width,
            viewport.Y + (0.5F - clip_y * inverse * 0.5F) * viewport.Height,
            std::clamp(clip_z * inverse, 0.0F, 1.0F),
            inverse,
            color
        };
    }

    // How far along from a to b a segment crosses the near plane
    static float near_plane_crossing(const Camera &camera, const Point3 &a, const Point3 &b) noexcept {
        return (camera.clip_z - a[2]) / (b[2] - a[2]);
    }

    static Point3 along(const Point3 &a, const Point3 &b, float t) noexcept {
        return { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t };
    }

    // A line in the world, whatever of it is in front of the camera
    static void world_line(Mesh &mesh, const Camera &camera, const Point3 &from, const Point3 &to, const ShapeColor &start, const ShapeColor &end, float thickness) {
        auto a = to_view(camera, from), b = to_view(camera, to);
        bool a_in = in_front(camera, a), b_in = in_front(camera, b);
        if(!a_in && !b_in) {
            return;
        }
        float t_a = 0.0F, t_b = 1.0F;
        if(!a_in || !b_in) {
            float t = near_plane_crossing(camera, a, b);
            auto crossing = along(a, b, t);
            (a_in ? b : a) = crossing;
            (a_in ? t_b : t_a) = t;
        }
        stroke(mesh, { project(camera, a, mix(start, end, t_a)), project(camera, b, mix(start, end, t_b)) }, false, thickness, false);
    }

    // A triangle in the world, cut to what of it is in front of the camera
    static void world_triangle(Mesh &mesh, const Camera &camera, const Point3 (&corners)[3], const ShapeColor (&colors)[3]) {
        struct Corner {
            Point3 view;
            ShapeColor color;
        };
        std::vector<Corner> kept;
        for(int i = 0; i < 3; i++) {
            Corner a = { to_view(camera, corners[i]), colors[i] };
            Corner b = { to_view(camera, corners[(i + 1) % 3]), colors[(i + 1) % 3] };
            bool a_in = in_front(camera, a.view), b_in = in_front(camera, b.view);
            if(a_in) {
                kept.push_back(a);
            }
            if(a_in != b_in) {
                float t = near_plane_crossing(camera, a.view, b.view);
                kept.push_back({ along(a.view, b.view, t), mix(a.color, b.color, t) });
            }
        }
        for(std::size_t i = 1; i + 1 < kept.size(); i++) {
            mesh.triangle(project(camera, kept[0].view, kept[0].color), project(camera, kept[i].view, kept[i].color), project(camera, kept[i + 1].view, kept[i + 1].color));
        }
    }

    static void draw_world_shape(Mesh &mesh, const Camera &camera, const WorldShape &shape, const Point3 &origin) {
        auto start = shape.color, end = shape.has_gradient ? shape.gradient : shape.color;
        auto point = [&origin](float x, float y, float z) {
            return Point3 { origin[0] + x, origin[1] + y, origin[2] + z };
        };
        float thickness = pixels(shape.thickness, shape.thickness_scaled, static_cast<float>(camera.viewport.Height));
        auto line = [&](const Point3 &p, const Point3 &q, const ShapeColor &c1, const ShapeColor &c2) {
            world_line(mesh, camera, p, q, c1, c2, thickness);
        };

        switch(shape.kind) {
            case WorldShape::LINE:
                line(point(shape.a[0], shape.a[1], shape.a[2]), point(shape.b[0], shape.b[1], shape.b[2]), start, end);
                break;

            case WorldShape::BOX: {
                float hx = shape.b[0] / 2.0F, hy = shape.b[1] / 2.0F, hz = shape.b[2] / 2.0F;
                Point3 corners[8];
                for(int i = 0; i < 8; i++) {
                    corners[i] = point(shape.a[0] + (i & 1 ? hx : -hx), shape.a[1] + (i & 2 ? hy : -hy), shape.a[2] + (i & 4 ? hz : -hz));
                }
                // The bottom's colour to the top's
                for(int i = 0; i < 8; i++) {
                    for(int bit : { 1, 2, 4 }) {
                        if(!(i & bit)) {
                            line(corners[i], corners[i | bit], i & 4 ? end : start, (i | bit) & 4 ? end : start);
                        }
                    }
                }
                break;
            }

            case WorldShape::CIRCLE: {
                float radius = shape.b[0];
                int segments = MINIMUM_CIRCLE_SEGMENTS * 2;
                std::vector<Point3> rim;
                for(int i = 0; i < segments; i++) {
                    float radians = 2.0F * PI * i / segments;
                    rim.push_back(point(shape.a[0] + radius * std::cos(radians), shape.a[1] + radius * std::sin(radians), shape.a[2]));
                }
                if(shape.fill) {
                    // Centre to edge
                    auto center = point(shape.a[0], shape.a[1], shape.a[2]);
                    for(int i = 0; i < segments; i++) {
                        const Point3 corners[3] = { center, rim[i], rim[(i + 1) % segments] };
                        const ShapeColor colors[3] = { start, end, end };
                        world_triangle(mesh, camera, corners, colors);
                    }
                    break;
                }
                // All of it in front: one line round, joined at every point
                std::vector<ScreenPoint> screen;
                bool all_in_front = true;
                for(int i = 0; i < segments && all_in_front; i++) {
                    auto view = to_view(camera, rim[i]);
                    all_in_front = in_front(camera, view);
                    screen.push_back(project(camera, view, mix(start, end, static_cast<float>(i) / segments)));
                }
                if(all_in_front) {
                    stroke(mesh, screen, true, thickness, false);
                }
                else {
                    for(int i = 0; i < segments; i++) {
                        line(rim[i], rim[(i + 1) % segments], mix(start, end, static_cast<float>(i) / segments), mix(start, end, static_cast<float>(i + 1) / segments));
                    }
                }
                break;
            }
        }
    }

    static float shape_opacity(const WorldShape &shape) noexcept {
        float opacity = shape.opacity;
        if(shape.fade > 0.0F) {
            opacity *= std::min(static_cast<float>(shape.age) / shape.fade, 1.0F);
            if(shape.seconds > 0.0F) {
                opacity *= std::min((shape.seconds - static_cast<float>(shape.age)) / shape.fade, 1.0F);
            }
        }
        return std::clamp(opacity, 0.0F, 1.0F);
    }

    static void draw_world(IDirect3DDevice9 *device, double seconds) noexcept {
        Camera camera;
        bool can_draw = world_view(camera.view) && device->GetViewport(&camera.viewport) >= 0;
        if(can_draw) {
            std::copy(camera.view.world_to_view, camera.view.world_to_view + 13, last_camera.world_to_view);
            std::copy(camera.view.projection, camera.view.projection + 16, last_camera.projection);
            last_camera.z_near = camera.view.z_near;
            last_camera.viewport = camera.viewport;
            last_camera.valid = true;
        }
        if(world_shapes->empty() && queued_world_shapes->empty()) {
            return;
        }

        if(can_draw && (state_block || device->CreateStateBlock(D3DSBT_ALL, &state_block) >= 0)) {
            camera.clip_z = -camera.view.z_near / DEPTH_PULL * 1.001F;

            // Tested against the depth buffer the way Halo's own geometry is (it may use w rather than z), unless
            // there's no depth buffer to test against
            DWORD depth_mode = D3DZB_TRUE;
            device->GetRenderState(D3DRS_ZENABLE, &depth_mode);
            IDirect3DSurface9 *depth_surface = nullptr;
            bool has_depth = device->GetDepthStencilSurface(&depth_surface) >= 0 && depth_surface;
            if(depth_surface) {
                depth_surface->Release();
            }
            if(depth_mode == D3DZB_FALSE) {
                depth_mode = D3DZB_TRUE;
            }

            state_block->Capture();
            set_image_states(device);
            set_shape_states(device);
            device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            for(auto *list : { world_shapes, queued_world_shapes }) {
                for(auto &shape : *list) {
                    float origin[3] = {};
                    if(shape.object != 0xFFFFFFFF && !object_position(shape.object, origin)) {
                        continue;
                    }
                    Mesh mesh(shape_opacity(shape));
                    draw_world_shape(mesh, camera, shape, { origin[0], origin[1], origin[2] });
                    device->SetRenderState(D3DRS_ZENABLE, shape.through_walls || !has_depth ? static_cast<DWORD>(D3DZB_FALSE) : depth_mode);
                    mesh.draw(device);
                }
            }
            state_block->Apply();
        }

        // Shapes on objects that have gone go with them
        for(auto shape = world_shapes->begin(); shape != world_shapes->end();) {
            float origin[3];
            shape->age += seconds;
            bool gone = shape->object != 0xFFFFFFFF && !object_position(shape->object, origin);
            shape = gone || (shape->seconds > 0.0F && shape->age >= shape->seconds) ? world_shapes->erase(shape) : std::next(shape);
        }
    }

    static void reset() noexcept {
        if(state_block) {
            state_block->Release();
            state_block = nullptr;
        }
    }

    void remove_world_shape(const std::string &name) noexcept {
        world_shapes->erase(std::remove_if(world_shapes->begin(), world_shapes->end(), [&name](const WorldShape &shape) { return shape.name == name; }), world_shapes->end());
    }

    void clear_world_shapes() noexcept {
        world_shapes->clear();
    }

    void queue_world_shape(const WorldShape &shape) noexcept {
        if(queued_world_shapes->size() < MAXIMUM_QUEUED_WORLD_SHAPES) {
            queued_world_shapes->push_back(shape);
        }
    }

    void clear_queued_world_shapes() noexcept {
        queued_world_shapes->clear();
    }

    bool world_to_screen(float x, float y, float z, float &screen_x, float &screen_y, float &depth) noexcept {
        if(!last_camera.valid) {
            return false;
        }
        Camera camera;
        camera.view = { last_camera.world_to_view, last_camera.projection, last_camera.z_near };
        camera.viewport = last_camera.viewport;
        camera.clip_z = -last_camera.z_near;
        auto view = to_view(camera, { x, y, z });
        if(!in_front(camera, view)) {
            return false;
        }
        auto point = project(camera, view, {}, 1.0F);
        auto &viewport = camera.viewport;
        screen_x = (point.x - viewport.X) / viewport.Width * 640.0F;
        screen_y = (point.y - viewport.Y) / viewport.Height * 480.0F;
        depth = point.z;
        return true;
    }

    // Options after a world shape's coordinates
    static bool parse_world_options(const std::vector<std::string> &arguments, std::size_t first, WorldShape &shape) noexcept {
        for(std::size_t i = first; i < arguments.size(); i++) {
            auto &option = arguments[i];
            auto values = arguments.size() - i - 1;
            auto &next = arguments[std::min(i + 1, arguments.size() - 1)];
            std::size_t taken = 0;
            float value;
            if((option == "color" || option == "colour") && parse_color(arguments, i + 1, shape.color, taken)) {
                i += taken;
            }
            else if(option == "gradient" && parse_color(arguments, i + 1, shape.gradient, taken)) {
                shape.has_gradient = true;
                i += taken;
            }
            else if(option == "thickness" && values >= 1 && parse_float(next, value) && value > 0.0F) {
                shape.thickness = value;
                i++;
            }
            else if(option == "through_walls") {
                shape.through_walls = true;
            }
            else if(option == "fill" && shape.kind == WorldShape::CIRCLE) {
                shape.fill = true;
            }
            else if(option == "on" && values >= 1) {
                char *end = nullptr;
                auto object = std::strtoul(next.c_str(), &end, 0);
                if(next.empty() || *end != '\0') {
                    return false;
                }
                shape.object = static_cast<std::uint32_t>(object);
                i++;
            }
            else if(option == "time" && values >= 1 && parse_float(next, shape.seconds) && shape.seconds > 0.0F) {
                i++;
            }
            else if(option == "fade" && values >= 1 && parse_float(next, shape.fade) && shape.fade >= 0.0F) {
                i++;
            }
            else if(option == "opacity" && values >= 1 && parse_float(next, shape.opacity)) {
                i++;
            }
            else {
                return false;
            }
        }
        return true;
    }

    static bool parse_numbers(const std::vector<std::string> &arguments, std::size_t first, std::size_t count, float *out) noexcept {
        for(std::size_t i = 0; i < count; i++) {
            if(first + i >= arguments.size() || !parse_float(arguments[first + i], out[i])) {
                return false;
            }
        }
        return true;
    }

    static void place_world_shape(WorldShape &&shape) {
        if(!world_drawing_available()) {
            console_error("buh: shapes in the world can't be drawn with this haloce.exe");
            return;
        }
        // A name is one thing, on the HUD or in the world
        remove_overlay(shape.name);
        auto existing = std::find_if(world_shapes->begin(), world_shapes->end(), [&shape](const WorldShape &other) { return other.name == shape.name; });
        if(existing != world_shapes->end()) {
            *existing = std::move(shape);
        }
        else {
            world_shapes->push_back(std::move(shape));
        }
    }

    #define WORLD_OPTIONS "[color <r> <g> <b>|#rrggbb] [gradient <color>] [thickness <pixels>] [through_walls] [on <object>] [time <seconds>] [fade <seconds>] [opacity <opacity>]"

    static void line_3d_command(const std::vector<std::string> &arguments) {
        WorldShape shape;
        shape.name = arguments[0];
        shape.kind = WorldShape::LINE;
        if(!parse_numbers(arguments, 1, 3, shape.a) || !parse_numbers(arguments, 4, 3, shape.b) || !parse_world_options(arguments, 7, shape)) {
            console_error("usage: buh_line_3d <name> <x1> <y1> <z1> <x2> <y2> <z2> " WORLD_OPTIONS);
            return;
        }
        place_world_shape(std::move(shape));
    }

    static void box_3d_command(const std::vector<std::string> &arguments) {
        WorldShape shape;
        shape.name = arguments[0];
        shape.kind = WorldShape::BOX;
        if(!parse_numbers(arguments, 1, 3, shape.a) || !parse_numbers(arguments, 4, 3, shape.b) || shape.b[0] < 0.0F || shape.b[1] < 0.0F || shape.b[2] < 0.0F
        || !parse_world_options(arguments, 7, shape)) {
            console_error("usage: buh_box_3d <name> <x> <y> <z> <width> <length> <height> " WORLD_OPTIONS);
            return;
        }
        place_world_shape(std::move(shape));
    }

    static void circle_3d_command(const std::vector<std::string> &arguments) {
        WorldShape shape;
        shape.name = arguments[0];
        shape.kind = WorldShape::CIRCLE;
        if(!parse_numbers(arguments, 1, 3, shape.a) || !parse_numbers(arguments, 4, 1, shape.b) || shape.b[0] <= 0.0F || !parse_world_options(arguments, 5, shape)) {
            console_error("usage: buh_circle_3d <name> <x> <y> <z> <radius> [fill] " WORLD_OPTIONS);
            return;
        }
        place_world_shape(std::move(shape));
    }

    void set_up_shapes() noexcept {
        if(!hud_drawing_available()) {
            return;
        }
        add_world_draw_callback(draw_world);
        add_reset_callback(reset);
        add_console_command("buh_line_3d", "<name> <x1> <y1> <z1> <x2> <y2> <z2> [options]", 7, MAXIMUM_ARGUMENTS, line_3d_command);
        add_console_command("buh_box_3d", "<name> <x> <y> <z> <width> <length> <height> [options]", 7, MAXIMUM_ARGUMENTS, box_3d_command);
        add_console_command("buh_circle_3d", "<name> <x> <y> <z> <radius> [options]", 5, MAXIMUM_ARGUMENTS, circle_3d_command);
    }
}
