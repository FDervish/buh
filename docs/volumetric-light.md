# Volumetric light

`buh_volumetric_light [true|false]` lights the air: sunlight falling through a doorway, a window or a gap in the rocks
shows as a shaft in the air from any side, not just looking toward the sun. Halo's dynamic lights glow in the air
around them too: the flashlight, muzzle flashes, glowing projectiles and explosions. It's off by default; put
`buh_volumetric_light true` in `init.txt` to keep it on.

It goes well with the [sun shafts](sun-shafts.md), which streak the light out from the sun itself when you look toward
it.

## The sun

The sun is the sun shafts' sun: the map's, the brightest light of its first sky, or wherever
`buh_sun_shafts_direction` puts it. Its light is shadowed by the level itself: the level's collision geometry, the
polygons Halo's bullets hit, drawn from the sun into a shadow map around you. The sky's own polygons, invisible walls
and what light goes through (glass, water, energy barriers) don't shadow it. A map's lightmaps were lit from its sky's
lights, so the shafts should line up with the patches of sunlight on the floor. If they don't on some map,
`buh_sun_shafts_direction` moves the sun.

At half size, each pixel steps along the way from you to what's there, up to 48 world units out, and adds the sun's
light wherever the shadow map says it reaches. Air scatters light forward more than to the side, so the shafts are
brightest looking toward the sun, but they show from the side and from behind too. Nearer air counts more, as the air
in front hides what's further.

Where the sun lights all of the way (across open ground, say), the air only hazes a little. Where shadow breaks the
way up, past a pillar's shadow or through a doorway into a dark room, the light shows in full: those are the shafts.

The shadow map is drawn again only for another sun or another level, or once you've gone a way from where it was
last drawn. The level doesn't move, so it doesn't need drawing every frame.

## Dynamic lights

Halo's dynamic lights are lit in the air too, the sixteen that matter most each frame: the brightest, the biggest, and
the nearest. Each one's light fades out to its radius, and spotlights only light their cone. They aren't shadowed, but the world in front of a light hides the light behind it, so a light round a
corner doesn't glow through the wall.

## How it's put in

The sun's light is blurred to take the steps' noise out, keeping what's near apart from what's far. The dynamic lights'
is worked out whole, with no noise to take out, so it's added after the blur and a small light's glow stays crisp.
It's all brought back to full size by distance too, then screen blended over the world, as the [bloom](bloom.md) is,
so it can't clip. It goes on
after [SMAA](anti-aliasing.md) and the [sun shafts](sun-shafts.md), and before [motion blur](motion-blur.md) and the
bloom, so it blurs and glows with the rest of the world. The first person weapon and arms are in front of all the air,
so it doesn't light them.

## Settings

- `buh_volumetric_light_intensity [amount]` (0 to 4, 1 by default): how bright the sun's light in the air is. 0 leaves
  the sun out.
- `buh_volumetric_light_haze [amount]` (0 to 1, 0.1 by default): how much air the sun lights all the way through glows,
  against the shafts. 0 leaves only the shafts; 1 hazes open ground as much as the shafts.
- `buh_volumetric_light_lights [amount]` (0 to 4, 1 by default): how bright the dynamic lights' light in the air is. 0
  leaves them out.

## Requirements

It needs pixel shader 3.0, half float render targets, and [the world's depth](depth.md). The sun's light also needs
vertex shader 3.0 and 32 bit float render targets for its shadow map; without them, it says so and leaves the sun out.
If the level's geometry can't be read (a protected map, say), the console says so, and the sun is left out too.
