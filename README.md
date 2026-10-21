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

GPL-3.0-only. Parts of buh are derived from Chimera.
