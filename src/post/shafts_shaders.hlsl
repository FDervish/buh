// SPDX-License-Identifier: GPL-3.0-only
//
// Sun shafts: light scattering as a post process (Kenny Mitchell, "Volumetric Light Scattering as a Post-Process",
// GPU Gems 3, 2007). The sky around the sun is taken at half size, with everything in front of it black. Then it's
// blurred out from the sun twice, the second time a whole first blur's length a step, for the many samples it takes
// without banding, and screen blended over the world in the sun's colour.
//
// tools/compile_shader.py compiles this into shafts_shaders.h.

#include "post_depth.hlsl"

// MaskPS: the world. RadialPS: the last pass. CompositePS: the shafts.
sampler2D source : register(s0);

// xy: one texel of the source
float4 sizes : register(c0);

// xy: where the sun is on screen (0 to 1 across the world); z: one over how far from it the sky counts, in screen
// heights; w: the world's width over its height
float4 sun : register(c1);

// RadialPS: x, how far toward the sun this pass reaches, as a fraction of the way; y, how fast it fades along
// CompositePS: rgb, the sun's colour times how strong the shafts are
float4 shafts : register(c2);

#define SAMPLES 12

float noise(float2 position) {
    return frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
}

// The sky, fading out away from the sun; anything in front of it is black
float4 MaskPS(float2 uv : TEXCOORD0) : COLOR0 {
    if(!is_sky(world_depth(uv))) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    float2 away = (uv - sun.xy) * float2(sun.w, 1.0);
    float near_sun = saturate(1.0 - length(away) * sun.z);
    return float4(tex2D(source, uv).rgb * near_sun * near_sun, 1.0);
}

// Each pixel the average of the way from it toward the sun, fading with distance along it
float4 RadialPS(float2 uv : TEXCOORD0, float2 position : VPOS) : COLOR0 {
    float2 toward = (sun.xy - uv) * shafts.x;
    float start = noise(position);
    float3 sum = 0.0;
    float total = 0.0;
    [loop]
    for(int i = 0; i < SAMPLES; i++) {
        float along = (i + start) / SAMPLES;
        float weight = exp(-shafts.y * along * shafts.x);
        sum += tex2Dlod(source, float4(uv + toward * along, 0.0, 0.0)).rgb * weight;
        total += weight;
    }
    return float4(sum / total, 1.0);
}

// Screen blended over the world in the sun's colour, tone mapped so it rolls off, and dithered so it doesn't band
float4 CompositePS(float2 uv : TEXCOORD0, float2 position : VPOS) : COLOR0 {
    float3 light = 1.0 - exp(-tex2D(source, uv).rgb * shafts.rgb);
    return float4(light + (noise(position) - 0.5) / 255.0, 1.0);
}
