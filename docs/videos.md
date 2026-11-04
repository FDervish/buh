# Videos

Bink videos, played with `binkw32.dll` like Halo's own. Paths are relative to the game folder, and `.bik` is added
when there's no extension.

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
