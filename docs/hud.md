# HUD

## Skew

`buh_hud_skew [degrees]` tilts each side of the HUD like a curved visor, so the inner edges dip (or rise, if
negative). Elements near the middle, like the crosshair, full screen overlays and navpoints stay as they are. Each
side is sheared as a whole, so things like ammo counters stay lined up. 0 (off) by default; around 4 to 6 looks right.

It works with Chimera's widescreen fix on or off.

## Elements

Each element of the HUD that the map's HUD tags define (each static element, meter and number, and each of a
weapon's overlays and crosshairs) can be moved, turned, scaled, faded or hidden on its own. An element's ID is where
its definition is in the map's tag data, so it stays the same while the map is loaded: switch away from a weapon and
back, and its elements' changes come back with it. Places and moves are in the HUD's 640 by 480, as for buh's
[drawing functions](lua.md#drawing); turning and scaling are about the middle of the whole element (all of a number's
digits, say). Changes last until they're reset or another map loads.

- `buh_hud_elements`: list the elements drawn in the last frame, with their IDs, kinds and places
- `buh_hud_element <id> [offset <dx> <dy>] [rotate <degrees>] [scale <scale>] [opacity <opacity>] [hide]`: draw one
  this way from now on, replacing any change made to it before
- `buh_hud_element_reset [id]`: draw one the way Halo does again, or all of them

For example, `buh_hud_elements` might list `0x4045A1C0 meter   at 20, 440, 200 by 8`, and then
`buh_hud_element 0x4045A1C0 offset 0 -60 rotate 10` lifts that meter and tilts it.

Lua scripts can do the same with [`get_hud_elements`, `set_hud_element` and `reset_hud_elements`](lua.md#hud-elements).

The motion sensor, navpoints, HUD messages and HUD text aren't HUD elements here. It works with Chimera's widescreen
fix on or off, and with `buh_hud_skew`.
