// SPDX-License-Identifier: GPL-3.0-only
//
// MaskPS: the first person mask, drawn where Halo's stencil buffer marks first person things. DepthViewPS:
// buh_depth_view, what buh reads of the world's depth, in place of the world. Near is white, fading to black 100 world
// units out; first person things are red, and the sky is blue.
//
// tools/compile_shader.py compiles this into depth_shaders.h.

#include "post_depth.hlsl"

float4 MaskPS(float2 uv : TEXCOORD0) : COLOR0 {
    return 1.0;
}

float4 DepthViewPS(float2 uv : TEXCOORD0) : COLOR0 {
    float depth = world_depth(uv);
    if(is_sky(depth)) {
        return float4(0.3, 0.5, 0.9, 1.0);
    }
    bool first_person = is_first_person(uv);
    float shade = exp(-distance_of(depth, first_person) / 30.0);
    return first_person ? float4(shade, shade * 0.2, shade * 0.2, 1.0) : float4(shade, shade, shade, 1.0);
}
