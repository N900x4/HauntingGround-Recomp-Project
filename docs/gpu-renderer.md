# GS on the GPU (Vulkan compute)

Roadmap Phase 5. The goal is a GPU renderer for 4K output at 60 fps. Reached on the night of 2026-10-14/15:

- a GPU rasterizer that produces **exactly** the same GS local memory as the software GS (`gs.c`) for every
  primitive, checked word for word;
- a **live mode** (`HG_GSVK=1`) in which the GPU draws everything: frames to VB 4000 are identical to the software
  GS's (702/702 against `hashA`), and the run is faster than the software GS on 4 worker threads.

- **upscaling** (`HG_GSVK_SCALE=2..8`) on top of it, while the native local memory stays exact: since 2026-10-15
  x5 (3200x2240, 4K screens) runs at full speed in the window;
- **60 fps** by frame interpolation (`HG_GSVK_INTERP=1`), at full speed at native resolution (see 60 fps below).
- **4K and 60 fps together** (2026-10-15, roadmap phase 1): x5 + 60 fps at full speed in the window, the window
  presented straight from video memory (see "Presenting the window" and "Speed, phase 1" below); 120 fps at full
  speed at native resolution.

## Files

| File | What |
|---|---|
| `recomp/vk/gs_sprite.comp` | The compute shader. Built three times: `gen/gs_sprite_spv.h` (one primitive per dispatch), with `-DBATCH` `gen/gs_batch_spv.h` (a list of primitives per dispatch, copy 0) and with `-DBATCH -DCOPIES` `gen/gs_batchc_spv.h` (the upscaling copies of a batch). |
| `recomp/gsvk.c`, `gsvk.h` | Vulkan set-up, buffers and rings, the pipelines, command recording (primitives, batches, uploads, downloads), GPU timestamps; the picture images and the window's swapchain (presenting from video memory). |
| `recomp/host_sdl.c` | The UI thread: SDL window and input; presents with `gsvk_present` (GPU renderer) or an SDL texture. |
| `recomp/gs.c` | The `HG_GSVK*` modes: builds the GPU primitive from the same set-up the CPU uses, decides what may be batched, tracks pages (live), compares (check). |

The build finds Vulkan through `pkg-config vulkan` (MSYS2 packages `mingw-w64-ucrt-x86_64-vulkan-devel` and
`...-glslang`, installed on 2026-10-14). Without them the GPU code is left out.

## Switches

