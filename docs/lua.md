# Lua functions

buh adds functions of its own to Chimera's Lua scripts. Chimera exports its Lua, and makes each script's Lua with one
call that opens its libraries; buh points that call at itself and adds these once they're open, through Chimera's own
Lua. A Chimera build that doesn't export its Lua gets none of them (buh's console commands still work), and one that
has functions by these names keeps its own.

Scripts can also run any of buh's console commands with `execute_script` (see
[Running the commands](getting-started.md#running-the-commands)).

## Drawing

These draw for the frame they're called in, the way `draw_text` does: call them from the `preframe` callback every
frame something should show. (For shapes that stay up until they're removed, see [Shapes](shapes.md).)

- `draw_rectangle(left, top, width, height, a, r, g, b [, options])`
- `draw_line(x1, y1, x2, y2, thickness, a, r, g, b [, options])`
- `draw_quad(x1, y1, x2, y2, x3, y3, x4, y4, a, r, g, b [, options])`
- `draw_circle(x, y, radius, a, r, g, b [, options])`
- `draw_line_3d(x1, y1, z1, x2, y2, z2, thickness, a, r, g, b [, through_walls])`
- `draw_box_3d(x, y, z, width, length, height, thickness, a, r, g, b [, options])`
- `draw_circle_3d(x, y, z, radius, thickness, a, r, g, b [, options])`

On the screen they're in `draw_text`'s 640 by 480, whatever its shape, with radii and thicknesses in 480ths of its
height. Colours are alpha, red, green and blue from 0 to 1, as `draw_text` takes them. `options` is a table of any
of `outline` (a thickness, or `true`), `round` (a radius for a rectangle's corners), `arc` (`{ from, to }` for a
circle), `gradient` (`{ a, r, g, b }`), `rotate` (degrees), `through_walls` and `fill` (for a circle in the world):

```lua
draw_rectangle(20, 440, 200 * shield, 12, 0.9, 0.3, 0.65, 1, { round = 4, gradient = { 0.9, 0.6, 0.85, 1 } })
draw_circle(320, 240, 30, 1, 1, 1, 1, { outline = 3, arc = { 0, 360 * progress } })
draw_circle_3d(x, y, z, 0.5, 2, 0.8, 0.2, 1, 0.4)
```

Shapes in the world are hidden by whatever is in front of them, unless `through_walls` says otherwise.

## world_to_screen

`world_to_screen(x, y, z)` gives where a point in the world is on the screen, in the same 640 by 480, and its depth
(0 at the camera to 1 at the far plane), from the camera the last frame was drawn with; nil if it's behind it.

## collision_test

`collision_test(x, y, z, dx, dy, dz [, options])` traces a ray with Halo's own collision, the one its bullets and its
AI's line of sight use, from `x, y, z` to `x + dx, y + dy, z + dz`. It returns where it hit, the surface's normal
there, what it hit (`"level"`, `"water"` or `"object"`), the material as tags name it (`"metal_thin"`, `"dirt"` and so
on, or nil) and the object's ID (or nil); or nil if it hits nothing or no map is loaded. `options` is a table of any
of:

- `hit`: what it can hit, a name or a list of them: `everything` (the default: the level, water and every object),
  `sight` (what blocks Halo's AI's line of sight: the level, vehicles, scenery and machines, past see-through
  surfaces), `level`, `water`, `objects`, or objects of one type: `bipeds`, `vehicles`, `weapons`, `equipment`,
  `garbage`, `projectiles`, `scenery`, `machines`, `controls`, `light_fixtures`, `placeholders` or `sound_scenery`
- `ignore`: an object it goes through, usually the one looking, by ID or by address (as `get_object` and
  `get_dynamic_player` give it)
- `back_faces`: hit surfaces from behind too
- `invisible`: hit invisible surfaces too, like player clipping, which it otherwise goes through

```lua
-- From the camera at cx, cy, cz, looking along fx, fy, fz: what's there, up to 100 units away
local x, y, z, _, _, _, what, material = collision_test(cx, cy, cz, fx * 100, fy * 100, fz * 100, { ignore = get_dynamic_player() })
if x then draw_circle_3d(x, y, z, 0.3, 2, 1, 1, 0.5, 0) end
```

## structure_bsp_index

`structure_bsp_index()` gives which of the scenario's structure BSPs is loaded, as Halo's own `structure_bsp_index`
script function does (-1 for none), or nil if buh couldn't find where Halo keeps it: it finds that script function's
definition by its name and reads the global its evaluator reads. On a map of several BSPs, an encounter of a BSP that
isn't loaded is kept inactive, its actors standing still; so a script spawning actors wants an encounter of the loaded
BSP, or of none.

```lua
-- The same number Halo's console gives for structure_bsp_index
console_out("loaded: " .. tostring(structure_bsp_index()))
```

## HUD elements

The same changes to single [HUD elements](hud.md#elements) as the console commands make:

- `get_hud_elements()`: the elements drawn in the last frame, each a table of `id`, `kind` (`"static"`, `"meter"`,
  `"number"` or `"overlay"`), and `x`, `y`, `width` and `height`, where Halo put it (before any change made here)
- `set_hud_element(id, { dx = <x>, dy = <y>, rotate = <degrees>, scale = <scale>, opacity = <opacity>, hidden = <true> })`:
  true, or false if Halo hasn't drawn that element on this map
- `reset_hud_elements([id])`

```lua
-- Sink the meters as the shield goes down
for _, element in ipairs(get_hud_elements()) do
    if element.kind == "meter" then
        set_hud_element(element.id, { dy = 40 * (1 - shield) })
    end
end
```

## Damage

Everything Halo damages, it damages through one function, and buh stands in for it at each of its calls. Chimera's Lua
scripts can see each damage before Halo does it, change how much it does or stop it, and see what it did after:

- `set_predamage_callback(function)`: called before each damage, with a table of it. Change its `multiplier` to change
  how much it does, or return `false` to stop it.
- `set_damage_callback(function)`: called after, with the same table and what it did: `health` and `shield`, how much
  of each the object lost as fractions of its maximums, and `killed`, whether it died of it (or, if it's not a unit,
  was destroyed).

Either takes a function, the name of a global one (as Chimera's `set_callback` does), or nil for none. The table has:

- `object`: the object damaged, by ID
- `tag` and `path`: the damage effect tag's ID and path
- `causer` and `player`: the object that caused it, by ID, and its player's index from 0 to 15, or nil
- `multiplier`: Halo's own, and after, the callbacks'
- `scale`: from 0 to 1, how far from its damage effect's least damage to its most it does (a charge, or how near an
  explosion it was)
- `area`: whether it's an explosion's
- `instant`: whether it kills outright
- `proc`: how much it can set off what happens on a hit, as Risk of Rain 2's proc coefficient: 1 for Halo's own
  damage, and what `damage_object` was given for its own

```lua
-- Twice the damage from the first player's melee, and a message for each of their kills
set_predamage_callback(function(damage)
    if damage.player == 0 and damage.path and damage.path:find("melee") then
        damage.multiplier = damage.multiplier * 2
    end
end)
set_damage_callback(function(damage)
    if damage.player == 0 and damage.killed then
        hud_message("kill")
    end
end)
```

Each script's callback sees the multiplier the one before it left. Damage a callback does itself (with
`execute_script`, say) is done without callbacks. Damage is only worked out in single player or on the host, so
that's where it can be changed. Chimera's `chimera_block_damage` still works.

`damage_object(object, tag [, options])` does damage of a script's own, the way Halo's is done: through the same
function, past the callbacks, with Halo crediting whoever caused it, kill and all. `tag` is a damage effect, by ID
or by path. `options` is a table of any of:

- `multiplier` and `scale` (1 if not given), as the callbacks' table has them
- `causer`: the object that caused it, by ID; Halo credits its player, if a player controls it
- `player`: a player to credit instead, by their whole player table handle
- `proc` (1 if not given): what the callbacks see as its `proc`, for scripts whose on-hit effects set off more

It gives back true, then the health and shield the object lost and whether it died; or false and why (no such
object, or no such damage effect). From inside a callback, or from a coroutine, it's done without the callbacks.

```lua
-- Lightning from what the player hit to the next enemy: half a pistol bullet, credited to the player's biped
damage_object(next_enemy, "weapons\\pistol\\bullet", { multiplier = 0.5, causer = biped, proc = 0.2 })
```

## hs

`hs(script [, values] [, type])` runs one of Halo's scripts, as Chimera's `execute_script` does, and gives back what
it returns: a boolean or a number, `true` for a script that returns nothing, or nil and why it didn't run (Halo's
console says what was wrong with a script it couldn't compile).

```lua
local bsp = hs("(structure_bsp_index)")
local inside = hs("(volume_test_object $zone $me)", { zone = "arena", me = get_dynamic_player() })
hs("(object_set_scale $enemy 1.5 15)", { enemy = elite })
-- A passthrough gives nothing back unless asked
local alive = hs("(if (> (ai_living_count enemies) 0) true false)", "boolean")
```

- **What comes back** is of the type the script's outermost function returns. `begin`, `if`, `cond`, `set` and the
  scenario's own scripts give nothing back unless `type` asks for one: `"boolean"`, `"real"`, `"short"`, `"long"` (or
  `"void"`). A function's name alone is called, and one of Halo's own globals' names gives its value. Objects, lists,
  strings and Halo's sets of names (`game_difficulty_get`'s) can't come back: ask the script for what you want of
  them, like `(list_count ...)` or `(= (game_difficulty_get) hard)`.
- **`$name`** in the script is `values.name`: a string as a script's string, a boolean, or an object (by ID or by
  address, as `get_object` and `get_dynamic_player` give it) as a name buh gives it until the script has run. A
  script can only name the objects the scenario named; this way it can act on any object.
- **How:** Halo's console only runs scripts, so buh runs `(set <global> <script>)` with one of Halo's own globals of
  that type, reads it and puts it back; a value the script can't have left there says it didn't run. The names for
  objects are added past the scenario's own (`buh_object_1` and on), each with its object in Halo's list of objects
  by name, and taken away after. A scenario with 511 or 512 names has no room for them.
- **Limits:** Halo's console runs nothing of a script with a `;` in it, and takes 1023 characters. A script that
  sleeps comes back before it's done. A callback that a script `hs` runs sets off (a damage callback, say) can't run
  another.
