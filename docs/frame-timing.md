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

## Pacing the window (2026-10-15, night: smooth 60 fps)

The user's first playtest (x5 + 60 fps, full screen, 4K **120 Hz** monitor) found 60 fps "not smooth". Smooth 60 fps
on any fixed-refresh display (no G-Sync/VRR) is a 0.1 release requirement. Code: `recomp/host_sdl.c` ("pacing"),
`recomp/gsvk.c` (presenting), measured with `tools/verify/cadence.py`.

**Before.** The UI thread presented one picture every 1/59.94 s of its own clock into a *mailbox* swapchain, and the
game thread kept emulated time at 59.94 Hz of the same host clock. A mailbox picture is shown at the display's next
refresh, so on a 120 Hz screen a picture stayed up for 2 refreshes, or 1 or 3 whenever the clock's jitter crossed a
refresh. On a 60 Hz screen 59.94 against 60.00 repeats a picture about every 17 s.

**Display-locked (V-Sync "On (smooth)", now the default; `vsync = on`, `HG_VSYNC=1`).**
- The swapchain is FIFO and the UI thread presents at **every refresh**, then waits (`VK_KHR_present_wait`) until that
  present is on the screen. The display is the clock: the loop runs exactly once per refresh.
- The refresh period is measured from those times (each interval counted as a whole number of refreshes, the sum over
  10 s; the driver's own figure from `VK_EXT_present_timing` when it has one). When refresh / N is within 0.5 % of
  59.94 Hz (60, 120, 180, 240 Hz...; the user's Q14 decision), the window **locks**: a new picture is taken every N
  refreshes (N / 2 at 120 fps), the refreshes in between show it again, so **every picture is held exactly N
  refreshes**.
- **The game follows the display.** Each N refreshes is a tick; the game thread's VBlank `vb` is due when the tick
  count, extended to the present moment, reaches its anchor plus `vb` (`host_vblank`). Emulation then runs at refresh / N
  VBlanks a second (120.00 Hz: 60.00 instead of 59.94, 0.1 % fast). Ticks older than 100 ms (a minimized window, a
  new swapchain) hand back to the host clock; a lock re-anchors.
- **A phase controller** keeps the picture queue short: a picture that is not ready at its tick (it is then shown
  again) makes the game run a quarter VBlank earlier; two pictures ready at every take for 10 s, a quarter VBlank later.
- **Sound** is resampled on its way into the ring (`host_audio_push`: 32-tap Kaiser-windowed sinc, 256 phases), the
  step 1 + 5 % per second of sound the ring holds beyond its 120 ms cushion, at most +-0.6 %. The 0.1 % settles 20 ms
  above the cushion; the ring no longer drifts into drops or underruns (and the host clock's drift against the sound
  card is absorbed too). The log line `host audio:` shows the resampling.
- **Other rates** (144, 165 Hz): no whole multiple. The game keeps its 59.94 Hz host clock and each refresh shows the
  picture that clock says is due, so the holds are as even as the rate allows (144 Hz: 2, 3, 2, 3, 2...). Better
  would need a 120 Hz mode (an exclusive full-screen mode switch, not done) or VRR (not relied on).
- **The GPU side.** The window's blits run on the graphics queue family and the GS's work on an async compute family at
  a low global priority (`VK_EXT_global_priority`), so a present's blit is not held up behind long GS dispatches; the UI
  thread runs at high priority.
- `vsync = mailbox` (menu "Exact 59.94 Hz") is the old way: exact 59.94 Hz, its own clock. `off`: immediate.

**Measuring it** (`HG_PRESENTLOG=S:N` logs N presents from S seconds on): `PRESENT` (after the call), `SHOWN` (when
`vkWaitForPresentKHR` returned) and, with `VK_EXT_present_timing` (the RTX 5090 driver has it), `SCAN`: the time the
driver reports the present's first pixel went out to the screen, the ground truth. `cadence.py LOG` makes the
histogram of holds in refreshes from the `SCAN` lines (or `SHOWN`), the order, and the delay from the start of the
picture's VBlank on the game thread to the screen (`lat`). `HG_PACEDBG=1` logs every late present with the time spent in
each step.

**Results** (the user's recording `20261015-184308`, New Game through the field, x4 + 60 fps, full screen, the
4K 120 Hz monitor; scanout times):

| Mode | Pictures held 2 refreshes | 3 | 1 | Out of order |
|---|---|---|---|---|
| Display-locked, every refresh, one present ahead | 99.6-99.7 % (1,194-1,195 of 1,199) | 0.3-0.4 % | 0 | 0 |
| Display-locked, windowed (present-wait times) | 98-100 % | 0-2 % | 0 | 0 |

The remaining 1 in 300 is the driver: the present call sometimes blocks 8-12 ms, or a present reaches the screen two
refreshes later, also at native resolution with the GPU idle and with the layers of RTSS and OBS switched off
(`VK_LOADER_LAYERS_DISABLE=~implicit~`); presenting two or three refreshes ahead did not remove it. A picture presented
once with a target time (`VK_EXT_present_timing`, relative, `HG_PRESENT_TIMED=1`) is the next step; its first minute
showed 1 hold of 3 in 323. Overnight measurement stopped when the monitor went to sleep (with the display off no
scanout happens, and presents fall to one per 80-100 ms): the 20-minute soak and the windowed numbers wait for the
display.

## The 3-VBlank frames (2026-10-25)

Measured with `HG_FRAMEPROF=FILE` on the user's session `recordings/20261025-005118`, VB 3,000-8,000: 131 of 2,431
frames took 3 VBlanks (the picture held 50 ms, 1-2 times a second). An EE twice as fast (`HG_EE_MIPS=600`) changed
nothing, and DMA/VU1 take no emulated time (`sceGsSyncPath` in `frame_begin` never waits). In every long frame the
frame thread waited about one VBlank in `sceSifCallRpc` (0x2700e8) for SNDDRV's command 0x26 (client 0x19756c0, from
0x21fb70). SNDDRV's server thread answers after a semaphore signalled by its DelayThread tick. The emulated IOP ran a
whole VBlank ahead at each VBlank (`iop_vblank`), so a request made during the VBlank found the IOP at the end of
its slice: the tick, and the answer, came in the next slice, while the idle EE jumped to the next VBlank.

IOP sync (**the default since 2026-10-25**, the user's decision; `HG_IOPSYNC=0` is the old timing): the IOP's time follows the EE's (8 EE cycles per IOP cycle); an RPC first catches
the IOP up; while no EE thread can run, `idle_advance` runs the IOP to each of its events before the EE's next one
and delivers the answers. Result: 0 frames of 3 VBlanks (2,499 of 2 from VB 3,000 to 8,000; G2 route 51 -> 0),
audiocheck PASS. It changes the emulation's timing (G1 706 frames, G2 746, different from the references; all four
renderers still identical to each other). Made the default on 2026-10-25: `ref/hashA.md5` and `ref/cs.md5` re-frozen
(two runs identical, `HG_VU1CHECK=1` 0 mismatches), G10 pictures re-made. Recordings now carry the mode
(`iopsync N` in pad.txt); a recording without that line (every one before 2026-10-25) replays with the old timing, so
`ref/soak.md5` and the user's earlier sessions still replay exactly. `HG_IOPSYNC` overrides both.

**Mode 2 (the default since the night of 2026-10-25).** The first IOP sync (now mode 1) took 8 EE cycles per IOP
cycle, but the EE's emulated time counts `HG_EE_MIPS` (300 M) per second, not the console's 294.9 M: the IOP ran
1.7 % fast, and its sound with it, 814.2 samples per VBlank instead of 800.8. With the window the sound ring then
filled (the resampling's 0.6 % could not absorb it), chunks were dropped and movie sound ran ahead of the pictures,
12 ms more each second (playtest 6b: "cutscenes are still out of sync"). Mode 2 gives the IOP the console's clock
per VBlank of EE time: IOP cycles = EE cycles x (36,864,000 / 59.94) / `ee_vb_period` (`iop_kernel.c ee_iop`; the
inverse rounds up, so an IOP event's EE time never comes before it). 799.8 samples per VBlank headless (the boot
accounts for the rest); in playtest 7 the user found the cutscenes in sync. Recordings carry `iopsync 2`; `iopsync 1`
recordings (2026-10-25 day) replay in mode 1, older ones in the old timing. `HG_IOPSYNC=0/1/2` overrides,
`HG_IOPSYNC_AT=N:M` switches to mode M at the game's VBlank N (a mode-1 recording replayed exactly to a cutscene,
which then plays with mode 2's clock). Gates in mode 2 (2026-10-26): two runs identical, `HG_VU1CHECK` 0
mismatches, all four renderers identical on both routes (706 and 746 frames), G5 0 differing batches, the soak
recording unchanged, saving identical to `ref/save.md5`, audiocheck PASS (4 clicks); `ref/hashA.md5` and
`ref/cs.md5` re-frozen (the mode-1 sets in git history).
