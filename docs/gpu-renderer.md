# GS on the GPU (Vulkan compute)

Roadmap Phase 5. The goal is a GPU renderer for 4K output at 60 fps. Reached on the night of 2026-10-14/15:

- a GPU rasterizer that produces **exactly** the same GS local memory as the software GS (`gs.c`) for every
  primitive, checked word for word;
- a **live mode** (`HG_GSVK=1`) in which the GPU draws everything: frames to VB 4000 are identical to the software
  GS's (702/702 against `hashA`), and the run is faster than the software GS on 4 worker threads.

Upscaling is built on top of that; see "Next" at the end.

## Files

| File | What |
|---|---|
| `recomp/vk/gs_sprite.comp` | The compute shader. Built twice: `gen/gs_sprite_spv.h` (one primitive per dispatch) and, with `-DBATCH`, `gen/gs_batch_spv.h` (a list of primitives per dispatch). |
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

1. **Fewer stalls.** Every VBlank the CPU waits for the GPU and reads back what it drew, for the display and frame
   dumps. Presenting straight from video memory, and double-buffering the command buffer, removes both.
2. **Speed**: coarse binning instead of the per-tile scan; serial primitives (none in the test ranges, but other
   rooms may have some) in a wavefront order.
3. **Upscaling to 4K.** Keep the native-resolution local memory exact (it is what the game reads back, uses as
   textures and as CLUTs), and draw each frame-buffer pixel also into an N x N super-sampled surface. Textures read
   from frame buffers can use the super-sampled data, and the display shows it. This keeps every game effect that
   depends on memory layout intact, unlike a texture-cache renderer.
4. **60 fps**: the game logic runs at 30 fps (`docs/frame-timing.md`); 60 needs frame interpolation or a logic
   change, separate from the renderer.
