# Loading screen

`buh_loading_screen [true|false]` animates Halo's loading screen: a light sweeps across a blue Halo ring in place of
its picture, the way the Xbox version looks. It crosses once, from when the loading screen comes up to when it's gone,
so the longer the loading screen, the slower it goes. How long a map will take to load can't be known, so while one
loads the light slows down the longer it takes, keeping some of the way for after. It keeps moving while maps load,
since they load on another thread, and the window keeps responding. Halo's messages (connecting to a server and so
on) and its fades stay as they are. The main menu's map loads as it always did.

Singleplayer maps always get it, even when Halo wouldn't put its loading screen up (`chimera_block_loading_screen`
stops it, for one), so the map never shows before its scripts have started, the way it can for a frame or two when
nothing covers it. Halo keeps the loading screen up for a second after the map loads while the game runs underneath,
then fades into the game over 0.75 s. `chimera_block_loading_screen` still blocks it when joining a server.

## How long it stays up

`buh_loading_screen_minimum [seconds]` (0 to 60, 0 by default) keeps a singleplayer map's loading screen up at least
that long, counted from when it appeared, for when maps load faster than you'd see it. The map is held back until then,
so the game and its cutscenes don't start behind it; it moves meanwhile, even without the `init.txt` line. Halo then
fades into the game as usual. A server's game isn't held. Put it in `init.txt` too to keep it.

## The tone

It comes with Xbox Halo's loading screen tone: a short hum looped in four voices, pitched to a root, a fifth, an
octave and a ninth, which come in one after another and fade out as the loading screen goes on, the way they did while
Xbox Halo copied a map off the disc. Maps load in a moment on PC, so it follows the loading screen's time instead, along
with the light; `buh_loading_screen_minimum` gives it more room.

`buh_loading_screen_tone [volume]` sets how loud it is (1, as loud as on the Xbox, by default; 0 turns it off), and
`buh_audio_volume` applies to it too.

## Trying it

`buh_loading_screen_test [seconds]` puts it up over whatever is on screen for that long (6 by default, up to 60),
fading out over the last 0.75 s the way it does after a map. It works in singleplayer and the menus, and shows Halo's
own loading screen while `buh_loading_screen` is false.

## Turning it on

It's off by default, and turning it on takes `buh_loading_screen true` in `init.txt`. Drawing while another thread
loads needs Direct3D set up for it, before the console exists, so buh looks for that line when Halo starts and only
then asks Direct3D for it. Without the line, Direct3D and map loading are just as they were. With it, the command
turns it off and on again; turning it on when Halo started without the line only changes the picture, and the command
says a restart is needed for the rest.

It needs pixel shader 3.0, which any card from the last fifteen years or so has; without it, Halo's picture stays.
The shader, picture and tone are built into buh. `tools/compile_shader.py` rebuilds the shader's header from its
source.
