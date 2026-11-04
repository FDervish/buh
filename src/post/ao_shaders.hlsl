// SPDX-License-Identifier: GPL-3.0-only
//
// Ambient occlusion: ground truth ambient occlusion (Jorge Jimenez et al., "Practical Realtime Strategies for Accurate
// Indirect Occlusion", 2016), worked out at half size from the world's depth. Each pixel looks along three slices
// evenly around the view, turned by a noise, for what rises highest over its surface either way within the radius,
// past a bias that lets the shallow creases between Halo's flat triangles go. It then works out how much of the sky
// over its surface (cosine weighted, about its normal) those horizons leave open. A blur that keeps to like depths
// takes the noise out, and the result is brought back to full size by depth too, and multiplied into the world. First
// person things (the weapon and arms) are occluded by themselves alone, and occlude nothing of the world.
//
// tools/compile_shader.py compiles this into ao_shaders.h.

#include "post_depth.hlsl"

// AoPS: nothing. BlurPS and ApplyPS: the occlusion so far, its visibility in r and its distance in g.
sampler2D ao_texture : register(s0);

// ApplyPS: the world
sampler2D world_texture : register(s1);

// xy: one texel of the half size occlusion; zw: the world's width and height in pixels
float4 sizes : register(c0);

// x: the radius in world units; y: how strong it is (a power of the visibility); z: how far, in radians, something has
// to rise over a surface before it occludes it
float4 ao : register(c1);

// BlurPS: one texel along the blur
float4 blur_step : register(c2);

// ApplyPS: Halo's atmospheric fog, already in the world's colours: x, how thick it gets; y, the depth it starts at; z,
// one over the depths it thickens across
float4 fog : register(c3);

#define PI 3.14159265
#define HALF_PI 1.57079633
#define SLICES 3
#define STEPS 6

// How far the blur reaches each way, in half size texels, and how much the far ones count
#define BLUR_TAPS 5
#define BLUR_SPREAD 12.0

// The most of the screen's height the radius gets, close up
#define MOST_RADIUS 0.1

// How much of the radius it fades out over, to its edge
#define FALLOFF 0.6

// How far toward the camera each pixel is looked from
#define CENTER_PULL 0.999

float noise(float2 position) {
    return frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
}

// The middle of the full size pixel a place is in
// A distance as the occlusion keeps it, for the blur and the upsample to go by: first person ones negative, so neither
// they nor the world's blur into the other
float apart(float distance, bool first_person) {
    return first_person ? -distance : distance;
}

float2 pixel_middle(float2 uv) {
    return (floor(uv * sizes.zw) + 0.5) / sizes.zw;
}

// Where a place is in view space, and what's there: the sky, the world, or first person things (the weapon and arms).
// The places looked at are all in the middles of full size pixels, where their depths are.
float3 position_at(float2 uv, out bool sky, out bool first_person) {
    float depth = world_depth(uv);
    sky = is_sky(depth);
    first_person = is_first_person(uv);
    return view_position(uv, distance_of(depth, first_person));
}

// The surface's normal from the depth around it, each way from whichever neighbour is nearer it in depth, so an edge
// doesn't bend it
float3 normal_at(float2 uv, float3 center, float2 texel) {
    bool sky;
    bool first_person;
    float3 left = position_at(uv - float2(texel.x, 0.0), sky, first_person);
    float3 right = position_at(uv + float2(texel.x, 0.0), sky, first_person);
    float3 up = position_at(uv - float2(0.0, texel.y), sky, first_person);
    float3 down = position_at(uv + float2(0.0, texel.y), sky, first_person);
    float3 across = abs(right.z - center.z) < abs(center.z - left.z) ? right - center : center - left;
    float3 along = abs(down.z - center.z) < abs(center.z - up.z) ? down - center : center - up;
    float3 normal = normalize(cross(along, across));
    return dot(normal, center) > 0.0 ? -normal : normal;
}

