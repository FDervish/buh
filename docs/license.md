# License

GPL-3.0-only. Some of buh's code and assets are derived from other GPL-3.0-only projects:

- the text drawing, from [Chimera](https://github.com/SnowyMouse/chimera);
- the region permutation limit, the fog reset and texture preloading, from
  [Balltze](https://github.com/MangoFizz/balltze);
- the loading screen's shader and picture, from Balltze, the shader from SnowyMouse's
  [GLSL original](https://gist.github.com/SnowyMouse/eb7558d2e036e4becbc764501d99a703).

The loading screen's tone is Xbox Halo's own, from its loading screen.

[SoLoud](https://github.com/jarikomppa/soloud) 20200207 is in `ext/soloud`, under the zlib/libpng license
(`ext/soloud/LICENSE`). Its WASAPI backend is altered so surround devices get sound (the top of
`soloud_wasapi.cpp` says how); the rest is unmodified.

`tools/definitions` holds [Invader](https://github.com/SnowyMouse/invader)'s tag definitions, GPL-3.0.

[SMAA](https://github.com/iryoku/smaa)'s shader and lookup textures are in `src/post/smaa`, unmodified, under the MIT
license (`src/post/smaa/LICENSE`).
