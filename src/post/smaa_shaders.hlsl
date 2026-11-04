// SPDX-License-Identifier: GPL-3.0-only
//
// SMAA 1x (Jimenez et al., "SMAA: Enhanced Subpixel Morphological Antialiasing", 2012) from upstream's SMAA.hlsl,
// unmodified, in smaa/: edges found by luma, then how much each edge pixel blends with which neighbour, then the
// blend. SMAA works its offsets out in vertex shaders, but buh draws its quads without one, so they're worked out here.
//
// tools/compile_shader.py compiles this into smaa_shaders.h.

// 1 / width, 1 / height, width, height of the world
float4 metrics : register(c0);

#define SMAA_RT_METRICS metrics
#define SMAA_HLSL_3
#define SMAA_PRESET_HIGH
#include "smaa/SMAA.hlsl"

// The world, then the edges, SMAA's area and search lookup textures, and the blend weights
sampler2D color_texture : register(s0);
sampler2D edges_texture : register(s1);
sampler2D area_texture : register(s2);
sampler2D search_texture : register(s3);
sampler2D blend_texture : register(s4);

float4 EdgesPS(float2 texcoord : TEXCOORD0) : COLOR0 {
    float4 offset[3];
    SMAAEdgeDetectionVS(texcoord, offset);
    return float4(SMAALumaEdgeDetectionPS(texcoord, offset, color_texture), 0.0, 0.0);
}

float4 WeightsPS(float2 texcoord : TEXCOORD0) : COLOR0 {
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
    return SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edges_texture, area_texture, search_texture, 0.0);
}

float4 BlendPS(float2 texcoord : TEXCOORD0) : COLOR0 {
    float4 offset;
    SMAANeighborhoodBlendingVS(texcoord, offset);
    return SMAANeighborhoodBlendingPS(texcoord, offset, color_texture, blend_texture);
}
