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
  (message 132), resetting the new rows' settings only.

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
