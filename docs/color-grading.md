# Colour grading

`buh_color_grading [true|false]` grades the world's colours the way games set their look; much of Halo: Reach's cool,
contrasty look is grading. It runs last before the HUD, after [SMAA](anti-aliasing.md) and the [bloom](bloom.md), so
the bloom is graded too and the HUD keeps its own colours. It's off by default. Turned on, it changes nothing until
there's something to grade.

## Adjustments

Without a value, each command says what it's set to.

- `buh_color_grading_temperature [amount]` (-1 to 1, 0 by default): warmer above 0, cooler below, keeping white as
  bright.
- `buh_color_grading_saturation [amount]` (0 to 2, 1 by default): 0 is gray, 2 twice as colourful.
- `buh_color_grading_contrast [amount]` (0.5 to 2, 1 by default): an S curve about the middle. Above 1, darks get
  darker and lights lighter; black and white stay where they are.
- `buh_color_grading_strength [amount]` (0 to 1, 1 by default): how much of the whole grade to use.

As a starting point for something like Reach's look, try a temperature of -0.2, a saturation of 0.9 and a contrast of
1.15.

## LUT images

For more than the adjustments, grade with a LUT (lookup table) image made in any image editor:

1. `python tools/neutral_lut.py lut.png` writes a neutral LUT, a strip 1024 by 32.
2. Take a screenshot in Halo and paste the strip into a corner of it.
3. Grade the whole picture in an image editor, with curves, colour balance, hue and saturation and the like. Use only
   changes that treat a colour the same wherever it is, so no blurs or vignettes.
4. Cut the strip back out, save it as a PNG of its own, and load it with `buh_color_grading_lut <file>`. The path is
   relative to the game folder, and `none` takes the LUT away.

A strip is N squares of N by N side by side, so N² by N: a square for each step of blue from left to right, with red
going across each square and green going down. That's the layout ReShade's LUT shader reads, so LUTs made for it work
here too. N can be from 2 to 64; 16 and 32 are usual. The adjustments go on top of the LUT.

## Each map its own

The commands work from `init.txt`, the console and Chimera Lua's `execute_script`, so a map's script can set its own
grade, as it can its bloom.

## Requirements

It needs pixel shader 3.0 and volume textures. It grades through a 32 by 32 by 32 table, rebuilt only when a setting
changes, so it's one lookup a pixel however much is set.
