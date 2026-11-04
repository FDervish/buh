# Tag packs

A tag pack is a set of tags (bipeds, weapons, vehicles and everything they use) that buh adds to maps as they load, so
they can be spawned and used without rebuilding the maps. A pack can also put its tags in place of a map's, to change
what every map has: other textures, sounds or weapons.

Adding tags is for singleplayer and local games: the server and every client would need the same packs, in the same
order. In a server's game, packs only go in place of what's only seen or heard (bitmaps, shaders, sounds, particles,
lights, decals, the HUD), which the server never sees; the map keeps its own of everything else, and buh says so.

## Making one

Build an ordinary map whose scenario lists what you want in its palettes, with all of it under a folder of its own
(e.g. `packs\covenant_armory\...`). Then:

```
python tools/tagpack.py covenant_armory.map -o covenant_armory.tagpack
```

That takes every tag the scenario's palettes use, and everything those use. `--tag <path>` takes particular tags (from
any map) instead, or every tag in a folder with a path ending in `\`, and `--info` lists what a pack holds. Tags the map
left in `bitmaps.map`, `sounds.map` or `loc.map` are copied into the pack, which needs those next to the map (or
`--maps <folder>`). The scenario, BSPs, globals, HUD globals and meters belong to whatever map the pack is used with,
so references to them are dropped. Needs Python 3.7 or later.

### Its own tags, and the rest

The tags a pack is made for, and everything in their folders, are its own: buh adds them to the map. The rest (the
stock effects, sounds, decals and so on they use) link to the map's own tags with the same paths when it has them, or
to an earlier pack's, so they aren't there twice; only those the map hasn't are added. `--own <folder>` sets the pack's
own folders instead (given more than once for more), and `--no-link` copies everything, as packs used to.

### In place of the map's tags

With `--override`, the pack's own tags go in place of the map's tags with the same paths. Everything in the map that
uses one gets the pack's, at the same index, so nothing else changes. Other textures for the cyborg in every map, say:

```
python tools/tagpack.py hd_cyborg.map -o hd_cyborg.tagpack --tag characters\cyborg\bitmaps\ --override
```

Its own tags that the map hasn't are added as usual.

## Using one

- `buh_tag_pack <file> [map...]`: add a pack (a path from the game folder) from the next map that loads, to every map
  or just the ones named (`bloodgulch`, or `maps\bloodgulch.map`). Put it in `init.txt` to always have it.
- `buh_tag_pack_remove <file>`, `buh_tag_packs`

Then spawn its tags by path, e.g. from a Chimera Lua script:

```lua
spawn_object("vehi", "packs\\covenant_armory\\ghost\\ghost", x, y, z)
```

They're ordinary tags from there: vehicles can be driven, weapons picked up, and so on.

## How it works

The tool does the hard part: it copies the tags into one block and lists every place in it that has to change when
the block moves (pointers, tag IDs, and offsets into bitmap, sound and model data), and which tags are its own and
which link. When a map loads, buh finds the map's tag (by class and path) for each one that links or goes in place of
one, copies each pack into the free part of tag space (above the map's tag data and its BSPs, in the room Chimera's
64 MiB of tag space leaves), applies that list, and puts the packs' added tags after the map's own, so the map's tags
keep their indices. If a map leaves too little room there, the packs go in memory of their own instead, and buh says
so in the console; it's kept for the next map that needs it. Pack models are added to the buffer Halo builds models
from, and bitmap and sound data is read from the pack file as Halo needs it.

Saved games and checkpoints remember their packs. Halo checks a saved game against the map's checksum; buh gives it
one that takes in the map's packs, in order, so a game saved with other packs (or none) is refused rather than loaded
wrong. A map without packs keeps its own, so games saved before still load. Sandboxed map scripts can't read pack tags'
data with `read_*` (it's outside Chimera's sandbox), but can still spawn them. The ui map gets no packs. Packs made
before linking (version 1) still work, with every tag added.
