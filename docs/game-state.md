# Game state

Halo carves most of its arrays out of a 0x440000 byte game state at 0x40000000, which can't grow there because the
tag data starts right after it. buh moves it to a 0x640000 byte block right below it, at 0x3F9C0000, and teaches
checkpoints, saved games and cores the new size.

!!! warning

    Saved games and cores made without buh (or with a different layout) won't load.

If that can't be done, the game state stays where it is and only the [limits](particles.md) that fit its slack are
raised.

Chimera's Lua sandbox only lets map scripts read 0x40000000 to 0x41B00000, so sandboxed map scripts can't read the
moved game state (objects, players and so on).
