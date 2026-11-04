# Getting started

## Install

Copy `buh.dll` into the `mods` folder next to `strings.dll` (create it if needed). Get it from the
[releases](https://github.com/FDervish/buh/releases), or [build it](building.md).

buh needs [Chimera](https://github.com/SnowyMouse/chimera), which loads it. Each fix checks for what it patches first
and leaves the game alone if it isn't there.

!!! warning "Saved games"

    buh gives Halo a [bigger game state](game-state.md), so saved games and cores made without buh (or with a
    different layout) won't load.

## Running the commands

buh's commands work wherever Halo runs console commands:

- **The console.** They go into its history (the up and down keys) like Halo's own commands do, and tab completes
  them along with Halo's (and Chimera's).
- **`init.txt`** in the game folder, which Halo runs line by line when it starts. That's where to put the settings
  you always want, e.g. `buh_hud_skew 5`.
- **Chimera Lua scripts**, through `execute_script`, map scripts included, e.g.
  `execute_script('buh_subtitle "Get to the Pelican!" 3')` or
  `execute_script("buh_sprite medal medals\\double_kill.png 0.45 0.2 0.1 0.13 time 3 fade 0.3")`. Case and quotes
  are kept as written.

Chimera's Lua scripts also get [functions of buh's own](lua.md).
