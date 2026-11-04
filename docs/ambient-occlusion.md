# Ambient occlusion

`buh_ao [true|false]` darkens where surfaces meet and close in: where a character stands on the ground, a crate meets
a wall, and in corners. Halo's lightmaps already shade the level, so the radius is small. What it adds is mostly the
contact that sits characters and vehicles in the world. It's off by default; put `buh_ao true` in `init.txt` to keep
it on.

## How it works

It's ground truth ambient occlusion (GTAO), from the world's depth (see [the world's depth](depth.md)). At half size,
each pixel looks in three directions spread evenly around the view, turned by a noise, for whatever rises highest over
its surface either way within the radius. It then works out how much of the sky over its surface those horizons leave
open. A blur takes the noise out. It follows the surface, so a wall seen at a slant is smoothed as well as one seen
head on, but it stops at edges. The result is brought back to full size by depth too, so it doesn't bleed across edges.

- Halo's levels are made of flat triangles meeting at shallow angles, which its lighting smooths over. Something has
  to rise more than a few degrees over a surface before it counts as occluding it (the bias), so those creases don't
  show and each triangle doesn't get a shade of its own. Real corners rise well past it.
- What rises is measured from the surface rather than from the view. So a floor seen at a slant doesn't blotch, and
  nothing a surface doesn't actually have darkens it, wherever it is on the screen.

- It's put in once Halo has drawn the world's opaque surfaces and before its transparent ones. So smoke, glass, water,
  particles and the like go over the occluded world rather than being darkened along with it.
- It fades with Halo's atmospheric fog, by the same amount the fog hides each surface. So something coming out of the
  fog gets its occlusion as it gets its own colours, rather than dark patches showing on it first. Fog planes (fog
  volumes) are drawn with the transparent surfaces, so they go over it anyway.
- First person things (the weapon and arms) are occluded by themselves alone, and don't occlude the world. Halo draws
  them over the world wherever they are in it, even partly inside a wall you're up against, so each is left out of the
  other's (see [the world's depth](depth.md#first-person)).
- The sky isn't occluded.
- Bright pixels (lights, mostly) aren't darkened, so self-lit surfaces stay lit.
- It fades out with distance, as the radius gets down to a few pixels.

## Settings

- `buh_ao_radius [world units]` (0.05 to 2, 0.25 by default, about two and a half feet): how far it looks for what
  closes in.
- `buh_ao_intensity [amount]` (0 to 4, 1 by default): how dark. It's a power of how much is left open, so 2 darkens
  twice over.
- `buh_ao_bias [degrees]` (0 to 60, 15 by default): how far something has to rise over a surface before it occludes
  it. If the creases between a map's triangles still show, raise it; lower it for more occlusion in shallow hollows.

## Requirements

It needs pixel shader 3.0, half float render targets, and [the world's depth](depth.md). It also needs buh's hook
before Halo's transparent geometry, which `buh_ao true` says if it can't find.
