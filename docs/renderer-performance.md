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
| `HG_VU1AOT=0` | VU1 interpreter instead of the compiled programs |

The run log also shows:
- how many draws fell back to the scalar path, and why;
- the image-upload time per pixel format.

## VU1 compiled as whole programs, timing proven at build time (roadmap phase 2, 2026-10-15)

Machine: the user's Windows PC (9800X3D, RTX 5090). VU1 was about 2/3 of the EE thread; it is now about half of
what it was.

### Step 1: when do Q and P arrive? (`recomp/vu1timing.py`)

The interpreters' timing model matters for results in one place only: when a DIV/SQRT/RSQRT result reaches Q and an
EFU result reaches P (7, 13, 11-54 cycles, counted with pipeline stalls), as seen by the Q consumers (`MULq`, `ADDq`,
...) and P consumers (`MFP`). Flags are immediate (`HG_VUFLAGDELAY=0`), and every run starts with a drained
pipeline and ends with Q/P committed.

`vu1timing.py` runs both copies of the model (`vu1_core.h` and the oracle `vu1_ref.c`, side by side) as an exact
abstract execution over every path of each program: a node is (pc, pending branch target, E-bit pending), its
state the VF fields not yet readable, the FDIV/EFU countdowns and the cycles since the last issue; branch conditions
are not evaluated, so all paths are covered. Entries: 0 and the pcs where runs end (the MSCNT entries), to a
closure. Each consumer is **SAFE** (the latest result has arrived on every path), **OLD** (on none: it reads the
previous one) or **AMBIGUOUS**.

| | Consumers | SAFE | OLD | AMBIGUOUS |
|---|---|---|---|---|
| Q (`MULq`, `ADDq`) | 200 | 195 | 5 | 0 |
| P (`MFP` after `ERLENG`) | 71 | 58 | 13 | 0 |

All 19 programs are covered: 5,381 pairs, 5,717 states (about one per pair: every loop converges on its first
iteration), 63 DIVs, 65 ERLENGs, 10 WAITQs, 12 WAITPs, no JR/JALR, no path leaving a program, the two models never
stall differently, and the VI register a branch reads "old" (written by the previous pair) is static at all 62
branch nodes. The OLD consumers are deliberate software pipelining in the game's code (an `ADDq` 3 cycles after a
`DIV` reads the previous vertex's 1/w; `MFP` 1-13 cycles after an `ERLENG` reads the previous vertex's value), so
the hardware does the same. `python vu1timing.py -v` prints every consumer with its cycle range.

### Step 2: generator v2 (`recomp/vu1gen.py`)

