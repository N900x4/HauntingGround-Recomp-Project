# The Options menu (roadmap step 3.0)

Research for the in-game settings (roadmap phase 3). The user's decision (Q6): the new settings are **extra rows in the
game's own Options menu**, drawn in the game's style and driven by its input handling. This document records how the
Options menu works, how the new rows hook in, and what that means for recorded sessions. Addresses are SLUS_210.75
(NTSC-U). Found 2026-10-15 with the frame-loop method (`docs/frame-timing.md`): call counts of a run idling on the
main menu against a run idling in Options (`funcs_entered.txt`), then `HG_TRACE` on the candidates, then reading the
code (a small disassembler over `recomp/r5900.py`), then experiments with hooks that draw.

## 1. What the screen is

Main menu (NEW GAME / LOAD GAME / OPTIONS) -> Options. The page:

```
                 Options
 [pad]  CONTROLLER TYPE              TYPE A
        (o): Search          |  (>): Pause
        (^): Throw/Set       |  (=): Sub-screen
        ([]): Kick           |  L1: Crouch
        (x): Dash            |  R1: Backstep
 [~~]   Vibration                      On
 [<)]   Sound                        Stereo
 [))]   Volume              Min |--------|Max
 [+]    Screen Position          X : 0   Y : 0
 (x):Select  (^):Back  ([]):Reset Defaults
```

Five selectable rows (Controller type, Vibration, Sound, Volume, Screen position); Cross on a row enters an edit
state (left/right change the value, blinking arrows around it, Cross keeps it, Triangle restores it); Square opens
"Reset Defaults? YES NO". The same class runs the Options tab of the in-game sub-screen (Special / Items / File / Map
/ Comment / Options), with L1/R1 switching tabs.

## 2. The code

The menu is a C++ object (two instances: the title's at `0x8fcec0`, inside the title object `0x888440`; the
sub-screen's at `0x180f680`). Methods are reached through member-function pointers (12 bytes: delta, virtual index or
-1, function), called by the helper `0x100b40`; `0x100bd0` tests one for null.

| Address | What |
|---|---|
| `0x3999c0` | per frame: calls the state at `obj+0x16fc` (pmf), counts frames at `obj+0xa8de0` |
| `0x399a10` | open (init): clears the fields, **row := 0** |
| `0x397d60` | top state: runs the sub-state at `obj+0x1708` (pmf); handles the exit flag `obj+0xa8de4` |
| **`0x393bd0`** | **sub-state "choose a row"**: input, then draws the page (`0x391450`) and the footer (message 130) |
| `0x393880`, `0x393480`, `0x3930c0`, `0x392b50`, `0x392670` | edit states of rows 0-4 (table of pmfs at `0x44b408`) |
| `0x392360` | Reset Defaults (the YES/NO dialog object at `obj+0x97764`: `0x384a90` opens it with a message, `0x384bc0` runs it) |
| **`0x391450(obj, editing)`** | **draws the whole page**: panel, labels, values, icons, cursor |

**Rows are hard-coded.** `0x393bd0` keeps the row in one byte, `obj+0xa8c56`, wraps it at 5 (`slti 5`) and dispatches
Cross through a `switch` on rows 0-4; `0x391450` draws every row at fixed coordinates; the cursor's y comes from the
5-entry table `0x44b558` (84, 244, 276, 308, 340). There is no item table to extend. The row byte is read or written
only by `0x391450`, `0x393bd0` and the init `0x399a10` (all 16 accesses in the executable), so values 5 and up are
free for new rows as long as those two functions are hooked.

**Input**: the menu reads its own edge word `0x47e36c` (pressed this frame; held in `0x47e364`), built by the game
from the controller type's mapping (`0x37e320` sets the confirm/cancel bits). Bits: 0 up, 1 right, 2 down, 3 left,
4 confirm (Cross), 5 back (Triangle), 6/7 tab switch (sub-screen only), 9 reset (Square).

**Sound effects**: `(*0x44e560)->vtbl[5](snd, id, 5)`: 42 cursor, 43 confirm, 44 cancel, 135 tab, 149 leave.

