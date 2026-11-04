# Sun shafts

`buh_sun_shafts [true|false]` adds shafts of sunlight: where the sun shines past something in front of it, like a
wall's edge, a doorway or the trees, light streaks out from it across the screen. It's off by default; put
`buh_sun_shafts true` in `init.txt` to keep it on.

## The sun

The sun is the map's: the brightest light of the map's first sky, which is also where Halo puts the sun's lens flare.
The shafts take its colour. `buh_sun_shafts_direction` says where it is, in degrees: its yaw round from the world's x
toward y, and its pitch up from the horizon.

For a map whose sky has no light, or to put the sun elsewhere, give it:
`buh_sun_shafts_direction <yaw> <pitch>`. The shafts are then white, unless the sky has a light to take the colour
from. `buh_sun_shafts_direction map` goes back to the map's. The [volumetric light](volumetric-light.md) shares the
sun, so this moves its sun too.

## How it works

At half size, the sky around the sun is taken from the world, with everything in front of the sky black (see
[the world's depth](depth.md)). That's blurred out from the sun twice, the second time a whole first blur's length a
step, for the many samples shafts take without banding. It's then screen blended over the world, as the
[bloom](bloom.md) is, so it can't clip. It's drawn before the bloom, so the bloom glows with it.

The shafts fade out as the sun leaves the screen and are gone once it's more than about 70 degrees off where you're
looking. For shafts in the air from any side, through doorways and gaps whichever way you look, turn on the
[volumetric light](volumetric-light.md) too: it shares this sun.

## Settings

- `buh_sun_shafts_intensity [amount]` (0 to 4, 1 by default): how bright.
- `buh_sun_shafts_length [amount]` (0 to 1, 0.75 by default): how far the shafts reach, as a fraction of the way to
  the sun.

## Requirements

It needs pixel shader 3.0 and [the world's depth](depth.md).
