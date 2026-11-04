// SPDX-License-Identifier: GPL-3.0-only
//
// Colour grading: each pixel of the world looked up in a table of colours, which buh builds from a LUT image and the
// adjustments, and dithered back to the screen's 8 bits so the grade doesn't band.
//
// tools/compile_shader.py compiles this into grading_shader.h.

sampler2D world : register(s0);
sampler3D table : register(s1);

// x: how far across the table a colour from 0 to 1 goes ((size - 1) / size); y: where its first texel's middle is
// (0.5 / size); z: how much of the grade to use
float4 grading : register(c0);

float4 GradePS(float2 uv : TEXCOORD0, float2 position : VPOS) : COLOR0 {
    float3 color = tex2D(world, uv).rgb;
    float3 graded = tex3D(table, color * grading.x + grading.y).rgb;
    float noise = frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
    return float4(lerp(color, graded, grading.z) + (noise - 0.5) / 255.0, 1.0);
}