Each program is one C function: a label per node, straight-line code per pair through `pair_untimed()` (the
interpreter's own `upper()`/`lower()` on constant instruction words, so decode folds away and the semantics stay
the interpreter's) and `goto` for control flow (a taken branch runs its delay-slot copy, then jumps).

- No cycle bookkeeping: no `vcyc`, stall model, `vf_rdy`, Q/P countdown, flag queue, dispatch switch or per-pair
  runaway test. A SAFE consumer commits the pending Q/P (`q_commit()`) just before it reads; an OLD one reads the old
  value. A pending Q/P is just a flag (`q_wait` nonzero).
- MAC/status: no program reads them (no FMxxx/FSxxx/FSSET), so a flag-setting pair whose flags another one
  overwrites on every path before the run can be observed (its end, or the runaway check on a backward jump) only
  contributes to the sticky status bits: 3,446 of 3,508 flag-setting pairs OR three lane masks
  (`flags_sticky`), folded into status at the run's end. MAC and status after every run are exactly the
  interpreter's.
- ACC is never clamped on input (it only ever holds clamped FMAC results); VF0/VI0 are not reset per pair (no op
  writes register 0).
- Entries other than the analysed ones go to the interpreter before anything runs (exact); a program the proof
  does not cover would be emitted the v1 way (per-pair `step()` with the cycle model): none is today.
- XGKICK sends its packets to the GS ring in one call (`gif_kick` in `gs.c`), the ring head published every 256
  entries as before.
- `vu1_ref.c` (the oracle) and the interpreter (`HG_VU1AOT=0`) are unchanged in behaviour.
- Build: the 19 programs compile in about 50 s with `make -j8`; a clean build takes 75 s.

### Verification

| Gate | Result |
|---|---|
| G4 `HG_VU1CHECK=1` (compiled code against `vu1_ref.c` after every run) | New Game to VB 4000: 2.02 M runs; Castle Suite to VB 4200: 1.36 M; the user's whole recorded session (soak route): 17.66 M; **0 mismatches, 0 handovers** |
| G1/G1g `hashcheck.sh` | software, GPU, +60 fps, x5: 702/702 identical to `hashA` each |
| G2 (`HG_ROUTE=cs`) | the same four: 742/742 identical to `cs.md5` |
| G3 `soak.sh` (with `HG_VU1CHECK=1`) | 1,005/1,005 identical to `soak.md5` |
| G5 `gpucheck.sh batch` | 0 batches differ in the four ranges |
| G7 window x5 + 60 fps (`wintest.sh`, `PL=58`) | 1.00x; presents median 16.72 ms, p5-p95 16.36-16.99, 0 repeats, 0 out of order |
| G11 clean build | 75 s; frames identical to `hashA` |

### Speed (`tools/verify/abba.py`, A = before phase 2, B = after; ms per VBlank, steady state)

| Scene | Mode | VU1 before | VU1 after | EE thread before | EE thread after | Drift |
|---|---|---|---|---|---|---|
| Stage 1 (VB 3000-4800) | `HG_GSDRAW=0` | 7.29 | **2.83** (-61 %) | 9.83 | **5.35** (-46 %) | 0.03 |
| Castle Suite (VB 3300-4800) | `HG_GSDRAW=0` | 6.91 | **2.73** (-60 %) | 8.96 | **4.77** (-47 %) | 0.06 |
| Stage 1 | `HG_GSVK=1` | 7.36 | **2.64** (-64 %) | 9.90 | **5.22** (-47 %) | 0.22 |
| Castle Suite | `HG_GSVK=1` | 6.80 | **2.66** (-61 %) | 8.83 | **5.45** (-38 %) | 0.03 |

Window, Castle Suite, x5 + 60 fps: the EE thread is busy 4.5 ms of each 16.7 ms VBlank (VU1 2.5). In steps (stage 1,
`HG_GSDRAW=0`): v1 7.28 -> whole programs 3.75 -> constant lane tables 3.45 -> XGKICK in one call 3.22 -> sticky-only
flags and no ACC clamp 2.83 ms. VU1 time includes XGKICK's GS ring traffic. About 707,000 instruction pairs run per
VBlank in stage 1 (VB 4000-4800), now about 4.0 ns per pair including the kicks (was 10.3).

### Decision: no VU1 thread now (roadmap Q2)

The user's rule (Q2): compile first, re-measure, then decide. After the compile the EE thread needs 4.8-5.5 ms per
VBlank of a 16.7 ms budget (about a third) in both scenes, and the window at x5 + 60 fps runs at 1.00x with the EE
thread 4.5 ms busy. A VU1 thread would take at most the 2.7 ms of VU1 (and the VIF1 unpacks) off this thread, at the
cost of a second producer for the GS ring and a copy of every VIF1 payload. It is not needed on this machine, so it
is deferred until a measurement shows a need (a slower machine, or later features eating the EE budget).

Design for later (from roadmap step 2.2, with what phase 2 learned):

- All GIF traffic has to come from one thread so that PATH1/2/3 stay in the EE's issue order: today `gif_kick`
  (PATH1, XGKICK), VIF1 DIRECT (`dmac.c`, `gif_qword(2)`) and GIF channel 2 (`feed`, `gif_qword(3)`) all run on the EE
  thread and push into the GS ring. The new thread would own `vif`, `vu1_run`, `gif_kick` and all three paths; the EE
  thread puts VIF1 payloads (copied at kick time, as the game may rewrite a buffer after a "completed" DMA), GIF
  channel 2 payloads and the VBlank marker into a ring in the style of the GS ring. The GS ring keeps one producer.
- The shadow packet tracking (`shadow[]`, `gif_path_done`) moves with it; `HG_VU1CHECK`, `HG_VU1DUMP`, `HG_CLIPLOG`,
  `HG_VTXLOG` run where VU1 runs. The EE's reads of `VIF1_STAT`/`VPU_STAT`/`D1_CHCR.STR` already answer "idle".
- Back-pressure: at most 2 VBlanks ahead (the `lag_ok` rule applied to both consumers). Ring size from
  `dma_dump_stats` (VIF1 qwords per VBlank).
- VU1 memory is never touched by the EE directly: the hardware-register table of the whole soak run has no access
  in 0x11000000-0x1100FFFF (checked in phase 2), so the thread owns `vu1_mem`/`vu1_code` alone.
- Gates: G1, G1g, G2, G3 (identical frames prove the order held), G4 on the new thread, G9, G8, G7, G12 (30 min).

Left for later (exact but not done): clean-value tracking per VF field so that inputs known to be clamped skip
`vclamp` (ITOF/FMAC results; LQ'd matrices stay unclamped, so the gain is the broadcast operands, maybe 10-15 % of
VU1); a cheaper GS ring entry for XGKICK traffic (part of the remaining VU1 time).

## GPU renderer cost in the user's rooms (2026-10-16)

See `docs/gpu-renderer.md`, "Speed, stage 2": the copies dominate (85 % of the GPU's time at x4/x5 + 60 fps), half
of it the replays of interpolation; the frame of each dispatch (tile marking, barriers, the copy's geometry) costs more
than the GS's per-pixel work. `HG_GSVK_DPROF=1` with `HG_GSVK_TIMING=1` prints the time per batch kind and size.
