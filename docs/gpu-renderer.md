# GS on the GPU (Vulkan compute)

Roadmap Phase 5. The goal is a GPU renderer for 4K output at 60 fps. The first milestone, reached on the night of
2026-10-14/15, is a GPU rasterizer that produces **exactly** the same GS local memory as the software GS (`gs.c`)
for every primitive, checked word for word. Upscaling is built on top of that; see "Next" at the end.

## Files

| File | What |
|---|---|
| `recomp/vk/gs_sprite.comp` | The compute shader. Built twice: `gen/gs_sprite_spv.h` (one primitive per dispatch) and, with `-DBATCH`, `gen/gs_batch_spv.h` (a list of primitives per dispatch). |
| `recomp/gsvk.c`, `gsvk.h` | Vulkan set-up, buffers, both pipelines, upload / dispatch / download, GPU timestamps. |
| `recomp/gs.c` | `HG_GSVKCHECK` / `HG_GSVKBATCH`: builds the GPU primitive from the same set-up the CPU uses, decides what may be batched, compares. |

The build finds Vulkan through `pkg-config vulkan` (MSYS2 packages `mingw-w64-ucrt-x86_64-vulkan-devel` and
`...-glslang`, installed on 2026-10-14). Without them the GPU code is left out.

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
  exact rectangles for the same buffer). One known safe case is allowed: T8H/T4HL/T4HH textures read only the top
  bits of 32-bit words, which a frame buffer that keeps those bits (CT24, or FBMSK) never changes. The other
  overlapping primitives are drawn by **one invocation in the CPU's order** (`serial`), so every primitive runs on
  the GPU.

## Batches

`HG_GSVKBATCH=1`: primitives are collected into a list, and one dispatch draws the list. Each invocation owns one
pixel and goes through the list in order, so per pixel the order is the CPU's. Each 8x8 tile first marks, 256
primitives at a time, the primitives whose rectangle meets it (a bitmask in shared memory), so that its pixels skip
the rest.

A batch ends:
- when a primitive would read pages an earlier primitive of the batch wrote, or write pages one read;
- when the frame or Z buffer changes;
- before anything else writes local memory: image transfers, local-to-local copies, lines and serial primitives.

## Checking

`HG_GSVKCHECK=FROM[:TO]` (VBlanks): for each primitive (or batch, with `HG_GSVKBATCH=1`) in that range, the GPU
draws from the CPU's local memory as it is, then the CPU draws, and the whole 4 MB is compared. Local memory lives in
device-local memory with a host-cached mirror: only the 4 KB chunks that differ are uploaded, and everything comes
back for the comparison. Drawing is serial (GS workers off) while checking. The log ends with a summary
(`GSVKCHECK: ...`, `GSVKBATCH: ...`) and the first 40 mismatches, if any.

Ranges used (script: `D:\ps2-work\scratch\vkcheck.sh TAG`, or `vkbatch.sh TAG` for batches):

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
| Batches | 714,673 primitives in 10,913 batches | 0 batches |
| Serial primitives (read what they write) | 27,741 triangles | 0 words |

That is every primitive drawn in the four ranges: 742,414, none left to the CPU.

GPU time per frame (timestamps around the batch dispatches, with tile culling, over two runs; serial primitives not timed):

| Room | GPU ms per frame |
|---|---|
| Stage 1 | 2.4–2.6 |
| Castle Suite | 3.3–3.7 |
| ST_020 | 2.9–3.2 |

For comparison, a 60 fps frame is 16.7 ms. These times are at native resolution, with a batch submitted and waited
for at a time (checking needs that); a live renderer records all batches of a frame in one command buffer.

## Next

1. **Live mode** (`HG_GSVK=1`): the GPU's local memory becomes the real one. Needs, on the GPU:
   - image transfers (host to local) and local-to-local copies, as small shaders that use `write_px()`;
   - CLUT loads from GPU memory, or from the CPU's copy where the game uploads CLUTs by transfer (the usual case);
   - local-to-host transfers and the display read as download points.
   The software GS stays as the reference: `HG_GSVKCHECK` keeps comparing.
2. **Speed**: coarse binning on the CPU (or a binning pass) instead of the per-tile scan; serial primitives in a
   wavefront order instead of one invocation; all batches of a frame in one submission.
3. **Upscaling to 4K.** Keep the native-resolution local memory exact (it is what the game reads back, uses as
   textures and as CLUTs), and draw each frame-buffer pixel also into an N x N super-sampled surface. Textures read
   from frame buffers can use the super-sampled data, and the display shows it. This keeps every game effect that
   depends on memory layout intact, unlike a texture-cache renderer.
4. **60 fps**: the game logic runs at 30 fps (`docs/frame-timing.md`); 60 needs frame interpolation or a logic
   change, separate from the renderer.
