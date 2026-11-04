# Motion blur

`buh_motion_blur [true|false]` blurs the world as things move: as the camera turns and moves, the way Halo: Reach's
does, and also for whatever moves by itself, like a passing Warthog, a running Elite, a spinning wheel or a melee
swing. It's off by default; put `buh_motion_blur true` in `init.txt` to keep it on.

## How it works

Once Halo has drawn the world's opaque surfaces, buh works out how far each pixel moved since the last frame:

- **The camera's motion:** each pixel is put back where the last frame's camera saw it, from its depth (see
  [the world's depth](depth.md)). The sky only turns.
- **Each model's own motion:** as Halo draws a model's parts, buh notes each part and the bones Halo skinned it with.
  It then draws each part that moved again, its vertices skinned with this frame's bones and the last frame's. So a
  model's motion is exact to the pixel and follows its animation: legs, arms, wheels, turrets. A model the camera keeps
  pace with, like the vehicle you're driving, comes out still, and stays sharp while the world streaks past.

Before the HUD, the world is blurred along that motion with a reconstruction filter (McGuire et al., 2012), the
method most games' motion blur builds on. Something moving blurs over what's behind it, rather than the two being
smeared together. Something still in front of something moving, like your weapon against the world as you turn, stays
sharp.

- It's scaled to a shutter time rather than to the frame, so it looks the same at any frame rate.
- It's capped at 5% of the screen's height.
- A frame where the camera jumps (a respawn, a cutscene cut, a teleport) isn't blurred, and nor is anything while the
  game is paused.
- It's drawn after [SMAA](anti-aliasing.md), [sun shafts](sun-shafts.md) and the
  [volumetric light](volumetric-light.md), and before the [bloom](bloom.md).
- Particles, effects and transparent surfaces aren't blurred for their own motion, only the camera's. Nor is a model
  the first frame it's seen.
- The first person weapon and arms move with the camera, so they stay sharp: neither the camera's motion nor their own
  animation blurs them. Halo draws them over the world wherever they are in it, so nothing of the world blurs over them
  either, even a wall you're up against that's nearer than the weapon (see [the world's depth](depth.md#first-person)).

## Settings

- `buh_motion_blur_amount [amount]` (0 to 1, 0.5 by default): how long the shutter is open, as a fraction of a frame
  at 30 frames a second. 0.5 is a 60th of a second.
- `buh_motion_blur_objects [true|false]` (true by default): blur models for their own motion. Turned off, the blur is
  the camera's alone.

## Checking it

`buh_motion_view [true|false]` shows the motion in place of the world, for as long as motion blur is on:

- still is gray;
- moving right makes a pixel redder and moving left less red; moving down makes it greener and moving up less green,
  more so the faster it goes;
- models moving for themselves are bluish.

If a moving model shows bluish and tinted in game, and the vehicle you're driving shows bluish but with next to no red
or green (it moves in the world, but not on the screen), both kinds of motion are working.

If the motion can't be worked out for a second or so, the console says why, once each time motion blur is turned on.
A line starting `buh: motion blur can't work out the motion:` names what's missing: Halo's opaque world not coming
through buh's hook, Halo's camera, or a camera that jumps every frame. When the world's depth is what's missing,
[the depth](depth.md) says so itself.

## Requirements

It needs pixel shader 3.0, half float render targets, and [the world's depth](depth.md). Models' own motion also needs
vertex shader 3.0 that can read 32-bit float textures. Without that it says so, and the blur is the camera's alone.
