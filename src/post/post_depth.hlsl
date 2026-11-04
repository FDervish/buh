// SPDX-License-Identifier: GPL-3.0-only
//
// Reading the world's depth, for the post processes that bind it with bind_world_depth() (depth.cpp). View space looks
// down -z, as Halo's does.
//
// First person things (the weapon and arms) are drawn with near and far clip distances of their own, so the same
// depth is a different distance there. Halo marks them in the stencil buffer as it draws them, which depth.cpp turns
// into a mask.

sampler2D first_person_texture : register(s6);
sampler2D depth_texture : register(s7);

// [0]: xy, the terms that turn the world's depth into distance; zw, one over the projection's x and y scales.
// [1]: xy, its x and y offsets; zw, the terms that turn first person depth into distance.
// [2]: where a place in the world (0 to 1 across the viewport) is in the depth texture: scaled by xy, offset by zw.
float4 depth_constants[3] : register(c4);

// The depth at or past which it's the sky, which Halo clears the depth buffer to and draws nothing at
#define SKY_DEPTH 0.9999999

// Where a place in the world is in the depth texture and the first person mask
float2 depth_uv(float2 uv) {
    return uv * depth_constants[2].xy + depth_constants[2].zw;
}

// The depth buffer at a place in the world
float world_depth(float2 uv) {
    return tex2Dlod(depth_texture, float4(depth_uv(uv), 0.0, 0.0)).r;
}

bool is_sky(float depth) {
    return depth >= SKY_DEPTH;
}

// Whether a place on screen is first person
bool is_first_person(float2 uv) {
    return tex2Dlod(first_person_texture, float4(depth_uv(uv), 0.0, 0.0)).r > 0.5;
}

// How far in front of the camera a depth is, in world units, for the world or for first person things
float distance_of(float depth, bool first_person) {
    float2 terms = first_person ? depth_constants[1].zw : depth_constants[0].xy;
    return terms.x / (depth + terms.y);
}

// How far in front of the camera what's at a place on screen is
float distance_at(float2 uv) {
    return distance_of(world_depth(uv), is_first_person(uv));
}

// Where a place on screen is in view space, that far in front of the camera
float3 view_position(float2 uv, float distance) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3((ndc + depth_constants[1].xy) * depth_constants[0].zw * distance, -distance);
}

// The view space direction through a place on screen, for the sky, which is as far as can be
float3 view_direction(float2 uv) {
    return normalize(view_position(uv, 1.0));
}
