# Particles

- **Limits:** particles 1024 → 16384 (in buh's own memory), effects 256 → 2048, effect locations 512 → 4096, live
  particle systems 64 → 127, particle system particles 512 → 4096, decals 2048 → 4096, glows 8 → 64 and glow
  particles 512 → 1024. Without the [larger game state](game-state.md), effects only go to 726 and effect locations
  to 1280, and the rest stay stock.
- **Heavy load:** per-frame vertex and index blocks 1023 → 4096, triangles 32768 → 65536, sprite batches 384 → 2048.
  Past any of these Halo silently drops whole batches.
- **Culling:** particles are no longer deleted after 15 undrawn frames, hidden by stale PVS and cluster tests, refused
  at spawn outside the PVS, skipped below their size threshold, or capped at 11 screen-filling sprites per frame.
  One-shot effects created off screen are no longer deleted at birth.
- **Sorting:** particles within a batch are drawn back to front, so overlapping sprites stop swapping order.

Each fix checks its signatures first and leaves the game alone if they aren't found.

**Decals:** Chimera restores the stock-sized part of the decal vertex buffer after a checkpoint revert, so decals past
the first 2048 can show stale geometry after one.

**Interpolation:** Chimera interpolates particles itself. Released versions size that buffer at a fixed 1024, so
particles past the first 1024 update at the tick rate. A Chimera build that sizes it from the particle table
interpolates all of them, with no change needed here.
