# Shapes

Rectangles, lines, quads and circles on the HUD, and lines, boxes and circles in the world. They stay up until
they're replaced, removed or their time runs out, so a script only has to send one when something about it changes,
not every frame. (For drawing every frame from Lua instead, see [Lua functions](lua.md#drawing).)

## On the HUD

In the same order as [sprites](sprites.md) (one added after another draws over it), placed like them in fractions of
the screen from the top left:

- `buh_rect <name> <x> <y> <width> <height> [options]`, and `outline [thickness]`, `round <radius>` for rounded
  corners
- `buh_line <name> <x1> <y1> <x2> <y2> [options]`
- `buh_quad <name> <x1> <y1> <x2> <y2> <x3> <y3> <x4> <y4> [options]`
- `buh_circle <name> <x> <y> <radius> [options]`, and `outline [thickness]`, `arc <from> <to>` (degrees clockwise
  from the top) for an arc, or filled, a slice. A progress ring is `outline 6 arc 0 270`.

A circle's radius and a rectangle's corners are fractions of the screen's height, so they stay round; thicknesses are
pixels. Plain rectangles are pixel-sharp, and slanted or curved edges are smoothed over a pixel. Options for any of
them:

- `color <r> <g> <b>` (0 to 1) or `color #rrggbb` (`#rrggbbaa` with alpha); white by default
- `gradient <color>`: to this colour, top to bottom on a rectangle, start to end on a line or an outlined arc, the
  first two corners to the last two on a quad, and middle to edge on a filled circle
- `thickness <pixels>`: lines are 2 pixels by default
- `opacity`, `time`, `fade`, `move` and `rotate`, as for [sprites](sprites.md)

For example, a half-transparent red square in the middle of the screen:

```
buh_rect check 0.4 0.4 0.2 0.2 color #ff000080
```

## In the world

At world coordinates. They're drawn once Halo has drawn the world and before the HUD, with the camera and depth buffer
the world was just drawn with, so whatever is in front of them hides them, and the HUD goes on top.

- `buh_line_3d <name> <x1> <y1> <z1> <x2> <y2> <z2> [options]`
- `buh_box_3d <name> <x> <y> <z> <width> <length> <height> [options]`: its edges, centred on x, y, z
- `buh_circle_3d <name> <x> <y> <z> <radius> [options]`: lying flat around x, y, z; `fill` fills it

Options: `color` and `gradient` (start to end; bottom to top on a box; middle to edge filled), `thickness` (pixels,
so they stay visible far away), `through_walls`, `on <object>` (the coordinates are then relative to that object, and
the shape follows it and goes when it does), `opacity`, `time` and `fade`. An object's position is its origin, or
its centre while it's in a seat or being held.

## Removing them

`buh_sprite_remove <name>` and `buh_sprite_clear` take shapes away too. A name is one thing: putting a shape up
replaces any sprite or shape by that name, on the HUD or in the world.
