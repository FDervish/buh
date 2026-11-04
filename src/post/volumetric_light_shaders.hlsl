// SPDX-License-Identifier: GPL-3.0-only
//
// Volumetric light: the light the air between the camera and the world scatters toward it, from the sun and from
// Halo's dynamic lights. So shafts of sunlight show in the air from any side, not just looking toward the sun.
//
// The sun's comes through a shadow map: the level drawn from the sun (ShadowVS and ShadowPS), each texel how far toward
// the sun the nearest of it is. At half size, each pixel's way to the world is stepped along (MarchPS), and each step
// the shadow map says is lit adds the sun's light, scattered more toward the sun than away (Henyey-Greenstein, half and
// half with even scattering, so it still shows from the side). The air nearer counts more, as the air in front hides
// what's further. A blur that keeps to like distances takes out the steps' noise (BlurPS, then LightsPS). Each dynamic
// light's is then added, worked out whole along the way, in a few pieces for its cone: its light fades out to its
// radius, and the world in front of it stops it (LightsPS). The light is brought back to full size by distance too,
// and screen blended over the world (ApplyPS), as the bloom is, so it can't clip.
//
// tools/compile_shader.py compiles this into volumetric_light_shaders.h.

#include "post_depth.hlsl"

// ShadowVS: the sun's view of the level. Each row, times a place in the world, gives where it is across the shadow
// map, up it, and how far toward the sun (0 to 1, the nearest of the level to the furthest).
float4 shadow_rows[3] : register(c0);

// MarchPS: the shadow map
sampler2D shadow_texture : register(s0);

// BlurPS, LightsPS and ApplyPS: the light so far, its colour in rgb and in a the distance of what it's in front of
// (negative for first person things, so neither they nor the world's blur into the other)
sampler2D light_texture : register(s1);

// xy: one half size texel; zw: the world's width and height in pixels
float4 sizes : register(c0);

// MarchPS: from view space to the shadow map, each a row (its xyz times a place, plus w): across, up, and toward the sun
float4 to_shadow[3] : register(c1);

// MarchPS: rgb, the sun's light; w, how much further from the sun than the shadow map's depth a place can be and be lit
float4 sun_light : register(c7);

// MarchPS: xyz, the way to the sun in view space; w, how far from the camera the sun's light is gathered, in world units
float4 sun_view : register(c8);

// MarchPS: y, how much the air hazes where the sun lights all of the way
float4 counts : register(c9);

// LightsPS: how many dynamic lights there are, an integer register for the loop over them to count with
int light_count : register(i0);

// BlurPS and LightsPS: one texel along the blur
float4 blur_step : register(c10);

// LightsPS: the dynamic lights. light_places: xyz, where each is in view space; w, one over its radius squared.
// light_colors: rgb, its light; w, how much the cosine off its cone's axis scales by. light_axes: xyz, its axis in view
// space; w, what the scaled cosine is offset by (an omnidirectional light's scale is 0 and its offset 1). Arrays of
// their own: the compiler picks a light's out of an array by comparing for each register it could be in, sixteen this
// way rather than forty-eight.
float4 light_places[16] : register(c16);
float4 light_colors[16] : register(c32);
float4 light_axes[16] : register(c48);

#define SUN_SAMPLES 32
#define MOST_LIGHTS 16
#define LIGHT_PIECES 4

// How thick the air is: how much light a world unit of it scatters, and how much it hides of what's past it
#define DENSITY 0.02

// How much scattering leans forward, along the light's way
#define FORWARD 0.5

// The distance the sky is kept at
#define SKY_DISTANCE 4096.0

// How far the blur reaches each way, in half size texels, and how much the far ones count
#define BLUR_TAPS 5
#define BLUR_SPREAD 8.0

float noise(float2 position) {
    return frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
}

float4 ShadowVS(float3 position : POSITION, out float depth : TEXCOORD0) : POSITION {
    float4 world = float4(position, 1.0);
    float3 at = float3(dot(shadow_rows[0], world), dot(shadow_rows[1], world), dot(shadow_rows[2], world));
    depth = at.z;
    return float4(at, 1.0);
}

