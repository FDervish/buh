# SMAA

Enhanced Subpixel Morphological Antialiasing, by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and
Diego Gutierrez. MIT licensed; see `LICENSE`.

From https://github.com/iryoku/smaa at commit 71c806a838bdd7d517df19192a20f0c61b3ca29d:

- `SMAA.hlsl` is upstream's, unmodified. `../smaa_shaders.hlsl` includes it for Direct3D 9 (`SMAA_HLSL_3`).
- `area_tex.bin` and `search_tex.bin` are the pixels of upstream's `Textures/AreaTex.h` (160 x 560, two bytes a pixel)
  and `Textures/SearchTex.h` (64 x 16, one byte a pixel). buh loads them as A8L8 and L8, which the shader's
  `SMAA_HLSL_3` texture selection expects.
