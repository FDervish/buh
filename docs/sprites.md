# Sprites

Images (PNG, JPEG, BMP, GIF or TIFF, decoded by Windows), still or animated from a sprite sheet, drawn with the HUD
like [Bink videos](videos.md). Positions and sizes are fractions of the screen from the top left, and paths are
relative to the game folder.

- `buh_sprite <name> <file> <x> <y> <width> <height> [options]`, with any of these options:
    - `sheet <columns> <rows> [frames]`: the image is a grid of frames, left to right then top to bottom
    - `fps <fps>`: frame rate (10 by default), looping unless `once` is given
    - `time <seconds>`: remove it after this long
    - `fade <seconds>`: fade in, and out before `time` runs out
    - `opacity <0 to 1>`, `rotate <degrees>`
    - `move <x> <y> <seconds>`: glide to there, easing in and out
- `buh_sprite_remove <name>`, `buh_sprite_clear`

For example, a medal animated from a 4 by 2 sheet, shown for 3 seconds:

```
buh_sprite medal medals\double_kill.png 0.45 0.2 0.1 0.13 sheet 4 2 fps 24 once time 3 fade 0.3
```

Using a name again replaces that sprite; later sprites draw over earlier ones. Animations, fades and moves follow the
game's time like videos do.
