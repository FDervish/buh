# Maps and models

## Maps

- **Texture preloading:** `buh_preload_textures [true|false]` loads every texture a map uses while it loads, instead
  of when each first shows up, which can hitch. Off by default. Halo's texture cache still has a fixed size, so this
  helps most on maps whose textures fit in it.
- **Fog:** the planar fog Halo is rendering with is cleared before each map loads, so the previous map's fog plane
  can't show in the next one.
- **Out of memory:** if Halo can't allocate a buffer to read a map into, buh says so and quits, instead of crashing
  reading into nothing.

## Models

Picking a model region's permutation copies every candidate into a 32 slot buffer without checking how many there
are, so regions with more than 32 permutations overrun it. buh makes room for 256, the most a permutation index can
address. More than 64 is untested.
