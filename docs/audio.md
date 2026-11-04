# Audio and subtitles

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
of, and checks what it writes first. Then build the map (or [tag pack](tag-packs.md)) as usual. Give sounds with
subtitles paths of their own: one with a stock path may be taken from `sounds.map` instead, which has none.

- `buh_subtitles [true|false]`: on by default.
- `buh_subtitle <text> [seconds]`: show one yourself (4 seconds by default).
- `buh_sound_length <sound tag path>`: how long each permutation of a sound in the loaded map is, counting the
  permutations long sounds are chained into.
