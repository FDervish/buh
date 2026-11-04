# Bloom

`buh_bloom [true|false]` makes the world bloom the way Halo: Reach's does. Lights, plasma and bright skies glow, the
whole picture gets a faint soft glow, and the picture itself stays as sharp as it was. It's drawn once Halo has drawn
the world and before the HUD, so the HUD, menus and console stay sharp. [Shapes in the world](shapes.md) bloom with
it.

It's off by default; put `buh_bloom true` in `init.txt` to keep it on.

## What blooms

Reach draws in HDR, where a light can be many times brighter than a white wall. Halo draws the world at 8 bits a
channel, where both are just white. So buh guesses at what was brighter than the screen could show: the closer a
pixel's brightest channel is to white, the harder it's pushed before the threshold, so lights bloom and lit walls
mostly don't. Coloured lights count by their brightest channel, so a deep blue plasma glows like a white light.

Each pixel is weighed on its own before anything is blurred, so a light a pixel or two across blooms too, and blooms
as much wherever it falls between pixels. Small lights don't pulse as they move.

## A clean picture

- The bloom is worked out at half size and smaller, and only added to the world. The world itself is never blurred
  or resampled.
- It's screen blended: it brightens a pixel only by as much as the pixel is short of white, so it can't clip or wash
  out what's already bright. It's also tone mapped, so a strong bloom rolls off rather than clipping.
- It's dithered to the screen's 8 bits, so wide, faint glows don't band.

## Settings

Without a value, each command says what it's set to.

- `buh_bloom_threshold [brightness]` (0 to 1, 0.8 by default): how bright a pixel's brightest channel is before it
  blooms. Raise it on snowy or white maps, which otherwise glow all over; lower it for more to bloom.
- `buh_bloom_intensity [amount]` (0 to 4, 0.25 by default): how much what's past the threshold blooms.
- `buh_bloom_glow [amount]` (0 to 1, 0.05 by default): how much all of the world glows, past the threshold or not.
  This is Reach's soft overall bloom; 0 leaves just the highlights.
- `buh_bloom_spread [amount]` (0 to 1, 0.7 by default): how far it spreads. Lower keeps it tight around the lights;
  higher spreads it wide.

Reach tuned its bloom for each area, and maps here can too: the commands work from `init.txt`, the console and
Chimera Lua's `execute_script`, so a map's script can set its own. A snow map might raise the threshold, for example,
or a boss fight push the intensity.

## Requirements

It needs pixel shader 3.0, and render targets of half floats that the card can filter, which any card from the last
fifteen years or so has. Without them, it says so and stays off. Each frame it copies the world, makes one pass over
it at full size, then makes a few small passes at half size and below.

The shaders are built into buh. `tools/compile_shader.py` rebuilds their header from `src/post/bloom_shaders.hlsl`:

```
python tools/compile_shader.py src/post/bloom_shaders.hlsl BrightPS bloom_bright_shader DownsamplePS bloom_downsample_shader UpsamplePS bloom_upsample_shader CompositePS bloom_composite_shader
```
