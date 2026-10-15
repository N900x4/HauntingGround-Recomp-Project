# GS on the GPU (Vulkan compute)

Roadmap Phase 5. The goal is a GPU renderer for 4K output at 60 fps. Reached on the night of 2026-10-14/15:

- a GPU rasterizer that produces **exactly** the same GS local memory as the software GS (`gs.c`) for every
  primitive, checked word for word;
- a **live mode** (`HG_GSVK=1`) in which the GPU draws everything: frames to VB 4000 are identical to the software
  GS's (702/702 against `hashA`), and the run is faster than the software GS on 4 worker threads.

- **upscaling** (`HG_GSVK_SCALE=2..8`) on top of it, while the native local memory stays exact: since 2026-10-15
  x5 (3200x2240, 4K screens) runs at full speed in the window;
- **60 fps** by frame interpolation (`HG_GSVK_INTERP=1`), at full speed at native resolution (see 60 fps below).

## Files

| File | What |
|---|---|
| `recomp/vk/gs_sprite.comp` | The compute shader. Built three times: `gen/gs_sprite_spv.h` (one primitive per dispatch), with `-DBATCH` `gen/gs_batch_spv.h` (a list of primitives per dispatch, copy 0) and with `-DBATCH -DCOPIES` `gen/gs_batchc_spv.h` (the upscaling copies of a batch). |
| `recomp/gsvk.c`, `gsvk.h` | Vulkan set-up, buffers and rings, both pipelines, command recording (primitives, batches, uploads, downloads), GPU timestamps. |
| `recomp/gs.c` | The `HG_GSVK*` modes: builds the GPU primitive from the same set-up the CPU uses, decides what may be batched, tracks pages (live), compares (check). |

The build finds Vulkan through `pkg-config vulkan` (MSYS2 packages `mingw-w64-ucrt-x86_64-vulkan-devel` and
`...-glslang`, installed on 2026-10-14). Without them the GPU code is left out.

## Switches

| Variable | Effect |
|---|---|
| `HG_GSVK=1` | Live: the GPU draws everything, the software GS draws nothing. GS worker threads are off. |
| `HG_GSVKCHECK=FROM[:TO]` | Check: in those VBlanks every primitive is drawn by the GPU and by the CPU, from the same memory, and the whole 4 MB is compared after each one. Overrides `HG_GSVK`. |
| `HG_GSVKBATCH=1` | With `HG_GSVKCHECK`: compare after each batch instead (the live mode's batching). |
| `HG_GSVK_SCALE=N` | With `HG_GSVK=1`: N = 2..8 times the resolution (see Upscaling; the window takes up to 4096 pixels, so N <= 6 there). |
| `HG_GSVK_INTERP=1` | With `HG_GSVK=1`: 60 fps (see 60 fps). `2`: 120 fps, experimental (87 % speed). |
| `HG_VI_SELFTEST=1` | With `HG_GSVK_INTERP`: every frame also replayed with no movement and compared with the real one. |
| `HG_FRAMES_SHOWN=dir:FROM:TO[:PERIOD]` | Dump the pictures handed to the window in those VBlanks (4 every PERIOD). |
| `HG_PRESENTLOG=S` | Log the time and picture of 600 presents from S seconds on (pacing checks; `D:\ps2-work\scratch\pacing.py`). |
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
- A compute pass interleaves the copies' display area (as `display_from()` reads it) into one picture, which is read
  back and shown in the window. It runs only when something was drawn or uploaded since the last one.
- The copies are for the picture only. Their order analysis uses copy 0's geometry, and they are not
  bit-reproducible between runs: 18 of 4.6M pixels differed by one level at x4.

Speed (Castle Suite, windowed, F3 overlay): x2 (1280x896), x4 (2560x1792) and x5 (3200x2240) 100 %; x6 (3840x2688)
78 %, limited by the GPU. On a 4K screen (F11) the 4:3 picture is shown at 2880x2160, so x5 is slightly more than
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

Found on the way: `gsvk_prim` read back the primitive ring, which lives in video memory over PCIe (microseconds per
primitive); without that read, normal GPU rendering went from 9.5 to 8.4 ms per VBlank.

Not yet: 60 fps together with upscaling (x4: 74 %, x5: 59 %; the in-between picture is a second full render and is
read back synchronously), and 120 fps (`HG_GSVK_INTERP=2`: pictures at 1/4, 1/2, 3/4 and presents every 8.34 ms
work, but the game reaches 87 % speed).

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

1. **4K at 60 fps**: read the in-between pictures back asynchronously as well, and present straight from video
   memory (no read-back at all); then 120 fps the same way.
2. **Speed**: coarse binning instead of the per-tile scan; serial primitives (none in the test ranges, but other
   rooms may have some) in a wavefront order.
3. **Upscaling**: check the effect passes room by room (any that sample render targets at another scale may need
   their own offset rule); texture replacement.