| Variable | Effect |
|---|---|
| `HG_GSVK=1` | Live: the GPU draws everything, the software GS draws nothing. GS worker threads are off. The window is presented from video memory. |
| `HG_GSVKCHECK=FROM[:TO]` | Check: in those VBlanks every primitive is drawn by the GPU and by the CPU, from the same memory, and the whole 4 MB is compared after each one. Overrides `HG_GSVK`. |
| `HG_GSVKBATCH=1` | With `HG_GSVKCHECK`: compare after each batch instead (the live mode's batching). |
| `HG_GSVK_SCALE=N` | With `HG_GSVK=1`: N = 2..8 times the resolution (see Upscaling; up to x5 at full speed with 60 fps). The SDL path (`HG_PRESENT=sdl`) takes up to 4096 pixels, N <= 6. |
| `HG_GSVK_INTERP=1` | With `HG_GSVK=1`: 60 fps (see 60 fps). `2`: 120 fps (full speed at native resolution, x5 55 %). |
| `HG_PRESENT=sdl` | With `HG_GSVK=1`: the old window path (pictures read back and copied into an SDL texture). |
| `HG_VSYNC=1` / `0` | The swapchain's present mode: default mailbox (no tearing, never blocks); `1` FIFO (stalled 60-200 ms at a time in a window on the 120 Hz screen); `0` immediate (may tear). |
| `HG_GSVK_TIMING=1` | GPU time per phase (exact batches, copies, single primitives, compose, transfers; the replays apart), per VBlank every 600 VBlanks and at the end. Serializes the exact and copies dispatches, so the total grows a little. |
| `HG_GSVK_EAGER=N` | Submit the command buffer every N batches (default 2; 0: only when a result is needed, as before). |
| `HG_GSVK_LAG=N` | The GS thread waits at each VBlank until the GPU has finished the work of N VBlanks ago (default 2). |
| `HG_VI_SELFTEST=1` | With `HG_GSVK_INTERP`: every frame also replayed with no movement and compared with the real one. |
| `HG_VI_ADAPT=0` / `1` | Adaptive interpolation (in-between pictures left out when the GPU is short of time, see 60 fps): default on with the window, off headless; `0` always off (every frame interpolated, the old behaviour), `1` on headless too. |
| `HG_VI_BUDGET=MS` | Adaptive interpolation: GPU ms per VBlank, with interpolation, above which the in-between pictures stop (default 14.5; back below MS - 1). |
| `HG_FRAMES_SHOWN=dir:FROM:TO[:PERIOD]` | Dump the pictures handed to the window in those VBlanks (4 every PERIOD). |
| `HG_PRESENTLOG=S` | Log the time and picture of 600 presents from S seconds on (pacing checks; `tools/verify/pacing.py`). |
| `HG_FRAMES_HR=dir` | With `HG_GSVK_SCALE`: dump the upscaled picture (`hr_VVVVVV.ppm`, every `HG_FRAME_EVERY` VBlanks). |

Do not combine `HG_HOSTPROF=1` with the GPU modes: a run with both hung during boot (the sampler suspends threads,
probably inside the Vulkan driver).

## How it stays bit-exact

The shader is a transcription of `pixel()`, `shade()`, `sample()`, `texel()`, `draw_sprite()`, `draw_triangle()`
and `draw_line()`, with these precautions:

- **Float operations** are marked `precise`, so nothing is fused into a multiply-add (`gs.c` is built with
  `-ffp-contract=off`). Add, subtract and multiply are correctly rounded in Vulkan, as on x86.
- **Division** is not correctly rounded on GPUs. `divx()` takes the GPU quotient and steps to a neighbour while its
  exact remainder, from `fma()`, is smaller. Used for the perspective divide and for lines.
- **Float to int**: `cvt()` gives 0x80000000 for NaN and out-of-range values, as x86 `cvttss2si` does.
  `lround_()` replaces `lroundf()`.
- **Triangle Z** is interpolated in double precision, as in `gs.c` (`shaderFloat64`, which the RTX 5090 has).
- **Triangle set-up** (vertex positions, 1/area, texture coordinates per vertex) is done on the CPU by the same code
  and passed in; only the per-pixel work is on the GPU.
- **16-bit pixels**: two neighbours share a 32-bit word, so 16-bit frame and Z writes use `atomicAnd`/`atomicOr`.
- **Order**: on the GPU the pixels of one primitive run in parallel. That is only equivalent when no pixel reads
  memory another pixel of the same primitive writes. `gs.c` checks frame, Z and texture rectangles (8 KB pages, or
  exact rectangles for the same buffer). Two safe cases are recognised:
  - T8H/T4HL/T4HH textures read only the top bits of 32-bit words, which a frame buffer that keeps those bits (CT24,
    or FBMSK) never changes;
  - **own-pixel** triangles: the game draws alpha passes that texture from their own frame buffer, through T8H, at
    the pixel's own position (offset 0.51–0.56 texel). Same buffer, 32-bit layout, point sampling, the same q at all
    vertices, and every vertex's offset in (0.02, 0.98) inside the texture: then u − x at any pixel is a convex
    combination of the offsets, so each pixel reads exactly its own word before writing it.
  Any other overlapping primitive is drawn by **one invocation in the CPU's order** (`serial`). In the test ranges
  none is left.

## Batches

Primitives are collected into a list, and one dispatch draws the list. Each invocation owns one pixel and goes
through the list in order, so per pixel the order is the CPU's. Each 8x8 tile first marks, 256 primitives at a time,
the primitives whose rectangle meets it (a bitmask in shared memory), so its pixels skip the rest.

A batch ends:
- when a primitive would read pages an earlier primitive of the batch wrote, or write pages one read (own-pixel
  triangles read only their own pixels, which the same invocation wrote, in order);
- when the frame or Z buffer changes;
- before anything else touches local memory: uploads, image transfers, local-to-local copies, lines and serial
  primitives.

## Live mode

`gsvk.c` records into one command buffer that stays open until a result is needed: batch dispatches, single
dispatches, uploads (data copied into a ring at once, so later CPU changes do not leak in) and page downloads. The
primitives and CLUTs go into rings in video memory the CPU writes but never reads.

`gs.c` keeps the CPU's copy of local memory for what the CPU itself reads, and tracks 8 KB pages: written by the GPU
since the CPU last read them back (`pg_gpu`), or written by the CPU since they last went up (`pg_cpu`). A page is
never in both.

| The CPU... | Before | After |
|---|---|---|
| loads a CLUT | its pages come back from the GPU if the GPU wrote them | – |
| receives image data (host to local) | the target pages come back if the GPU wrote them | they go up before the next GPU work |
| copies local to local | everything the GPU wrote comes back | the destination goes up |
| shows or dumps a frame (each VBlank) | everything the GPU wrote comes back | – |

Run to VB 4000 (`runhash.sh` with `HG_GSVK=1`): 8.0M primitives in 116,588 batches, 8,935 submissions, 8,934
read-backs (1.1M pages), 255k pages uploaded, GPU busy 4.0 s; frames identical to `hashA`.

## Upscaling

Besides the exact local memory (copy 0) the GPU keeps N x N more 4 MB copies. Copy `1 + i + j*N` is drawn with every
primitive moved by `(-dx_i, -dy_j)`, so its pixel (x, y) shows the scene at `(x + dx_i, y + dy_j)`, with
`dx_i = ((2i + 1) * 8 / N - 8) / 16` pixel: a whole number of the GS's 1/16 units, so the fixed-point set-up stays
exact (N = 2, 4 or 8). Interleaving the copies gives a picture N times larger in each direction, rendered properly
(geometry, coverage, depth and texture coordinates per sample), not stretched.

- Uploads (textures, CLUTs, movie frames) go to every copy; everything the CPU reads comes from copy 0, which stays
  bit-exact: frames to VB 4000 with x4 are identical to `hashA`.
- **Render targets.** In copy k a frame buffer's pixel t holds the scene at t + offset. A primitive that textures
  from pages the GPU has drawn (since the CPU last wrote them) therefore reads, in copy k, at its coordinates minus
  the offset. Without this, the full-screen effect passes mixed a different set of neighbours in each copy, which
  showed as vertical banding.
- Textures keep their native resolution (texture replacement is a later step).
- **Copies made on the GPU.** A batched primitive is written once; it carries its raw sprite or point vertices,
  texture coordinates and render-target flag, and the copies build of the batch shader (workgroup z + 1 = the copy)
  moves each copy's geometry itself. Copy 0 uses the exact pipeline. The copies build is compiled without
  `precise`: glslang's precise analysis hung on the rewritten fields, and the copies are for the picture only.
  Lines and serial primitives still get their copies from the CPU. Any N from 2 to 8: triangle offsets are exact,
  sprite and point offsets are rounded to 1/16 pixel.
- Uploads cross the bus once (into copy 0 of the set) and are copied on the GPU into the others.
- The window's picture is composed each VBlank and collected at the next one (command buffers rotate), so the GS
  thread does not wait for the GPU every VBlank; one VBlank of extra delay.
- A compute pass interleaves the copies' display area (as `display_from()` reads it) into one picture, which goes to
  the window from video memory (see "Presenting the window"); dumps and the SDL path read it back.
- **The copies shader works on tiles of the upscaled picture**: invocation (X, Y) of a workgroup draws copy
  `1 + X%N + (Y%N)*N` at pixel `(X/N, Y/N)`, so a workgroup draws all copies of a small area. A triangle covering a
  pixel covers it in most copies, so the invocations stay busy together on the small triangles of 3D scenes (one tile
  per copy left most of them idle): triangle copies 30 % cheaper. A triangle's coverage is tested from its geometry
  before the whole primitive (about 120 words) is loaded (7 %). Tried and dropped: a tile mask made once per batch
  for all copies (no gain: the per-tile scan was not the cost), float instead of double Z in the copies (no gain).
- The copies are for the picture only. Their order analysis uses copy 0's geometry, and they are not
  bit-reproducible between runs: 18 of 4.6M pixels differed by one level at x4.

Speed (Castle Suite, windowed, F3 overlay): x2 (1280x896), x4 (2560x1792) and x5 (3200x2240) 100 %; x6 (3840x2688)
78 %, limited by the GPU. With 60 fps: x4 and x5 100 %, x6 77 % (phase 1, below). On a 4K screen (F11) the 4:3 picture is shown at 2880x2160, so x5 is slightly more than
the screen's own resolution.

## 60 fps (frame interpolation)

The game updates and draws at 30 fps: a frame is drawn in one VBlank interval, copied into the display buffer, and
shown for two VBlanks (`docs/frame-timing.md`). With `HG_GSVK_INTERP=1`:

- **Log.** Each frame's GPU records (the finished per-primitive records, uploads and batch breaks) are logged with
  the primitive they came from (draw state, vertices, CLUT).
- **Frame completion**: something was drawn into the display buffer since the last VBlank, two VBlanks after the
  previous completion (60 Hz menus are left alone).
- **Replay.** The frame's records are submitted again into a separate set of copies of local memory (set I), from a
  snapshot (set S) of the memory as the frame began, with each triangle's vertices moved halfway towards where they
  were in the previous frame. Sprites (effect passes, text) and lines are replayed as they were. That picture is
  shown at this VBlank and the real one at the next: one VBlank later than before.
- **Matching.** A triangle's identity is its draw state plus its vertices' texture coordinates, which come from
  the model and do not move with the camera. Identities found once in each frame feed a motion field per render
  target (16-pixel cells); ambiguous ones (repeated floor tiles) take the candidate whose movement agrees with the
  field (within 8 pixels); triangles the game clipped differently in the two frames move with the matched vertices
  they share, else with the field. A frame whose matched triangles moved 32 pixels on average is a camera cut: the
  real picture is shown twice.
- **Exactness.** The game's own memory is never touched (frames identical to `hashA` with interpolation on).
  `HG_VI_SELFTEST=1` replays each frame with no movement and compares it with the real frame pixel for pixel
  (1,496 frames, 0 differ); it caught uploads logged after the primitive that triggered them.
- **Window.** A UI thread owns the window: it polls input and presents one picture every 1/59.94 s from a queue
  the GS thread fills, so every picture is shown once and in order. Castle Suite, windowed: 100 % speed, presents
  every 16.68 ms (5th-95th percentile 16.50-16.88), in-between and real pictures alternating.
- **Replays, phase 1.** The last replay of a frame goes straight into the snapshot set S (the next snapshot replaces
  it anyway), so 60 fps no longer copies S into I first; set I is used only by the 120 fps replays before it and by the
  self-test. With upscaling the replays leave out the set's exact copy, which the picture does not use.
- **Adaptive (2026-10-15, after the user's playtest).** The user's rooms needed 15-19 ms of GPU per VBlank at x5 +
  60 fps (the Castle Suite 14.3), so emulation fell behind real time: sound underran (~350 times in 6 minutes),
  stutter. The in-between pictures are display-only, so now they are left out when the GPU is short of time and
  the real picture is shown again (as at 30 fps); the game, its frames and their hashes are unchanged. Two signals
  (`vi_adapt_tick` in `gs.c`): a **budget**, from GPU time measured every 30 VBlanks in two parts (the
  interpolation's command buffers are tagged, `gsvk_tag`) so that the cost *with* interpolation is known also while
  it is off: P = B + f x kappa x B (B: the rest per VBlank, f: interpolated frames per VBlank, kappa: a frame's
  interpolation / B, measured while on). P smoothed above `HG_VI_BUDGET` (default 14.5 ms) turns the pictures off
  (and the snapshot copy with them); they come back under the budget - 1 ms after at least 2 s. And **behind**:
  when a frame completes and the GPU has not finished what was submitted up to the previous VBlank
  (`gsvk_poll`, never waits), that frame's picture is left out. Counted in the GSVK line every 600 VBlanks, at
  exit, and as `SKIP n` (pictures left out in the last second) in the F3 overlay. On by default when the window
  shows the pictures; off headless, with `HG_VI_SELFTEST` and with picture dumps, so the gates are unaffected.

Found on the way: `gsvk_prim` read back the primitive ring, which lives in video memory over PCIe (microseconds per
primitive); without that read, normal GPU rendering went from 9.5 to 8.4 ms per VBlank.

120 fps (`HG_GSVK_INTERP=2`: pictures at 1/4, 1/2, 3/4, presents every 8.34 ms) runs at full speed at native
resolution since phase 1; with upscaling it is GPU-bound (three replays per frame: x5 55 %).

## Presenting the window (phase 1)

With the GPU renderer the window's pictures never leave video memory:
- `gsvk_pic()` (GS thread) composes a picture as before and copies it into one of 12 picture images
  (`R8G8B8A8`, any size), then submits; the picture queue (`fq` in `gs.c`) holds {image, submission id} instead of
  28 MB of pixels.
- Every GS submission signals a timeline semaphore with its id (Vulkan 1.2). The UI thread takes the next picture
  whose submission is done (else shows the last one again rather than wait), and in its own command buffer, on the
  queue family's second queue (one queue and a lock if there is only one), clears the swapchain image, blits the
  picture into the largest 640:448 rectangle (as `SDL_RenderSetLogicalSize(640, 448)` did), blits the F3 overlay
  (a 176 x 42 image), and presents. A picture image is free again when the UI thread has moved on and its blits are
  done, or when the picture was dropped unseen.
- Present mode mailbox (no tearing, never blocks; the UI thread's 59.94 Hz clock decides when). FIFO blocked the UI
  thread's submissions for 60-200 ms at a time in a window on the 120 Hz screen, even at native resolution.
- The window is created without an SDL renderer when `HG_GSVK=1`; the UI thread sets up the swapchain once the GPU
  renderer is ready (first primitive) and falls back to SDL if that fails. `HG_PRESENT=sdl` keeps the old path.
- At exit the UI thread waits for its queue and drops the swapchain (`host_shutdown`, called by `stop()`).
- Checked with `VK_LAYER_KHRONOS_validation` (MSYS2 `mingw-w64-ucrt-x86_64-vulkan-validation-layers`,
  `VK_LAYER_PATH=C:\msys64\ucrt64\bin VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`): no errors in a windowed
  x5 + 60 fps run to VB 1300. Caution: every run with the layer loaded hung at process exit, after the last log line,
  and the process then stayed in the task list unkillable ("no running instance"); without the layer the exit is clean.

## Speed, phase 1 (2026-10-15)

Where the time went at x5 + 60 fps (headless, Castle Suite, `HG_GSVK_TIMING=1`, GPU ms per VBlank): 14.0 in all, of
which the upscaling copies 7.4 for the real frames and 7.2 for the replays (about 60 % of them the full-screen effect
passes), the exact copy 1.4 + 1.4, transfers 1.2. But the GS thread waited for the GPU almost all that time: work was
submitted only at read-backs, and three read-backs per VBlank (textures streamed into pages the effect passes had
drawn) each waited for everything; in the window the in-between picture also waited for the whole queue and crossed
the bus twice (28 MB at x5). The fixes, in order of gain:
- image transfers that cover whole pages no longer read those pages back (`full_pages()` in `gs.c`);
- the per-VBlank read-back of everything (meant for frame dumps only) skipped with upscaling as intended;
- submissions every 2 batches and when the GS thread runs out of work, 32 command buffers, rings in two halves;
- the copies shader by upscaled tiles (above);
- presenting from video memory; replays without the I = S copy and without the exact copy; the GPU kept at most two
  VBlanks behind.

Window, Castle Suite (LOAD GAME), muted, `tools/verify/wintest.sh` + `pacing.py` (speed from VB 3000; presents from
58 s: median, 5th-95th percentile; GPU ms per VBlank of emulated time):

| Mode | Before | Speed | Presents | Repeats / out of order | GPU ms |
|---|---|---|---|---|---|
| native GPU (`HG_GSVK=1`) | 100 % | 100 % | 16.74, 16.18-17.20 | 0 / 0 | 4.6 |
| native 60 fps | 100 % | 100 % | 16.73, 16.27-17.09 | 0 / 0 | 3.2 |
| x5 | 100 % | 100 % | 16.70, 16.13-17.22 | 0 / 0 | 9.8 |
| x4 + 60 fps | 74 % | 100 % | 16.73, 16.33-17.04 | 0 / 0 | 9.8 |
| **x5 + 60 fps** | 59 % | **100 %** | 16.73, 16.38-16.97 | 0 / 0 | 14.3 |
| x6 + 60 fps | - | 77 % | 16.68, 16.46-16.92 | 143 / 0 | 19.8 |
| native 120 fps | 87 % | 100 % | 8.40, 7.99-8.69 | 0 / 0 | 5.7 |
| x5 + 120 fps | - | 55 % | 8.34, 8.17-8.49 | 275 / 0 | 28.4 |
| x5 + 60 fps, `HG_PRESENT=sdl` | 59 % | 77 % | 16.66, 15.15-18.27 | 147 / 0 | 14.3 |
| software GS (`HG_GSVK=0`) | 100 % | 100 % | 16.68, 16.08-17.32 | 0 / 0 | - |

The present times are logged after the present call, so they include its own variance (about +-0.3 ms); with mailbox
on the 120 Hz screen the picture is shown at the next refresh anyway. x6 + 60 and x5 + 120 are limited by the GPU
(budget 16.7 ms per VBlank); headless, x5 + 60 runs at 1.07x and x5 at 1.25x.

## Checking

Local memory lives in device-local memory with a host-cached mirror. For a check the GPU's memory is set to the CPU's
(only the 4 KB chunks that differ from the mirror go up), the primitive or batch is recorded, then the whole of it
comes back and is compared with the CPU's result. The log ends with a summary (`GSVKCHECK: ...`, `GSVKBATCH: ...`)
and the first 40 mismatches, if any.

Ranges used (scripts: `D:\ps2-work\scratch\vkcheck.sh TAG` one primitive at a time, `vkbatch.sh TAG` batches):

| Range | VBlanks | Run |
|---|---|---|
| Menus | 500–2400 | New Game path, empty card |
| Stage 1 | 3000–3060 | movies skipped, New Game |
| Castle Suite (ST_021) | 3400–3440 | LOAD GAME from the user's save |
| ST_020 (door specks room) | 7900–7940 | `HG_PADPLAY=../recordings/20261014-200745` |

## Results (2026-10-15)

| Check | Primitives | Differing |
|---|---|---|
| One primitive per dispatch | 46,578 sprites, 668,095 triangles | 0 words |
| Batches | 742,414 primitives in 10,774 batches (27,741 own-pixel triangles among them) | 0 batches |
| Live, frames to VB 4000 | 702 frames | 0 frames |
| Live at x4 (copy 0), the user's whole session `20261014-200745` replayed (20,600 VBlanks, 92M primitives) | 1,005 frames, against the software GS | 0 frames |

Speed (headless, per VBlank, VB 3000–4800 in stage 1 and 3200–4400 in the Castle Suite):

| Room | Renderer | Host ms per VBlank | GS thread busy |
|---|---|---|---|
| Stage 1 | software GS, 4 workers (2 runs) | 10.48, 10.31 | 59 % |
| Stage 1 | GPU, live (2 runs) | 9.47, 9.47 | 15 % |
| Castle Suite | software GS, 4 workers | 8.63 | 66 % |
| Castle Suite | GPU, live | 8.42 | 29 % |

In both modes the EE thread (VU1 and game code) is the limit: the GS thread has room to spare with the GPU. The GPU
mode also frees the four cores the GS workers used (Windows sees 8 logical CPUs), which shows as a slightly faster EE
thread. Real time needs 16.7 ms per VBlank.

GPU time per frame from timestamps, batch dispatches only, at native resolution: 2.4–3.7 ms in the 3D rooms.

## Next

1. **GPU cost** for x6 + 60 and 120 fps with upscaling: the full-screen effect passes are about 60 % of the copies'
   time (each drawn N x N times); the exact copy is 10 % (one dispatch per batch, `precise`, fp64 Z). Serial
   primitives (none in the test ranges, but other rooms may have some) in a wavefront order.
2. **Display rate**: lock the emulated VBlank to the display's refresh within 0.5 % (the user's choice, roadmap Q14),
   exact 59.94 as a setting; not done (the UI thread still runs its own 59.94 Hz clock and presents with mailbox).
3. **Upscaling**: check the effect passes room by room (any that sample render targets at another scale may need
   their own offset rule); texture replacement.
