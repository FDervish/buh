// SPDX-License-Identifier: GPL-3.0-only
//
// Motion blur (Morgan McGuire et al., "A Reconstruction Filter for Plausible Motion Blur", 2012). How far each pixel
// moved since the last frame goes into a motion buffer, in pixels: the camera's motion first, each pixel put back
// through the last frame's camera from the world's depth, then each model Halo drew over that with its own motion, its
// vertices skinned with this frame's bones and the last's. The largest motion is found in tiles as wide as the most
// blur, then the largest of each tile's neighbours, and each pixel is blurred along its neighbourhood's, each sample
// weighed by which of the two is in front and how far each one's own motion reaches.
//
// tools/compile_shader.py compiles this into motion_blur_shaders.h.

#include "post_depth.hlsl"

// CameraMotionPS: the world's motion. TileMaxPS: the motion or the last tile pass. NeighbourMaxPS: the tiles.
// GatherPS and MotionViewPS: the world.
sampler2D source : register(s0);

// GatherPS and MotionViewPS: the motion
sampler2D motion : register(s1);

// GatherPS: the largest motion around each tile
sampler2D neighbours : register(s2);

// xy: one pixel of the world; zw: the world's width and height in pixels
float4 sizes : register(c0);

// CameraMotionPS: from this frame's view space to the last's, a 3x4 matrix, a row each
float4 reprojection[3] : register(c1);

// CameraMotionPS: the last frame's projection, x, y and w of a point each a column of it
float4 last_projection[3] : register(c7);

// x: how much of the motion to blur along, for the shutter; y: the most to blur, in pixels; z: how wide a tile is, in
// pixels. CameraMotionPS: w, 1 if there's a last frame to have moved from.
float4 blur : register(c10);

// TileMaxPS: xy, which way it goes (one of them 1); zw, the size of what it reads, in texels.
// GatherPS: xy, one texel of the tiles.
float4 tiles : register(c11);

#define SAMPLES 15

float noise(float2 position) {
    return frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
}

// A pixel's motion as it's blurred: scaled for the shutter, and no longer than the most
float2 scaled(float2 moved) {
    moved *= blur.x;
    float length_moved = length(moved);
    return length_moved > blur.y ? moved * (blur.y / length_moved) : moved;
}

// How far each pixel moved for the camera: put back where the last frame's camera saw it, from its depth (the sky
// from its direction alone). First person things move with the camera, so not at all.
float4 CameraMotionPS(float2 uv : TEXCOORD0) : COLOR0 {
    float depth = world_depth(uv);
    if(blur.w == 0.0 || is_first_person(uv)) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    float distance = distance_of(depth, false);
    float4 here = is_sky(depth) ? float4(view_direction(uv), 0.0) : float4(view_position(uv, distance), 1.0);
    float4 there = float4(dot(reprojection[0], here), dot(reprojection[1], here), dot(reprojection[2], here), here.w);
    float w = dot(there, last_projection[2]);
    if(w <= 0.0001) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    float2 last = float2(dot(there, last_projection[0]) / w * 0.5 + 0.5, 0.5 - dot(there, last_projection[1]) / w * 0.5);
    return float4((uv - last) * sizes.zw, 0.0, 1.0);
}

// The longest motion along a tile's width or height, scaled as it's blurred the first time
float4 TileMaxPS(float2 uv : TEXCOORD0, float2 position : VPOS) : COLOR0 {
    float2 first = position * (1.0 + (blur.z - 1.0) * tiles.xy);
    float2 longest = 0.0;
    float longest_length = 0.0;
    [loop]
    for(int i = 0; i < blur.z; i++) {
        float2 at = min(first + tiles.xy * i, tiles.zw - 1.0);
        float2 moved = tex2Dlod(source, float4((at + 0.5) / tiles.zw, 0.0, 0.0)).rg;
        moved = tiles.x > 0.0 ? scaled(moved) : moved;
        float moved_length = length(moved);
        if(moved_length > longest_length) {
            longest = moved;
            longest_length = moved_length;
        }
    }
    return float4(longest, 0.0, 1.0);
}

// The longest motion of a tile and the eight around it
float4 NeighbourMaxPS(float2 uv : TEXCOORD0) : COLOR0 {
    float2 longest = 0.0;
    float longest_length = 0.0;
    for(int y = -1; y <= 1; y++) {
        for(int x = -1; x <= 1; x++) {
            float2 moved = tex2Dlod(source, float4(uv + float2(x, y) * sizes.xy, 0.0, 0.0)).rg;
            float moved_length = length(moved);
            if(moved_length > longest_length) {
                longest = moved;
                longest_length = moved_length;
            }
        }
    }
    return float4(longest, 0.0, 1.0);
}

// How far along the view what's at a place is, for what's in front of what. First person things are drawn over the
// world wherever they are in it (up against a wall, the wall can be nearer than them), so they're in front of it all.
float in_front_distance(float2 uv) {
    return is_first_person(uv) ? 0.0 : distance_of(world_depth(uv), false);
}

// How much a nearer sample counts: all if a is nearer than b, fading out as it gets further behind
float nearer(float a, float b) {
    return saturate(1.0 - (a - b) / (0.05 + 0.01 * b));
}

// Whether something blurred that far reaches a sample this far away, fading toward the end (cone) or not (cylinder)
float cone(float away, float reach) {
    return saturate(1.0 - away / reach);
}