**Drawing** (all callable from host code; they append GIF packets to the frame's display list):

| Function | Use |
|---|---|
| `0x3858e0(obj, piece, 128, 48)` | a panel piece: table `0x44c160`, 7 u16 per piece = (texture 0-7, u, v, w, h, x, y). The page uses the set at `0x44c120[obj+0xa8c66 & 0x7f]`: title (10) = pieces 0x12 (the "Options" header, texture 7) + 0x13 (texture 6: the CONTROLLER TYPE bar, the mapping area with its divider) + 0x0b (texture 7: the four row bars, the volume slider line); sub-screen (3) = 0x0a (texture 6 with the tabs) + 0x0b. The row bars, separators, divider and slider line are part of these pictures; the footer and every text are not. |
| `0x3854c0(obj, x, y, sprite, alpha, 0)` | a sprite from table `0x44c280` (9 u16 each): 16-20 the row icons (pad, waves, speaker, volume, cross-hair), 25 the volume knob, 26/27 the edit arrows, 28 the yellow cursor frame (48 x 48 around a 32 x 32 icon at x 32) |
| `0x384b00(text, id)` | message text `id` (bank tables at `0x1991ec0`) |
| `0x384650(text, x, y, colour, str, 128, 48, 16, [sp] 21)` | draws `str` (a guest pointer) with the text object `text = obj+0x97868`; colour 128 normal, 130 the yellow of a row being edited; 16 = size |
| `0x382970(text, id, size)` / `0x382730(text, {str, 0})` | width of a message / of a string |

Text is ASCII with escapes: `0x01` new line, `0x03 c g 0x04` a button glyph `g` in colour `c` (glyphs 0xc0 Circle,
0xc1 Triangle, 0xc2 Square, 0xc3 Cross, 0xc4 L1, 0xc5 L2, 0xc6 R1, 0xc7 R2, 0xc8 Select, 0xc9 Start; colours 4-6 as
on the page). Coordinates are the game's 512 x 448 frame, shown 640 wide (x times 1.25).

**Where the values live**: the page edits copies in the object (`obj+0xa8c44..0xa8c4c`: sound mode, screen X/Y,
vibration, controller type, volume) and commits them to the system data at `*(0x44e978)+48..+59` (saved to the memory
card with the game's system file). The host settings never go there.

## 3. Decision: native rows, by hooking two functions

Native rows are practical and safe. The design (implemented in `recomp/game/options.c`):

- **Hooks** (`set_hook`) on `0x393bd0` (choose a row) and `0x391450` (draw the page). While the row byte is 0-4 both
  run the original code untouched, except one case in `0x393bd0`: Down on Screen Position, or Up on Controller Type,
  moves to the new rows (row byte 5 and up) instead of wrapping. While the row byte is 5 or more, the hooks draw and
  drive the new rows themselves (the game's own functions are called for every picture and sound); Triangle (leave)
  and the sub-screen's tab switch are passed to the original code, which handles them the same way on any row.
- **Drawing in the game's style**: the original panel is drawn, the parts that belong to page 1 (the mapping area's
  divider, the volume slider line) are covered with parchment cut from the same panel pictures, and row bars are
  copied from an existing row; then the game's own text, icon, arrow and cursor functions draw the rows. Pieces at
  arbitrary rectangles are drawn by writing one entry of the piece table, calling `0x3858e0` and restoring the entry
  within the hook (the packet is built during the call), so game memory is unchanged afterwards.
- **No guest memory of our own**: strings and argument blocks go in a frame below the guest stack pointer for the
  duration of the hook (`recomp/game/guest.h`).
- **Edit model** as in the original: Cross on a row starts editing (yellow label, blinking arrows), left/right change
  the value, Cross applies it (sound 43), Triangle restores it (sound 44). Square: the game's own Reset Defaults dialog
  (message 132, NO preselected as in the game), resetting only the settings of the page it is opened on.

## 4. Determinism and the recorded sessions

- **The references never reach the hooked functions.** New Game to VB 4000 and LOAD GAME to the Castle Suite never
  open Options. The user's recorded session (`recordings/20261014-200745`, 20,600 VBlanks) runs both Options objects'
  frame functions (`0x3999c0`/`0x397d60`, the sub-screen being opened) but never `0x393bd0` or `0x391450`
  (`funcs_entered` of a full replay): the page is never shown. The hooks cannot change these runs.
- **A recording that uses only the original rows replays identically** (the original code runs, instruction for
  instruction). One behaviour changes: Down past Screen Position (or Up past Controller Type) used to wrap to the other
  end of the page and now enters the new rows. A recording that wrapped there will diverge from that frame on. None
  exists today.
- **Rows are drawn from host settings**, so a frame of the new rows depends on them (the value strings, the controller
  names). A replay therefore applies the settings recorded in its header (`HG_PADPLAY`), and everything that comes
  from outside the pad (a captured key or button, the type of controller that names the buttons) is written to the
  recording as an event and taken from there on replay. Environment variables still override (test scripts), at the
  price of different value strings on the new rows if a recording shows them.
- On the new rows our C code runs instead of the game's, so emulated time per frame differs from what the game's code
  would take; it is the same in every run with the same input, which is what replays need.

## 5. What was built (phase 3.1 and 3.2, 2026-10-15)

### The rows

Down from Screen Position (or Up from Controller Type) turns the page: the same panel (the title's "Options" header,
or the sub-screen's tabs) with ten rows (HD textures added 2026-10-26, Aspect ratio 2026-10-29, Upscaling 2026-10-31: 28 pixels apart since, on parchment of
our own with a bar per row, so the note still fits above the footer), the game's icons, bars, cursor, arrows, colours,
sounds and footer:

```
 [+]  Renderer          GPU (Vulkan)          Software | GPU (Vulkan); takes effect at the next start
 [+]  Resolution        x5  3200 x 2240       Native, x2 ... x6, Auto (this display); GPU renderer only
 [~]  Frame rate        60 fps                30 | 60 | 120 (120 only at native resolution); GPU renderer only
 [+]  Display           Window                Window | Full screen
 [+]  Aspect ratio      4:3 (original)        4:3 (original) | Widescreen (the display's shape, up to 2.4:1; at once)
 [+]  Upscaling         FSR (sharp)           FSR (sharp) | Bilinear (soft): how the picture is enlarged to the window (at once)
 [~]  V-Sync            On (smooth)           On (smooth) | Exact 59.94 Hz | Off (may tear)
 [+]  HD textures       On                    On | Off ("Not installed" without a textures folder; GPU renderer only)
 [pad] Keyboard         Change                -> the keyboard page
 [pad] Controller       PlayStation           -> the controller page (the value: the kind connected)
      one note: the renderer changing at the next start, or the software renderer running (resolution, frame
      rate and HD need the GPU), or HD at native resolution; otherwise no note (the 60 fps delay is in the README).
 (x):Select  (^):Back  ([]):Reset Defaults
```

Down from Controller goes round to Controller Type; Up from Renderer back to Screen Position. Cross on a value row
starts editing it; Left/Right change it; Cross applies it (the renderer at the next start, everything else at once),
Triangle keeps the old value. Choosing a resolution other than native while at 120 fps drops to 60 fps (logged).

**The mapping pages** (Cross on Keyboard or Controller): a heading row (KEYBOARD, or CONTROLLER with the kind
connected) and two columns over the game's two-column area, ten entries each: the face buttons with the game's own
glyphs, L1/R1/L2/R2/L3/R3, the D-pad, Start, Select, and for the keyboard the left stick's four directions. Right of
each the key (SDL key names) or the controller button: PlayStation names (Cross, Circle, L1, R2, Options, Create ...)
when SDL reports a PS3/PS4/PS5 controller, Xbox names (A, B, LB, RT, Menu, View ...) otherwise. Since phase 4
(`docs/controller.md`) the controller page also has two value entries below Select, **Vibration** (Off / Low /
Medium / High) and **Light bar** (Amber / Red / Blue / White / Off), edited like the display page's rows, and the
kind shown is "DualSense" for a PS5 controller (Create, Mute) or "PlayStation" for PS3/PS4 (Share). Up/Down move within a
column, Left/Right between columns, Cross waits for a key or button ("Press a key" blinking; Esc cancels), Triangle
goes back, Square resets the page to the defaults (the game's dialog). A key or button already used by another of
the controller's buttons moves to the one that had the new one (a swap), so two buttons never share one key. After a
capture the page takes no input until every button is up (the captured key may now stand for a menu button).

### How it is drawn

`recomp/game/options.c` replays the start of `0x391450` to draw the panel (the same set of pieces, chosen the same
way), then: the "Sound" row's strip (separator line and bar, picture 7) copied into the two-column area at the new
rows' heights; plain parchment (the two-column area left and right of its divider, picture 6) over the two last rows,
where the notes go; for the mapping pages the two-column area (with its divider) copied down over the old row bars.
Text, icons (16-20), arrows (26/27), cursor (28) and the dialog are the game's functions. Nothing is left in game
memory: rectangles go through a borrowed piece-table entry that is put back within the call, strings live on the
guest stack below the hook's frame (`recomp/game/guest.h`). The footer of the display page is the game's own message
130; the mapping pages draw "(x):Change (^):Back ([]):Reset Defaults" with the game's glyph escapes.

## 6. Settings (`recomp/settings.c`, `settings.h`)

One typed structure for the host-facing options: renderer (and the one chosen for the next start), resolution (1-8 or
auto), frame rate, full screen, vsync, window size at start, the keyboard map (20 entries) and the controller map
(16). Precedence, highest first: environment variables (`HG_GSVK`, `HG_GSVK_SCALE`, `HG_GSVK_INTERP`, `HG_VSYNC`,
`HG_SCALE`, `HG_FULLSCREEN`, with the meanings they always had), the header of a replayed recording, `hg.ini`, the
defaults. **Without `hg.ini` the defaults are exactly the behaviour before settings existed** (software GS, native,
30 fps, window x2, mailbox, the old key and button tables). One log line at start lists the effective settings, the
file and the environment variables in use:

```
settings: renderer gpu, resolution x5, 60 fps, window, vsync mailbox, window x2 (file: ...\recomp\hg.ini, none; environment: HG_GSVK)
```

`hg.ini` lives next to the executable (`HG_INI=path` for another file, `HG_INI=off` for none), is written whenever a
row is applied (never while a recording is replayed), and is plain `key = value` (comments start with `#`):
`renderer`, `resolution`, `fps`, `fullscreen`, `vsync`, `window_scale`, `kb.<button>` (SDL key names, `none`),
`pad.<button>` (`a b x y back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright
misc1 paddle1-4 touchpad lefttrigger righttrigger none`). Button names: `select l3 r3 start up right down left l2 r2 l1
r1 triangle circle cross square`, and for the keyboard `ls_up ls_down ls_left ls_right`. Phase 4 added `vibration`
(off/low/medium/high), `lightbar` (amber/red/blue/white/off or `RRGGBB`), `deadzone` (percent) and `touchpad_select`
(`docs/controller.md`). A setting the environment
overrides can still be changed in the menu (it applies at once and is saved; the log notes that the variable wins at
the next start).

The settings are host state only. The game's own options (vibration, sound, volume, screen position, controller type)
stay in its system data and its memory-card save; nothing of ours is written there.

## 7. Live changes

| Setting | Applied | How |
|---|---|---|
| Resolution, frame rate | at the next VBlank | `gs_vblank` passes the wanted factor and interpolation with each VBlank marker; the GS thread (`vk_reconfigure` in `gs.c`) finishes the GPU's work, reads back what it drew into `vm`, makes local memory again for the new number of copies (`gsvk_resize`: buffers and descriptor), uploads it all from `vm`, and restarts frame interpolation (two whole frames before the first in-between picture). Copy 0 holds the same memory before and after. 2-27 ms. |
| Presenting rate | when the GS switches | the UI thread follows `gs_interp_now()`: 59.94 or 119.88 presents a second; the picture queue's depth follows (2 or 5) |
| Full screen | at once | the UI thread compares the setting with the window |
| Aspect ratio | at once | the camera hook and the 2D squeeze read `settings_wide_factor()` each frame; the window blit uses `settings_display_aspect()`; a window (not full screen) takes the new shape at its height |
| Upscaling | at once | the UI thread reads `settings_ui_upscaler()` at every present: FSR 1 (gsvk.c) when the picture is smaller than the window's rectangle, else the blit |
| V-Sync | at the next present | `gsvk_present_mode()`: the swapchain is made again in the new mode |
| Keyboard, controller | at once | the UI thread copies the maps when the settings' generation changes |
| Renderer | next start | the software GS and the GPU renderer are chosen when the game starts |

## 8. Recordings and replays

A recording's header gets `# set key=value` lines with every setting in effect, and the changes that do not come from
the pad are written as `set VBLANK key=value` lines at the VBlank the game thread applied them: F11 (full screen), a
captured key or button (`kb.cross=V`, `pad.circle=y`, `capture=cancel`), the kind of controller that names the buttons
(`padtype=0/1/2`, also when one is plugged in or out). A replay applies the header (unless an environment variable
sets the same thing: then it warns), applies the events at their VBlanks, ignores live ones, and saves nothing. While
replaying, F11 changes the window only. `HG_HOSTEV="VB:key=value,..."` makes such host changes from a script (tests).

## 9. Verification (gates, 2026-10-15)

`tools/verify/optionstest.sh TAG [env...]`: the scripted run described in the script (every new row changed once,
both mapping pages, both kinds of Reset Defaults, 120 fps, round trips between the game's rows and ours, leaving),
frames hashed every 5 VBlanks, the settings changes and GS switches listed.

| Check | Result |
|---|---|
| G1/G1g `hashcheck.sh` (software, GPU, +60, x5) | 702/702 identical to `hashA` each (no `hg.ini`: behaviour as before) |
| G2 the same, Castle Suite | 742/742 each |
| G3 `soak.sh` with `HG_VU1CHECK=1` | 1,005/1,005 identical to `soak.md5`; 17.6 M VU1 runs, 0 mismatches, 0 handovers; GPU x5 + 60: 1,005/1,005 |
| G5 `gpucheck.sh batch` | 0 batches differ (menus, stage 1, Castle Suite, `ST_020`) |
| Options test, two runs / recorded / replayed / in a muted window | 672/672 frame hashes identical in all five; 12 setting changes; GS switches to x3, 60 fps, native and 120 fps took 2-27 ms |
| G7 window, Castle Suite (`wintest.sh`, `pacing.py`) | x5 30 fps: median 16.68 ms, x5 + 60: 16.74 (p5-p95 16.41-16.94), native 120: 8.39; each 1.00x, 0 repeats, 0 out of order |
| G12 clean build | 76 s |

## 10. Limits and open points

- The renderer cannot change while the game runs (software GS <-> GPU): the row records the choice for the next start.
- "Auto" picks the smallest factor whose height fills the display the window is on (at most x6); headless it is native.
- The mapping pages map one key and one controller input per button; the analog sticks are not remappable (a
  controller's sticks stay sticks; the keyboard's left stick is four keys).
- Keys are shown with SDL's English names; the game's font has no glyphs for the D-pad, L3/R3 or keyboard keys.
- A recording that wraps the game's cursor round the page (Down from Screen Position) behaves differently now.
- `HG_OPTIONS_ROWS=0` turns the extra rows off (the game's page exactly as it was).

## 11. Manual checklist for the user's playtest

Start with `play.bat` (a first start without `hg.ini` behaves as before: the software renderer unless `set
HG_GSVK=1`). Then:

1. Main menu, OPTIONS: the page looks as before. Down from Screen Position: the new page (Renderer ... Controller,
   the delay note). Up from Renderer goes back to Screen Position; Down from Controller goes round to Controller
   Type; Up from Controller Type comes to Controller.
2. Renderer: GPU (Vulkan), Cross; "The renderer changes at the next start." Leave, quit (Esc), start again: the log's
   first `settings:` line says `renderer gpu`, and `hg.ini` exists next to `hgboot.exe`.
3. Resolution: x5 (or Auto), Cross: the picture sharpens at once (F3: speed 100 %). Back to Native works too.
4. Frame rate: 60 fps, Cross: smoother at once. 120 fps is offered only at Native; with it, the F3 overlay should
   still say 100 %. Choosing x5 while at 120 drops to 60.
5. Display: Full screen / Window; F11 toggles too, and the row follows it.
6. V-Sync: On (smooth: locked to the display, the default since 2026-10-15 night), Exact 59.94 Hz (the old mailbox
   pacing), Off: look for tearing or stutter in each.
7. Keyboard: Cross on a button (e.g. Cross), press a key (e.g. Space): it shows at once and works in the menus. Esc
   cancels a "Press a key". A key already used moves to the button that had the new one. Square, Left (YES), Cross:
   the defaults return.
8. Controller (a DualSense or PS4 pad): the row says PlayStation and the page uses PlayStation names (an Xbox pad:
   Xbox names; none: "Not connected"). Remap one button, try it in the game, then Reset Defaults.
9. In the game: Select (sub-screen), L1 to the Options tab: the same new rows; L1/R1 still switch tabs from them;
   Triangle leaves.
10. Reset Defaults on the display page: resolution, frame rate, display and vsync to the defaults (not the renderer).
11. The game's own options (vibration, sound, volume, screen position, controller type) still change and save as
    before; `hg.ini` holds only ours.
12. `replay.bat` of the session shows the same pages and values.