float4 AoPS(float2 uv : TEXCOORD0, float2 position : VPOS) : COLOR0 {
    // A half size pixel's middle is on the corner of four full size ones, where any way a sample went up or down
    // would take it a row further than the line it's on: a floor would look like steps, and occlude itself. So it
    // starts from one of their middles, and samples go whole pixels from there, as Intel's GTAO does.
    uv = pixel_middle(uv);
    bool sky;
    bool first_person;
    float3 center = position_at(uv, sky, first_person);
    float distance = -center.z;
    float kept_apart = apart(distance, first_person);
    float2 texel = 1.0 / sizes.zw;

    // The radius on screen in pixels: the projection's y scale is one over depth_constants[0].w
    float radius_pixels = min(ao.x * 0.5 * sizes.w / (depth_constants[0].w * distance), MOST_RADIUS * sizes.w);
    if(sky || radius_pixels < 1.0) {
        return float4(1.0, kept_apart, 0.0, 1.0);
    }
    float3 normal = normal_at(uv, center, texel);

    // Moved a hair toward the camera, as Intel's GTAO does, so a slanted surface's own samples, a part of a pixel off
    // its line, don't rise over it
    center *= CENTER_PULL;
    float3 view = normalize(-center);
    float falloff_multiply = -1.0 / (FALLOFF * ao.x);
    float falloff_add = (1.0 - FALLOFF) / FALLOFF + 1.0;
    float turn = noise(position);
    float jitter = noise(position + float2(37.0, 17.0));

    // Two ways square to the view, which the slices turn evenly between. Turned evenly across the screen instead, off
    // its middle they'd bunch up around the view, and a surface nothing occludes would come out darker or lighter
    // than it is, more so toward the screen's edges.
    float3 first = normalize(cross(view, abs(view.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
    float3 second = cross(view, first);

    float visibility = 0.0;
    for(int slice = 0; slice < SLICES; slice++) {
        // The slice's way across, in view space (y going up), and the line it makes on screen (v going down): where a
        // point moved that way from here goes, by the projection's x and y scales, depth_constants[0].zw
        float angle = (slice + turn) * PI / SLICES;
        float3 across_slice = first * cos(angle) + second * sin(angle);
        float2 omega = normalize(float2((center.x * across_slice.z - across_slice.x * center.z) * sizes.z / depth_constants[0].z,
            (across_slice.y * center.z - center.y * across_slice.z) * sizes.w / depth_constants[0].w));
        float3 axis = normalize(cross(across_slice, view));
        float3 projected_normal = normal - axis * dot(normal, axis);
        float projected_length = max(length(projected_normal), 0.0001);
        float cos_normal = saturate(dot(projected_normal, view) / projected_length);
        float n = (dot(across_slice, projected_normal) < 0.0 ? -1.0 : 1.0) * acos(cos_normal);

        // The most anything within the radius rises over the surface either way, as an angle in the slice: each
        // sample's height over the surface's plane, over the slice's share of the normal. Samples go on whole pixels,
        // so a part of a pixel off the slice; measured from the plane, one that's on the surface still lies on it,
        // where measured from the view, on a floor seen at a slant, it could look to tower over it.
        float rise0 = 0.0;
        float rise1 = 0.0;
        [loop]
        for(int step = 0; step < STEPS; step++) {
            float t = (step + frac(jitter + step * 0.618034)) / STEPS;
            float2 offset = round(omega * max(t * t * radius_pixels, step + 1.0)) * texel;
            bool sky0;
            bool sky1;
            bool first_person0;
            bool first_person1;
            float3 delta0 = position_at(uv + offset, sky0, first_person0) - center;
            float3 delta1 = position_at(uv - offset, sky1, first_person1) - center;
            float length0 = length(delta0);
            float length1 = length(delta1);

            // Only the same kind occludes: first person things are drawn over the world wherever they are in it (up
            // against a wall, they can be partly inside it), so neither occludes the other
            float weight0 = !sky0 && first_person0 == first_person ? saturate(length0 * falloff_multiply + falloff_add) : 0.0;
            float weight1 = !sky1 && first_person1 == first_person ? saturate(length1 * falloff_multiply + falloff_add) : 0.0;
            rise0 = max(rise0, asin(saturate(dot(delta0, normal) / (length0 * projected_length))) * weight0);
            rise1 = max(rise1, asin(saturate(dot(delta1, normal) / (length1 * projected_length))) * weight1);
        }

        // Halo's surfaces are flat triangles meeting at shallow angles, which its lighting smooths over: each rise only
        // occludes as far as it goes past the bias, scaled so straight up still occludes it all, as HBAO does for low
        // detail geometry. Real corners rise well past it.
        float bias_scale = HALF_PI / (HALF_PI - ao.z);
        rise0 = max(rise0 - ao.z, 0.0) * bias_scale;
        rise1 = max(rise1 - ao.z, 0.0) * bias_scale;

        // How much of the sky between the two horizons the surface sees, cosine weighted
        float h0 = n - HALF_PI + rise1;
        float h1 = n + HALF_PI - rise0;
        float arc0 = (cos_normal + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) * 0.25;
        float arc1 = (cos_normal + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) * 0.25;
        visibility += projected_length * (arc0 + arc1);
    }
    // Left above 1 where it comes out so, as it can at a slant, for the blur to average: clamped first, the average
    // would come out too dark
    visibility = max(visibility / SLICES, 0.0);

    // Fading out as the radius gets down to a few pixels, far away
    visibility = lerp(1.0, visibility, saturate((radius_pixels - 1.0) / 3.0));
    return float4(visibility, kept_apart, 0.0, 1.0);
}

// How much a sample at another distance counts, against one at this distance. Kept apart, first person distances are
// negative, so the two never count for each other.
float depth_weight(float other, float distance) {
    return exp(-abs(other - distance) / (0.03 * abs(distance) + 0.01));
}

// Where the surface carried on from a distance would be, i taps along at the slope of one over it: of the same sign
float carried(float inverse, float slope, float i) {
    float carried_inverse = inverse + slope * i;
    return 1.0 / (inverse < 0.0 ? min(carried_inverse, -0.0001) : max(carried_inverse, 0.0001));
}

// Taps along one way, weighted by how far along and by how near each is to the surface carried on from here. A flat
// surface's one over distance goes evenly across the screen, so its slope here (from whichever neighbour is nearer in
// depth, so an edge doesn't bend it) says where the surface would be at each tap: a surface seen at a slant is blurred
// along as much as one seen head on, and an edge still stops it.
float4 BlurPS(float2 uv : TEXCOORD0) : COLOR0 {
    float2 center = tex2D(ao_texture, uv).rg;
    float before = tex2D(ao_texture, uv - blur_step.xy).g;
    float after = tex2D(ao_texture, uv + blur_step.xy).g;
    float inverse = 1.0 / center.g;
    float slope = abs(after - center.g) < abs(center.g - before) ? 1.0 / after - inverse : inverse - 1.0 / before;
    float sum = center.r;
    float total = 1.0;
    for(int i = 1; i <= BLUR_TAPS; i++) {
        float spatial = exp(-i * i / BLUR_SPREAD);
        float2 a = tex2D(ao_texture, uv + blur_step.xy * i).rg;
        float2 b = tex2D(ao_texture, uv - blur_step.xy * i).rg;
        float weight_a = spatial * depth_weight(a.g, carried(inverse, slope, i));
        float weight_b = spatial * depth_weight(b.g, carried(inverse, slope, -i));
        sum += a.r * weight_a + b.r * weight_b;
        total += weight_a + weight_b;
    }
    return float4(sum / total, center.g, 0.0, 1.0);
}

// The world times its occlusion, from the four half size texels around each pixel, weighted by how near and how alike
// in depth. What's bright (lights, mostly) isn't darkened.
float4 ApplyPS(float2 uv : TEXCOORD0) : COLOR0 {
    float3 color = tex2D(world_texture, uv).rgb;
    float depth = world_depth(uv);
    if(is_sky(depth)) {
        return float4(color, 1.0);
    }
    bool first_person = is_first_person(uv);
    float distance = distance_of(depth, first_person);
    float kept_apart = apart(distance, first_person);
    float2 half_size = 1.0 / sizes.xy;
    float2 at = uv * half_size - 0.5;
    float2 base = floor(at);
    float2 along = at - base;
    float sum = 0.0;
    float total = 0.0;
    for(int i = 0; i < 4; i++) {
        float2 corner = float2(i % 2, i / 2);
        float2 tap = tex2Dlod(ao_texture, float4((base + corner + 0.5) * sizes.xy, 0.0, 0.0)).rg;
        float2 bilinear = lerp(1.0 - along, along, corner);
        float weight = bilinear.x * bilinear.y * depth_weight(tap.g, kept_apart) + 0.0001;
        sum += tap.r * weight;
        total += weight;
    }
    float visibility = pow(saturate(sum / total), ao.y);

    // As much as the fog has hidden the surface, it hides its occlusion: something coming out of the fog gets its
    // occlusion as it gets its own colours, rather than dark patches showing on it first
    visibility = lerp(visibility, 1.0, fog.x * saturate((distance - fog.y) * fog.z));
    float brightest = max(color.r, max(color.g, color.b));
    visibility = lerp(visibility, 1.0, smoothstep(0.75, 1.0, brightest));
    return float4(color * visibility, 1.0);
}
