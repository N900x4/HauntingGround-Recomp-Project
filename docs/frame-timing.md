# Frame and update timing

First target of the targeted decompilation (roadmap). Measured 2026-10-15 in stage 1 (`ST_02A`), New Game route,
headless, by diffing per-function call counts (`funcs_entered.txt`) between runs stopped at VBlank 3000 and 3600,
and tracing callers with `HG_TRACE`.

## Result: the game runs at 30 fps

- 220 functions are called exactly 300 times in 600 VBlanks (30 Hz). These are the per-frame game logic, animation
  and frame building.
- 112 functions are called exactly 600 times (60 Hz). These are the VBlank interrupt handlers, pad and sound
  housekeeping, and the VBlank wait loop.
- The display registers (`PMODE`, `DISPFB2`, `DISPLAY2`, …) are rewritten every VBlank, always with the same
  display buffer (`DISPFB2` = 0x1400). Each 30 Hz frame is drawn off screen and copied into it by the effect
  passes, so every picture is shown for two VBlanks.

So game updates and rendering are both 30 Hz. 60 fps cannot come from uncapping the frame rate: the logic, timers
and animation advance one step per frame, so the game would run at double speed (the Dark Cloud 2 recompilation hit
exactly this). It needs frame interpolation: an extra frame drawn between two game states (roadmap phase 6).

## The frame loop

| Address | Called | What it is |
|---|---|---|
| `0x1beef0` | 30 Hz | Frame driver (main loop body). It calls `0x1b86a0` once per frame (from 0x1bef98) and does the frame's work. |
| `0x1b86a0` | 30 Hz | Start of the frame. Waits for one VBlank (`0x10d200`, call at 0x1b8738) and calls `0x10c9a0` (at 0x1b86cc). |
| `0x1b8750` | 30 Hz | End of the frame. Waits for the second VBlank (`0x10d200`, call at 0x1b877c). |
| `0x10d200` | 60 Hz | Waits for a VBlank: polls the GS CSR through `0x10c8b8` (called at 0x10d238 and 0x10d248). |

Two VBlank waits are hard-coded per frame (two separate calls), so unlike Dark Cloud 2 there is no frame-rate
variable to change.

## Next steps

- Decompile `0x1beef0`, `0x1b86a0` and `0x1b8750` into named C. Each rewrite must keep the recorded sessions
  replaying to identical frames.
- Find what the frame driver updates (actors, camera, animation) and which state the renderer reads. Frame
  interpolation needs the previous and current state of each object.
- Check the other game modes (menus, movies, the House of Truth) for the same 30 Hz structure.