// Pushed away from the sun by as much as the depth changes across a texel, so a surface the sun grazes (a floor, with
// the sun low) doesn't shadow the air just over it
float4 ShadowPS(float depth : TEXCOORD0) : COLOR0 {
    return float4(depth + abs(ddx(depth)) + abs(ddy(depth)), 0.0, 0.0, 1.0);
}

// How much light coming one way is scattered back along another, by the cosine between them: 1 averaged over every way
float phase(float cosine) {
    float g2 = FORWARD * FORWARD;
    return 0.5 + 0.5 * (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * FORWARD * cosine, 0.0001), 1.5);
}

// How much light the air scatters along the way from the camera, reach long, the nearer counting more: in x where the
// sun lights it, in y as if it lit all of it
float2 sun_along(float3 direction, float reach, float2 position) {
    float3 start = float3(to_shadow[0].w, to_shadow[1].w, to_shadow[2].w);
    float3 step = float3(dot(to_shadow[0].xyz, direction), dot(to_shadow[1].xyz, direction), dot(to_shadow[2].xyz, direction)) * (reach / SUN_SAMPLES);
    float jitter = noise(position);
    float2 sum = 0.0;
    [loop]
    for(int i = 0; i < SUN_SAMPLES; i++) {
        float along = i + jitter;
        float3 at = start + step * along;
        float nearest = tex2Dlod(shadow_texture, float4(at.xy, 0.0, 0.0)).r;
        float weight = exp(-DENSITY * reach * along / SUN_SAMPLES);
        sum += float2(at.z <= nearest + sun_light.w ? weight : 0.0, weight);
    }
    return sum * (DENSITY * reach / SUN_SAMPLES);
}

// The dynamic lights' along the way from the camera, reach long. Within a light's radius, its light is taken as
// 1 - d^2 / r^2 of what it is at its middle, d away from it, so along the way it's a polynomial, worked out whole for
// each piece; the piece's middle gives its cone and which way it's scattered.
float3 lights_along(float3 direction, float reach) {
    float3 sum = 0.0;
    [loop]
    for(int i = 0; i < light_count; i++) {
        float4 where = light_places[i];
        float4 light = light_colors[i];
        float4 cone = light_axes[i];
        float closest = dot(direction, where.xyz);
        float off = max(dot(where.xyz, where.xyz) - closest * closest, 0.0);
        float inside = 1.0 / where.w - off;
        float half_chord = sqrt(max(inside, 0.0));
        float t0 = max(closest - half_chord, 0.0);
        float t1 = min(closest + half_chord, reach);
        if(inside > 0.0 && t1 > t0) {
            float total = 0.0;
            for(int piece = 0; piece < LIGHT_PIECES; piece++) {
                float s0 = lerp(t0, t1, piece / (float)LIGHT_PIECES) - closest;
                float s1 = lerp(t0, t1, (piece + 1) / (float)LIGHT_PIECES) - closest;
                float radial = (1.0 - off * where.w) * (s1 - s0) - (s1 * s1 * s1 - s0 * s0 * s0) * where.w / 3.0;
                float3 from_light = direction * (closest + (s0 + s1) * 0.5) - where.xyz;
                float3 way = from_light * rsqrt(max(dot(from_light, from_light), 0.000001));
                float spot = saturate(dot(way, cone.xyz) * light.w + cone.w);
                // Scattered toward the camera, back along the way
                total += max(radial, 0.0) * spot * phase(-dot(direction, way));
            }
            sum += light.rgb * total;
        }
    }
    return sum;
}

// The way from the camera through a place on screen, and how far along it what's there is (the sky as far as can be)
void way_at(float2 uv, out float3 direction, out float distance, out bool first_person) {
    float depth = world_depth(uv);
    bool sky = is_sky(depth);
    first_person = is_first_person(uv);
    float3 end = view_position(uv, sky ? 1.0 : distance_of(depth, first_person));
    direction = normalize(end);
    distance = sky ? SKY_DISTANCE : length(end);
}