float cylinder(float away, float reach) {
    return 1.0 - smoothstep(0.95 * reach, 1.05 * reach, away);
}

// The middle of the pixel a place is in, where its motion and depth are
float2 pixel_middle(float2 uv) {
    return (floor(uv * sizes.zw) + 0.5) * sizes.xy;
}

float4 GatherPS(float2 uv : TEXCOORD0, float2 position : VPOS) : COLOR0 {
    float3 color = tex2Dlod(source, float4(uv, 0.0, 0.0)).rgb;
    float2 tile = (floor(position / blur.z) + 0.5) * tiles.xy;
    float2 around = tex2Dlod(neighbours, float4(tile, 0.0, 0.0)).rg;
    float around_length = length(around);
    if(around_length < 0.5) {
        return float4(color, 1.0);
    }

    float here_reach = max(length(scaled(tex2Dlod(motion, float4(uv, 0.0, 0.0)).rg)), 0.5);
    float here_distance = in_front_distance(uv);
    float weight = 1.0 / here_reach;
    float3 sum = color * weight;
    float jitter = noise(position) - 0.5;
    [loop]
    for(int i = 0; i < SAMPLES; i++) {
        if(i == SAMPLES / 2) {
            continue;
        }
        float t = lerp(-1.0, 1.0, (i + jitter + 1.0) / (SAMPLES + 1.0));
        float2 at = pixel_middle(uv + around * t * sizes.xy);
        float there_reach = max(length(scaled(tex2Dlod(motion, float4(at, 0.0, 0.0)).rg)), 0.5);
        float there_distance = in_front_distance(at);
        float away = abs(t) * around_length;

        // A sample in front blurred over this pixel, this pixel blurred over a sample behind it, or both blurred
        float a = nearer(there_distance, here_distance) * cone(away, there_reach)
            + nearer(here_distance, there_distance) * cone(away, here_reach)
            + cylinder(away, there_reach) * cylinder(away, here_reach) * 2.0;
        weight += a;
        sum += tex2Dlod(source, float4(at, 0.0, 0.0)).rgb * a;
    }
    return float4(sum / weight, 1.0);
}

// buh_motion_view: the motion in place of the world. Still is gray; moving right reddens a pixel and left takes red
// away, moving down greens it and up takes green away, more the nearer the most it moves; models drawn with their own
// motion are bluish.
float4 MotionViewPS(float2 uv : TEXCOORD0) : COLOR0 {
    float3 moved = tex2Dlod(motion, float4(uv, 0.0, 0.0)).rgb;
    float2 shown = clamp(moved.xy / max(blur.y, 1.0), -1.0, 1.0) * 0.5 + 0.5;
    return float4(shown, moved.z > 0.5 ? 0.85 : 0.5, 1.0);
}

// The models: each vertex skinned twice, with this frame's bones and the last's, from a vertex texture of them: a row
// for each part drawn, this frame's bones from its start and the last's from bones.z, three texels each, as Halo
// uploads them to its vertex shader's register 29 on.
sampler2D bones : register(s0);

// VS: x, the row's v; y, one texel across; z, where the last frame's bones start, in texels
float4 bone_row : register(c0);

// VS: this frame's world to clip, and the last's, a column of each a register
float4 now_clip[4] : register(c1);
float4 last_clip[4] : register(c5);

struct ModelVertex {
    float3 position : POSITION;
    float2 nodes : BLENDINDICES;
    float2 weights : BLENDWEIGHT;
};

struct ModelMotion {
    float4 position : POSITION;
    float4 now : TEXCOORD0;
    float4 before : TEXCOORD1;
};

float4 bone(float texel) {
    return tex2Dlod(bones, float4((texel + 0.5) * bone_row.y, bone_row.x, 0.0, 0.0));
}

// Where a node puts a vertex, its bone three texels from first
float3 placed(float3 position, float node, float first) {
    float texel = first + max(node, 0.0) * 3.0;
    float4 p = float4(position, 1.0);
    return float3(dot(bone(texel), p), dot(bone(texel + 1.0), p), dot(bone(texel + 2.0), p));
}

float4 skinned(ModelVertex vertex, float first) {
    return float4(placed(vertex.position, vertex.nodes.x, first) * vertex.weights.x
        + placed(vertex.position, vertex.nodes.y, first) * vertex.weights.y, 1.0);
}

float4 clip(float4 world, float4 columns[4]) {
    return float4(dot(world, columns[0]), dot(world, columns[1]), dot(world, columns[2]), dot(world, columns[3]));
}

ModelMotion ModelMotionVS(ModelVertex vertex) {
    ModelMotion motion;
    motion.now = clip(skinned(vertex, 0.0), now_clip);
    motion.before = clip(skinned(vertex, bone_row.z), last_clip);
    motion.position = motion.now;
    return motion;
}

// How far the model moved here, in pixels, marked as a model's. There's no depth buffer while post processes draw, so
// what of it is behind the world, as the world's depth has it, is left out here.
float4 ModelMotionPS(ModelMotion motion) : COLOR0 {
    float2 now = motion.now.xy / motion.now.w;
    float2 before = motion.before.xy / motion.before.w;
    float2 uv = pixel_middle(now * float2(0.5, -0.5) + 0.5);
    float world_distance = in_front_distance(uv);
    if(motion.now.w > world_distance * 1.01 + 0.02) {
        discard;
    }
    return float4((now - before) * float2(0.5, -0.5) * sizes.zw, 1.0, 1.0);
}
