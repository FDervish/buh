# The world's depth

[Ambient occlusion](ambient-occlusion.md), [sun shafts](sun-shafts.md), [volumetric light](volumetric-light.md) and
[motion blur](motion-blur.md) work from the world's depth: how far away each pixel is. Halo draws into a depth buffer that can't be read. So while one of
them is on, buh has Halo draw into one that can be, an INTZ depth texture, set in place of Halo's own. Once none has
been on for a moment, Halo's own goes back, and nothing about Halo's drawing is any different from before.

## Checking it

`buh_depth_view [true|false]` shows what buh reads in place of the world:

- near is white, fading to black about 100 world units out;
- the sky is blue;
- first person things (the weapon and arms) are red.

If the view looks like that in game, the depth effects have what they need. It goes over everything else, but not the
HUD.

If the depth can't be read for a second or so, the console says why, once each time an effect that reads it is
turned on. A line starting `buh: the world's depth can't be read:` says what got in the way: Present never coming
through buh, Direct3D not taking buh's depth buffer (with its error), buh's depth buffer being let go (and why), another
depth buffer (with its format and size) set as the world is drawn, or Halo's camera not being readable.

## First person

Halo draws the first person weapon and arms before the world, with near and far clip distances of their own (a near
clip of 3/256 of a unit by default, against the world's 1/16), and marks them in the stencil buffer so the world drawn
after doesn't cover them. That's why the weapon never pokes into a wall you're
up against. So the same depth is a different distance there. Once a frame, buh draws where the stencil buffer marks
them into a mask, so the effects tell them apart exactly and read their depth at its true distance:

- the weapon is ambient occluded by itself (the arms on it, say), but the world and it don't occlude each other;
- it stays sharp in motion blur, and nothing of the world blurs over it, even a wall nearer than it.

## Requirements

It needs a card and driver that can read INTZ depth textures, which cards since Direct3D 10 or so can. It doesn't work
with multisampling forced on in the driver, since a readable depth buffer can't be multisampled. Without it, the depth
effects say so and stay off.
