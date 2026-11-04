# Anti-aliasing

`buh_smaa [true|false]` smooths the world's jagged edges with SMAA, Enhanced Subpixel Morphological Antialiasing, by
Jorge Jimenez and others. Halo has no anti-aliasing of its own. It's off by default; put `buh_smaa true` in `init.txt`
to keep it on.

## How it works

SMAA works on the finished picture of the world. It finds edges by their brightness and works out the shape of the
steps along each one. Then it blends each pixel there with its neighbour by as much as the real edge would cover it.

- It runs once Halo has drawn the world, before the [bloom](bloom.md), [colour grading](color-grading.md) and the HUD.
  The bloom works from the smoothed edges, and the HUD's text and lines stay sharp.
- Pixels away from edges aren't touched, so textures don't get blurred.
- Where the card can read and write sRGB, it blends in linear light, as SMAA's authors intend, so edges come out as
  bright as they should.
- It uses SMAA's high preset, which its authors put at 95% of its full quality.

## What it can't do

It only sees whole pixels, so detail smaller than one can still shimmer as you move: a distant railing or wire, or
specular sparkle. Supersampling fixes those. Your graphics driver can do it: NVIDIA's DSR or DLDSR, or AMD's VSR,
with Halo run at the higher resolution. SMAA on top then cleans up what's left.

A long straight edge stays as it is, except near a corner. Within the 32 pixels SMAA searches, it takes the corner
for a step and ramps into it a little.

## Requirements

It needs pixel shader 3.0. SMAA's shader and lookup textures are built into buh. `src/post/smaa` holds upstream's
files, unmodified, and says where they're from. `tools/compile_shader.py` rebuilds the shaders' header:

```
python tools/compile_shader.py src/post/smaa_shaders.hlsl EdgesPS smaa_edges_shader WeightsPS smaa_weights_shader BlendPS smaa_blend_shader
```