// The sun's light along the way, which the blur then takes the steps' noise out of
float4 MarchPS(float2 uv : TEXCOORD0, float2 position : VPOS) : COLOR0 {
    float3 direction;
    float distance;
    bool first_person;
    way_at(uv, direction, distance, first_person);
    float3 light = 0.0;
    [branch]
    if(any(sun_light.rgb > 0.0)) {
        // Where the sun lights all of the way (across open ground, say), the air only hazes, as much as counts.y says.
        // Where shadow breaks the way up, its light shows in full: shafts through an opening, past a pillar's shadow.
        float2 sun = sun_along(direction, min(distance, sun_view.w), position);
        float shafts = sun.x * (counts.y + (1.0 - counts.y) * (1.0 - sun.x / max(sun.y, 0.000001)));
        light = sun_light.rgb * phase(dot(direction, sun_view.xyz)) * shafts;
    }
    return float4(light, first_person ? -distance : distance);
}

// How much a distance counts against another, so the light in front of something near doesn't blur into what's in
// front of something far, or first person things' into the world's
float alike(float other, float distance) {
    return exp(-abs(other - distance) / (0.05 * abs(distance) + 0.05));
}

float4 blurred(float2 uv) {
    float4 center = tex2D(light_texture, uv);
    float3 sum = center.rgb;
    float total = 1.0;
    for(int i = 1; i <= BLUR_TAPS; i++) {
        float spatial = exp(-i * i / BLUR_SPREAD);
        float4 a = tex2D(light_texture, uv + blur_step.xy * i);
        float4 b = tex2D(light_texture, uv - blur_step.xy * i);
        float weight_a = spatial * alike(a.a, center.a);
        float weight_b = spatial * alike(b.a, center.a);
        sum += a.rgb * weight_a + b.rgb * weight_b;
        total += weight_a + weight_b;
    }
    return float4(sum / total, center.a);
}

float4 BlurPS(float2 uv : TEXCOORD0) : COLOR0 {
    return blurred(uv);
}

// The blur's other half, and the dynamic lights' light added after it: worked out whole, it has no noise to take out,
// and blurred, a small light's glow (brightest right around it, where light scatters on toward the camera) would
// spread out and dim
float4 LightsPS(float2 uv : TEXCOORD0) : COLOR0 {
    float4 light = blurred(uv);
    float3 direction;
    float distance;
    bool first_person;
    way_at(uv, direction, distance, first_person);
    return float4(light.rgb + lights_along(direction, distance), light.a);
}

// From the four half size texels around each pixel, weighted by how near and how alike in distance, tone mapped so it
// rolls off, and dithered so it doesn't band
float4 ApplyPS(float2 uv : TEXCOORD0, float2 position : VPOS) : COLOR0 {
    float depth = world_depth(uv);
    bool first_person = is_first_person(uv);
    float distance = is_sky(depth) ? SKY_DISTANCE : length(view_position(uv, distance_of(depth, first_person)));
    float kept = first_person ? -distance : distance;
    float2 half_size = 1.0 / sizes.xy;
    float2 at = uv * half_size - 0.5;
    float2 base = floor(at);
    float2 along = at - base;
    float3 sum = 0.0;
    float total = 0.0;
    for(int i = 0; i < 4; i++) {
        float2 corner = float2(i % 2, i / 2);
        float4 tap = tex2Dlod(light_texture, float4((base + corner + 0.5) * sizes.xy, 0.0, 0.0));
        float2 bilinear = lerp(1.0 - along, along, corner);
        float weight = bilinear.x * bilinear.y * alike(tap.a, kept) + 0.0001;
        sum += tap.rgb * weight;
        total += weight;
    }
    float3 light = 1.0 - exp(-sum / total);
    return float4(light + (noise(position) - 0.5) / 255.0, 1.0);
}
