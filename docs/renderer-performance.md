# Software renderer and VU1 performance

Date 2026-10-13. Machine: the development VM (2 vCPUs of a Ryzen 7 9800X3D, AVX2/AVX-512, no GPU).

## Rule: faster, never different

Every speed-up was checked to give the same output as the previous code:

- **Frames.** `HG_FRAMES` dumps, hashed with md5:
  - 421 frames from logos to stage 1 (every 5th VBlank, 495–2600);
  - 405 stage frames (VBlank 2380–2520, drawing on).
  - Both sets are identical to the original renderer, with and without the GS thread.
- **VU1.** `HG_VU1CHECK=1` runs the old per-lane interpreter (`recomp/vu1_ref.c`) in lockstep with the new one and
  compares, after every microprogram run, all registers, flags, data memory, the end address, and a hash of the
  data sent with XGKICK. Through boot and into stage 1, 607,338 runs were compared, with 0 mismatches.

## Results (stage 1, 600 frames with drawing)

Run: boot without drawing to VBlank 2380, then 600 stage frames with drawing (`HG_GSDRAW=2380 HG_MAXVB=2980`).
Per-frame time = (total − the same run stopped at 2380) / 600.

| Version | Total | Per stage frame |
|---|---|---|
| Original renderer (commit 7348cc2) | 70.4 s | 103 ms |
| Now, single-threaded (`HG_MTGS=0`) | 29.7 s | — |
| Now, with the GS thread (default) | 24.9 s | 33 ms (0.5x real time) |

Full run from boot to stage 1 (2600 VBlanks, everything drawn): 70.6 s → 23.6 s.

## What changed

- **Sprites** (full-screen effect passes, about 4.2 million pixels per frame):
  - Specialized row loops.
  - An AVX2 path, 8 pixels per step, with gathers for the texture and frame buffer.
  - The rest go through the generic vector pipeline.
  - Common passes now cost 3–5 ns per pixel, down from 12–15.
- **Triangles:**
  - 8-wide coverage test with conservative per-row spans.
  - A vector pixel pipeline: interpolation (the same float operations per lane, built with `-ffp-contract=off`),
    texture fetch (CT32/24/16/16S, T8/T4 with CLUT, 8H/4HL/4HH), bilinear filtering, perspective, fog, alpha/Z
    tests, all blend equations, and 32/24/16-bit frame buffers.
- **When the scalar path is still used:**
  - destination alpha test;
  - Z16 buffers;
  - a texture or Z buffer that overlaps the frame buffer. Vector lanes read before they write, so a pixel could
    see a texel written earlier by the same draw.
  - Sprites that read their own buffer (the channel-shuffle passes) are checked texel by texel, with bounds
    computed from the renderer's own per-pixel expressions.
- **Image uploads.** Aligned PSMT8/PSMT4/PSMCT32 rows are written a whole qword at a time. The game re-uploads
  textures every frame.
- **VU1.**
  - Instructions are decoded once; MPG uploads invalidate the decoded copies.
  - FMAC operations run on 4 lanes with SSE.
  - The "VI written by the previous instruction" branch rule now saves one old value instead of copying all 16
    registers per instruction.
  - About 2x faster (8.8 ns per micro-instruction).
- **GS thread** (`HG_MTGS`, default on with 2+ CPUs):
  - The GS runs on a second thread fed by a ring buffer.
  - The EE side tracks GIF packet ends itself (XGKICK needs them).
  - The display registers are snapshotted per VBlank.
  - The EE side stays at most two VBlanks ahead of the GS thread.

## Where the time goes now (per stage frame)

- **GS thread, about 31 ms** (the critical path):
  - sprites about 17.6 ms;
  - triangles about 13.3 ms; the main setup is bilinear T8 with CLUT, perspective, alpha test and Z24, at about
    14 ns per covered pixel;
  - image uploads.
- **EE thread, about 20 ms:** VU1 about 10 ms, plus EE code, the IOP and SPU2.
- **Limit.** Both cores are busy. Real time on this VM would need roughly 2x less work on both threads, and the
  remaining cost is mostly gather-bound texture fetch. Real time at higher resolutions needs the GPU renderer
  (not possible here: VMware 3D is off).

## GS workers (2026-10-14, Windows, 9800X3D)

Rasterizing is split across `HG_GSTHREADS` worker threads by 8-row bands. Page-level dependency tracking keeps the
output bit-identical to serial drawing; the rules are at "GS workers" in `gs.c` and in `handoff.md`. Stage 1 went
from 0.51x to 1.49x real time and the Castle Suite from 0.68x to 1.63x. The VU1 ahead-of-time compilation helped too.

## Switches

| Variable | Effect |
|---|---|
| `HG_MTGS=0` | GS on the main thread |
| `HG_GSTHREADS=n` | n rasterizing threads (default half the CPUs, at most 8; 0 = draw on the GS thread) |
| `HG_GSSIMD=0` | no AVX2 sprite paths |
| `HG_TRISIMD=0` | no vector pixel pipeline |
| `HG_GSDRAW=0` | no rasterizing (sound/logic runs) |
| `HG_GSDRAW=N` | rasterizing from VBlank N on |
| `HG_SPRSTAT=1` | per-configuration time for sprites |
| `HG_SPRSTAT=t` | per-configuration time for triangles |
| `HG_VU1CHECK=1` | VU1 lockstep check |

The run log also shows:
- how many draws fell back to the scalar path, and why;
- the image-upload time per pixel format.
