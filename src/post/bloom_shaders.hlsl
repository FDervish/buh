// SPDX-License-Identifier: GPL-3.0-only
//
// Bloom the way Call of Duty: Advanced Warfare does it (Jorge Jimenez, "Next Generation Post Processing in Call of
// Duty: Advanced Warfare", SIGGRAPH 2014). What of the world blooms is found pixel by pixel, then taken down to half
// size and halved level by level with a 13 tap filter. It's then built back up level by level with a 3x3 tent, each
// level lerped into the one above it, and that goes over the world.
//
// Halo draws the world at 8 bits a channel, where a light is no brighter than a white wall. So before the threshold,
// the bright pass guesses at what was brighter than the screen could show: it pushes a pixel up the nearer white its
// brightest channel is (an inverse tone map, up to a most), so lights and plasma bloom and lit walls mostly don't.
//
// The most a pixel is pushed to is also as much as a glint can bloom, so there's no Karis average to hold glints
// down. Everything stays linear, so a small light blooms the same wherever it falls between pixels and doesn't pulse
// as it moves.
//
// tools/compile_shader.py compiles this into bloom_shaders.h.

sampler2D source : register(s0);

// UpsamplePS: the level below
sampler2D lower : register(s1);

// One texel of what's sampled: the source, or for UpsamplePS the level below
float2 texel : register(c0);

// BrightPS: x, the threshold, pushed; y, the knee either side of it; z, the most a pixel is pushed to; w, how much of
// what's over the threshold blooms
float4 bright : register(c1);

// BrightPS: how much of everything blooms, over the threshold or not
float glow : register(c2);

// UpsamplePS: how much of the level below goes into this one, so how far the bloom spreads
float spread : register(c3);

// Jimenez's 13 taps, each bilinear between four texels (one in the middle, four a texel out diagonally, and eight two
// texels out around them), as five boxes of four: the middle one weighted 0.5, and the four overlapping corner ones
// 0.125 each
float3 downsample(float2 uv) {
    float3 a = tex2D(source, uv + float2(-2.0, -2.0) * texel).rgb;
    float3 b = tex2D(source, uv + float2(0.0, -2.0) * texel).rgb;
    float3 c = tex2D(source, uv + float2(2.0, -2.0) * texel).rgb;
    float3 d = tex2D(source, uv + float2(-2.0, 0.0) * texel).rgb;
    float3 e = tex2D(source, uv).rgb;
    float3 f = tex2D(source, uv + float2(2.0, 0.0) * texel).rgb;
    float3 g = tex2D(source, uv + float2(-2.0, 2.0) * texel).rgb;
    float3 h = tex2D(source, uv + float2(0.0, 2.0) * texel).rgb;
    float3 i = tex2D(source, uv + float2(2.0, 2.0) * texel).rgb;
    float3 j = tex2D(source, uv + float2(-1.0, -1.0) * texel).rgb;
    float3 k = tex2D(source, uv + float2(1.0, -1.0) * texel).rgb;
    float3 l = tex2D(source, uv + float2(-1.0, 1.0) * texel).rgb;
    float3 m = tex2D(source, uv + float2(1.0, 1.0) * texel).rgb;
    return (j + k + l + m) * 0.125 + ((a + b + d + e) + (b + c + e + f) + (d + e + g + h) + (e + f + h + i)) * 0.03125;
}

// A 3x3 tent, a texel apart
float3 tent(sampler2D s, float2 uv) {
    float3 sum = tex2D(s, uv + float2(-1.0, -1.0) * texel).rgb
        + tex2D(s, uv + float2(0.0, -1.0) * texel).rgb * 2.0
        + tex2D(s, uv + float2(1.0, -1.0) * texel).rgb
        + tex2D(s, uv + float2(-1.0, 0.0) * texel).rgb * 2.0
        + tex2D(s, uv).rgb * 4.0
        + tex2D(s, uv + float2(1.0, 0.0) * texel).rgb * 2.0
        + tex2D(s, uv + float2(-1.0, 1.0) * texel).rgb
        + tex2D(s, uv + float2(0.0, 1.0) * texel).rgb * 2.0
        + tex2D(s, uv + float2(1.0, 1.0) * texel).rgb;
    return sum / 16.0;
}

// What of each pixel of the world blooms: what's over the threshold once it's pushed (with a soft knee, so it comes in
// gradually), and the glow. It's found before anything is averaged, so a light a pixel across counts as much wherever
// it falls between texels.
float4 BrightPS(float2 uv : TEXCOORD0) : COLOR0 {
    float3 color = tex2D(source, uv).rgb;
    float m = max(color.r, max(color.g, color.b));
    float pushed = m / (1.0 - m + m / bright.z);
    float knee = clamp(pushed - bright.x + bright.y, 0.0, 2.0 * bright.y);
    float over = max(knee * knee / (4.0 * bright.y + 0.00001), pushed - bright.x);
    return float4(color * (over / max(m, 0.00001) * bright.w + glow), 1.0);
}

// To half size
float4 DownsamplePS(float2 uv : TEXCOORD0) : COLOR0 {
    return float4(downsample(uv), 1.0);
}

// A level built back up: itself, with the level below spread over it
float4 UpsamplePS(float2 uv : TEXCOORD0) : COLOR0 {
    return float4(lerp(tex2D(source, uv).rgb, tent(lower, uv), spread), 1.0);
}

// The bloom over the world, which is screen blended (it can brighten a pixel only as far as white). It's tone mapped
// so it rolls off rather than clipping, and dithered to the screen's 8 bits so the faint edges of a wide bloom don't
// band.
float4 CompositePS(float2 uv : TEXCOORD0, float2 position : VPOS) : COLOR0 {
    float3 bloom = 1.0 - exp(-tent(source, uv));
    float noise = frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
    return float4(bloom + (noise - 0.5) / 255.0, 1.0);
}
