# buh

A [Chimera](https://github.com/SnowyMouse/chimera) mod for Halo: Combat Evolved. Chimera loads every DLL in the
game's `mods` folder after it has applied its own patches, and buh patches the game on top of that without touching
Chimera itself.

## Game state

Halo carves most of its arrays out of a 0x440000 byte game state at 0x40000000, which can't grow there because the
tag data starts right after it. buh moves it to a 0x640000 byte block right below it, at 0x3F9C0000, and teaches
checkpoints, saved games and cores the new size. **Saved games and cores made without buh (or with a different layout) won't load.**

If that can't be done, the game state stays where it is and only the limits that fit its slack are raised.

Chimera's Lua sandbox only lets map scripts read 0x40000000 to 0x41B00000, so sandboxed map scripts can't read the
moved game state (objects, players and so on).

## Particles

- **Limits:** particles 1024 → 16384 (in buh's own memory), effects 256 → 2048, effect locations 512 → 4096, live
  particle systems 64 → 127, particle system particles 512 → 4096, decals 2048 → 4096, glows 8 → 64 and glow
  particles 512 → 1024. Without the larger game state, effects only go to 726 and effect locations to 1280, and the
  rest stay stock.
- **Heavy load:** per-frame vertex and index blocks 1023 → 4096, triangles 32768 → 65536, sprite batches 384 → 2048.
  Past any of these Halo silently drops whole batches.
- **Culling:** particles are no longer deleted after 15 undrawn frames, hidden by stale PVS and cluster tests, refused
  at spawn outside the PVS, skipped below their size threshold, or capped at 11 screen-filling sprites per frame.
  One-shot effects created off screen are no longer deleted at birth.
- **Sorting:** particles within a batch are drawn back to front, so overlapping sprites stop swapping order.

Each fix checks its signatures first and leaves the game alone if they aren't found.

Decals: Chimera restores the stock-sized part of the decal vertex buffer after a checkpoint revert, so decals past the
first 2048 can show stale geometry after one.

Interpolation: Chimera interpolates particles itself. Released versions size that buffer at a fixed 1024, so particles
past the first 1024 update at the tick rate. A Chimera build that sizes it from the particle table interpolates all of
them, with no change needed here.

## Bink videos

Console commands, using `binkw32.dll` like Halo's own videos. Paths are relative to the game folder, and `.bik` is
added when there's no extension.

- `buh_bink_play <file> [loop] [pause]`: fullscreen, letterboxed on black.
- `buh_bink_pip <file> <x> <y> <width> <height> [loop] [pause]`: in a box, placed and sized as fractions of the
  screen (of the player's view in split screen) from the top left, e.g.
  `buh_bink_pip movies\cam 0.7 0.05 0.25 0.25 loop`.
- `buh_bink_stop`
- `buh_bink_volume [volume]`: 1 is normal.

`pause` pauses the game while the video plays, in singleplayer only. Without it the game keeps running.

The video is drawn right after the HUD, so menus and the console go over it, and it isn't drawn when the HUD isn't.
It follows the game: it only plays while it's being drawn and, in singleplayer, while the game isn't paused and the
console is closed, then carries on from where it was. One video plays at a time, and starting another replaces it.

## Sprites

Images (PNG, JPEG, BMP, GIF or TIFF, decoded by Windows), still or animated from a sprite sheet, drawn with the HUD
like Bink videos. Positions and sizes are fractions of the screen from the top left, and paths are relative to the
game folder.

- `buh_sprite <name> <file> <x> <y> <width> <height> [options]`, with any of these options:
  - `sheet <columns> <rows> [frames]`: the image is a grid of frames, left to right then top to bottom
  - `fps <fps>`: frame rate (10 by default), looping unless `once` is given
  - `time <seconds>`: remove it after this long
  - `fade <seconds>`: fade in, and out before `time` runs out
  - `opacity <0 to 1>`, `rotate <degrees>`
  - `move <x> <y> <seconds>`: glide to there, easing in and out

  e.g. `buh_sprite medal medals\double_kill.png 0.45 0.2 0.1 0.13 sheet 4 2 fps 24 once time 3 fade 0.3`. Using a
  name again replaces that sprite; later sprites draw over earlier ones.
- `buh_sprite_remove <name>`, `buh_sprite_clear`

Animations, fades and moves follow the game's time like videos do.

## Audio

Sound files (wav, mp3, flac or ogg), played with [SoLoud](https://solhsa.com/soloud/) rather than Halo's sound system.

- `buh_audio_play <name> <file> [loop] [volume]`: using a name again replaces that sound.
- `buh_audio_stop [name]`: all of them without a name.
- `buh_audio_volume [volume]`: for all of them, 1 is normal.

In singleplayer they pause while the game is paused or the console is open.

## Subtitles

Sound permutations have a subtitle data field Halo never uses. When a permutation that has text there plays, buh
shows it at the bottom of the screen for as long as the permutation lasts, in the font Halo uses for HUD messages.
Up to three show at once, newest at the bottom. A long sound is split into a chain of permutations ("pieces") when
it's built: text on its first piece stays up for the whole sound, or until a later piece with text of its own, so a
long line can also be captioned a piece at a time.

`tools/subtitles.py` puts the text into HEK `.sound` tags (UTF-16, which is what buh reads; `|n` starts a new line):

```
python tools/subtitles.py list tags\sound\dialog\marine\pelican.sound
python tools/subtitles.py set tags\sound\dialog\marine\pelican.sound "Get to the Pelican!"
python tools/subtitles.py set tags\sound\dialog\marine\pelican.sound "Move it!" --permutation take2
python tools/subtitles.py set tags\sound\dialog\cortana\long.sound "...and the rest." --permutation 0 --piece 2
python tools/subtitles.py clear tags\sound\dialog\marine\pelican.sound
```

Without `--permutation` (an index or a name) it changes every permutation. Setting a whole permutation puts the text
on its first piece and clears its others; `--piece` sets one piece. It only writes a tag it can account for every byte
of, and checks what it writes first. Then build the map (or tag pack) as usual. Give sounds with subtitles paths of
their own: one with a stock path may be taken from `sounds.map` instead, which has none.

- `buh_subtitles [true|false]`: on by default.
- `buh_subtitle <text> [seconds]`: show one yourself (4 seconds by default).
- `buh_sound_length <sound tag path>`: how long each permutation of a sound in the loaded map is, counting the
  permutations long sounds are chained into.

## HUD skew

- `buh_hud_skew [degrees]`: tilt each side of the HUD like a curved visor, so the inner edges dip (or rise, if
  negative). Elements near the middle, like the crosshair, full screen overlays and navpoints stay as they are. Each
  side is sheared as a whole, so things like ammo counters stay lined up. 0 (off) by default; around 4 to 6 looks right.

It works with Chimera's widescreen fix on or off.

## Maps

- **Texture preloading:** `buh_preload_textures [true|false]` loads every texture a map uses while it loads, instead
  of when each first shows up, which can hitch. Off by default. Halo's texture cache still has a fixed size, so this
  helps most on maps whose textures fit in it.
- **Fog:** the planar fog Halo is rendering with is cleared before each map loads, so the previous map's fog plane
  can't show in the next one.
- **Out of memory:** if Halo can't allocate a buffer to read a map into, buh says so and quits, instead of crashing
  reading into nothing.

## Loading screen

`buh_loading_screen [true|false]` animates Halo's loading screen: a light sweeps across a blue Halo ring in place of
its picture, the way the Xbox version looks. It keeps moving while maps load, since they load on
another thread, and the window keeps responding. Halo's messages (connecting to a server and so on) and its fades stay
as they are. The main menu's map loads as it always did.

Singleplayer maps always get it, even when Halo wouldn't put its loading screen up (`chimera_block_loading_screen`
stops it, for one), so the map never shows before its scripts have started, the way it can for a frame or two when
nothing covers it. Halo keeps the loading screen up for a second after the map loads while the game runs underneath,
then fades into the game over 0.75 s. `chimera_block_loading_screen` still blocks it when joining a server.

`buh_loading_screen_minimum [seconds]` (0 to 60, 0 by default) keeps a singleplayer map's loading screen up at least
that long, counted from when it appeared, for when maps load faster than you'd see it. The map is held back until then,
so the game and its cutscenes don't start behind it; it moves meanwhile, even without the `init.txt` line. Halo then
fades into the game as usual. A server's game isn't held. Put it in `init.txt` too to keep it.

`buh_loading_screen_test [seconds]` puts it up over whatever is on screen for that long (6 by default, up to 60),
fading out over the last 0.75 s the way it does after a map. It works in singleplayer and the menus, and shows Halo's
own loading screen while `buh_loading_screen` is false.

It's off by default, and turning it on takes `buh_loading_screen true` in `init.txt`. Drawing while another thread
loads needs Direct3D set up for it, before the console exists, so buh looks for that line when Halo starts and only
then asks Direct3D for it. Without the line, Direct3D and map loading are just as they were. With it, the command
turns it off and on again; turning it on when Halo started without the line only changes the picture, and the command
says a restart is needed for the rest.

It needs pixel shader 3.0, which any card from the last fifteen years or so has; without it, Halo's picture stays.
The shader and picture are built into buh. `tools/compile_shader.py` rebuilds the shader's header from its source.

## Models

Picking a model region's permutation copies every candidate into a 32 slot buffer without checking how many there
are, so regions with more than 32 permutations overrun it. buh makes room for 256, the most a permutation index can
address. More than 64 is untested.

## Tag packs

A tag pack is a set of tags (bipeds, weapons, vehicles and everything they use) that buh adds to every map that
loads, so they can be spawned and used without rebuilding the map. Singleplayer and local games only: the server and
every client would need the same packs, in the same order.

**Making one.** Build an ordinary map whose scenario lists what you want in its palettes, with all of it under a
folder of its own (e.g. `packs\covenant_armory\...`) so nothing in it has the same path as a tag in the maps you'll
use it with. Then:

```
python tools/tagpack.py covenant_armory.map -o covenant_armory.tagpack
```

That takes every tag the scenario's palettes use, and everything those use. `--tag <path>` takes particular tags (from
any map) instead, and `--info` lists what a pack holds. Tags the map left in `bitmaps.map`, `sounds.map` or `loc.map`
are copied into the pack, which needs those next to the map (or `--maps <folder>`). The scenario, BSPs, globals, HUD
globals and meters belong to whatever map the pack is used with, so references to them are dropped. Needs Python 3.7
or later.

**Using one.**

- `buh_tag_pack <file>`: add a pack (a path from the game folder), from the next map that loads. Put it in `init.txt`
  to always have it.
- `buh_tag_pack_remove <file>`, `buh_tag_packs`

Then spawn its tags by path, e.g. from a Chimera Lua script:
`spawn_object("vehi", "packs\\covenant_armory\\ghost\\ghost", x, y, z)`. They're ordinary tags from there: vehicles
can be driven, weapons picked up, and so on.

**How it works.** The tool does the hard part: it copies the tags into one block and lists every place in it that
has to change when the block moves (pointers, tag IDs, and offsets into bitmap, sound and model data). When a map
loads, buh copies each pack into the free part of tag space (above the map's tag data and its BSPs, in the room
Chimera's 64 MiB of tag space leaves), applies that list, and puts the pack's tags after the map's own, so the map's
tags keep their indices. If a map leaves too little room there, the packs go in memory of their own instead, and buh
says so in the console; it's kept for the next map that needs it. Pack models are added to the buffer Halo builds models from, and bitmap and sound data is
read from the pack file as Halo needs it.

Saved games and checkpoints only load with the same packs, added in the same order. Sandboxed map scripts can't read
pack tags' data with `read_*` (it's outside Chimera's sandbox), but can still spawn them. The ui map gets no packs.

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

## Install

Copy `buh.dll` into the `mods` folder next to `strings.dll` (create it if needed).

## Build

buh is a 32-bit DLL. With MSYS2, from a MINGW32 shell:

```
cmake -S . -B build -G Ninja
cmake --build build
```

Cross-compiling from Linux:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-w64-mingw32.cmake
cmake --build build
```

## License

GPL-3.0-only. Some of buh's code and assets are derived from other GPL-3.0-only projects:

- the text drawing, from [Chimera](https://github.com/SnowyMouse/chimera);
- the region permutation limit, the fog reset and texture preloading, from
  [Balltze](https://github.com/MangoFizz/balltze);
- the loading screen's shader and picture, from Balltze, the shader from SnowyMouse's
  [GLSL original](https://gist.github.com/SnowyMouse/eb7558d2e036e4becbc764501d99a703).

[SoLoud](https://github.com/jarikomppa/soloud) 20200207 is in `ext/soloud`, unmodified, under the zlib/libpng
license (`ext/soloud/LICENSE`).

`tools/definitions` holds [Invader](https://github.com/SnowyMouse/invader)'s tag definitions, GPL-3.0.
