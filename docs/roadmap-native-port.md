# Roadmap to the native port: review and plan (written 2026-10-15)

Written by Fable 5.1 after reading the docs, the git history (69 commits, 2026-10-12 to 2026-10-15), the runtime in
`recomp/` and the tools in `tools/`. Nothing was built or run for this document. It has four parts:

1. what exists and how it is verified (short);
2. the review: risks, weak spots, technical debt, untested areas, and what threatens the end goal;
3. the plan, as phases with steps, gates, dependencies, risks and effort, starting with the user's ordered batch
   (4K + 60 fps, CPU part 2, in-game settings, DualSense);
4. questions for the user, to be answered before the work is orchestrated.

The end goal, in the user's words: **a native PC port that runs at 4K / 60 fps and is easily readable and moddable.**

---

## 1. What exists today

### 1.1 Architecture in one page

| Layer | Files | What it does |
|---|---|---|
| Translator | `recomp/rec2.py`, `bulk2.py`, `r5900.py`, `data/*.txt|json` | Every one of the 11,499 MIPS functions becomes one C function `f_XXXXXXXX(Ctx *)` in `gen/part00..15.c` (one C statement per instruction, labels per address, switch tables, delay slots). `REC_CYC(n)` adds a per-block instruction estimate to `ee_cycles`; a VBlank is due every `ee_vb_period` (= 300 M instructions / 59.94). `REC_ENTER` counts calls and runs `rec_hook[]` (the override mechanism). |
| EE runtime | `runtime.c`, `hle.c`, `interp.c`, `compat.c` | 32 MB RAM, dispatcher for indirect calls, EE kernel HLE (threads as Win32 fibers, semaphores, INTC/DMAC handlers, timers, SIF command layer, CDVD over `DATA.CVM`), MMI/COP0/COP2 interpreter fallbacks, Windows/Linux compatibility. |
| IOP, sound, movies | `iop_cpu.c`, `iop_kernel.c`, `spu2.c`, `ipu.c`, `iop_pad_mc.c` | The game's own IRX sound modules on an emulated R3000 + SPU2; IPU + scratchpad DMA so the game's own Sofdec player plays movies; controller (DBCMAN) and memory card (MCSERV) as HLE; input recording/replay. |
| DMA, VIF1, VU1 | `dmac.c`, `vu1.c`, `vu1_core.h`, `vu1_ref.c`, `vu1gen.py` | Synchronous DMA on the EE thread; VIF1 unpack fast path; VU1 interpreter (SSE lanes, stall model) with the 19 microprograms ahead-of-time compiled in 64-pair chunks from the interpreter's own `step()`; a per-lane reference interpreter for lockstep checks. |
| GS | `gs.c` (2,690 lines, 187 KB), `gsvk.c`, `vk/gs_sprite.comp` | Exact software GS (scalar + AVX2, 4 worker threads by 8-row bands), the GS thread (ring from the EE thread), the Vulkan compute GS (bit-exact), N x N sub-pixel upscaling copies made on the GPU, 60/120 fps frame interpolation by replaying logged GPU work with moved triangles, display composition, picture queue to the window. |
| Window | `host_sdl.c` | A UI thread owns the SDL window: polls input, presents one picture every 1/59.94 s from the queue, F3 overlay, F11 fullscreen; the game thread paces emulated time to real time and reads the atomics. Keyboard/controller maps are hard-coded tables. |
| Overrides | `overrides.c` | Three hooks (movie skip). The targeted decompilation has not started. |

Threads at run time: game/EE thread (guest threads are fibers on it), GS thread, 0 or 4 GS workers (software mode only), UI thread, timer thread; plus the GPU through 4 rotating command buffers.

### 1.2 How correctness is established today

| Check | Tool | Last result |
|---|---|---|
| Frame hashes, New Game to VB 4000 (software GS reference) | `tools/verify/runhash.sh` vs `D:\ps2-work\scratch\hashA.md5` | 702/702 identical with workers, GPU live, GPU x4, GPU + interpolation |
| Frame hashes, LOAD GAME to the Castle Suite | same, 742 frames (`cs4*.md5` in scratch) | identical |
| Whole recorded session (20,600 VBlanks, 92 M primitives) | `scratch/soak.sh`, recording `recordings/20261014-200745` | 1,005/1,005 frames GPU x4 = software GS |
| GPU vs CPU word for word | `HG_GSVKCHECK` (per primitive), `HG_GSVKBATCH` (per batch), `scratch/vkcheck.sh`, `vkbatch.sh`, four ranges | 742,414 primitives, 0 differing words |
| Interpolation replay exactness | `HG_VI_SELFTEST=1` | 1,496 frames, 0 differ |
| VU1 lockstep | `HG_VU1CHECK=1` (fast interpreter and AOT code vs `vu1_ref.c`, after every run: registers, flags, data memory, XGKICK hash) | 2,000,000 runs, 0 mismatches |
| VIF unpack fast path | `HG_UNPACKCHECK=1` | 76.7 M unpacks, 0 mismatches |
| Pacing | `HG_PRESENTLOG`, `scratch/pacing.py` | 16.68 ms median, every picture once, in order |
| Timing claims | `tools/verify/abba.py` (A, B, B, A with drift) | used for the VIF change (-12.3 %, drift 0.04 ms) |
| Against the console (visual) | PCSX2 unattended instance, `pcsx2_ref.py`, `framecmp.py` | stage 1 and Castle Suite 99.7-99.9 % of pixels within 8 levels |

This is a strong setup. The exact software GS acts as the oracle for the GPU, the recorded session acts as the oracle for speed-ups and later rewrites, and the lockstep checks catch semantic drift in the hot paths.

---

## 2. Review: risks, weak spots, debt

Ordered roughly by how much each threatens the end goal.

### 2.1 Readability is the largest unpaid debt

- The hand-written runtime is dense. `gs.c` averages 68 characters a line with 109 lines over 160 characters (longest 386); `hle.c` has a 354-character line; several statements per line and several declarations per `static` are the norm (`gs.c:1153-1186`, `host_sdl.c:26-30`). The top-of-section comments are good, but the code between them is hard to change safely by anyone who did not write it.
- `gs.c` holds seven concerns in one file: local memory and swizzling, the scalar pixel pipeline, the AVX2 pipeline, the GS worker threads, the GPU bridge (page tracking, batches, checks), frame interpolation, display composition and picture queue, the GS thread. The GPU work of the next phases touches three of these.
- Configuration is 68 distinct `HG_*` environment variables read by `getenv` at 78 sites in 14 files (`GETENV_ONCE` on hot paths), with no central list, no types, no defaults in one place. `play.bat` documents nine of them. An in-game settings page (phase 3) cannot be built on top of this without a settings module.
- The generated code is unreadable by design (`ctx->r[4].u32[0]`, `L_1beef4:`), and there is no symbol map: `data/` holds function starts, jump tables and syscalls but no names. `docs/frame-timing.md` names the first three functions to decompile; none has been done. "Readable, moddable" today means "hookable by address".
- Positive: the generator (`rec2.py`, 298 lines) is small and clear; the verification tools are documented; each runtime file starts with an honest description.

### 2.2 The reference data and the verification scripts live outside the repository

`hashA.md5`, `cs4*.md5`, `soak.sh`, `wintest3.sh`, `vkcheck.sh`, `vkbatch.sh`, `pacing.py` are in `D:\ps2-work\scratch`, not under version control (`handoff.md`: "the reference frame hashes ... are gone after a reboot"; the Linux hashes were lost once already). The Windows reference was never compared with the Linux VM's output. A reset or a disk change loses the oracle. The fix is cheap (section 3, phase 1 step 0): the hashes are a few hundred KB of text and the scripts are under 30 lines each.

### 2.3 4K + 60 fps: where the time goes

At every VBlank with interpolation on (`vi_vblank`, `gs.c:1797-1836`): `vi_replay` (CPU matching and a second GPU render), `display_pic_` records the composition into a slot, then **`gsvk_finish()` waits for the whole GPU queue**, `gsvk_compose_fetch` copies the picture over PCIe, `fq_push` copies it again, `gs_display` copies it into `front` and once more into the UI thread's buffer, and `SDL_UpdateTexture` sends it back to the GPU. At x5 a picture is 3200 x 2240 x 4 = 28.7 MB; two pictures per 33 ms means about 7 GB/s of CPU copies and 1.7 GB/s of read-back, under a mutex the UI thread also takes. The static buffers (`host_sdl.c:30` `frame[4096*4096]`, `front`, 8 queue slots, 3 in-between pictures, `real_pic`'s buffer) are 64 MB each and cap the window at 4096 px. Measured: x4 + 60 fps 74 %, x5 59 %; x6 alone 78 % GPU-bound. The fix (present from video memory, no read-back) is known and is phase 1.

The copies pipeline cost is unknown per phase: `gsvk_stats` gives one GPU time for everything. Before shader work, GPU timestamps per phase (exact batch, copies, replay, compose) are needed.

### 2.4 Frame interpolation: tuned on one room

The matching heuristics (`vi_match`, `gs.c:1617`) were tuned in the Castle Suite and stage 1: 16-pixel motion-field cells, 8-pixel ambiguity tolerance, 64-pixel "too far" cut-off, a camera cut at 32 pixels of average motion. Untested: Hewie and the stalkers at speed, particles, water, the panic effects, cutscenes with fast cuts, the House of Truth. Failure modes are ghosting or a frame shown twice, never a crash or a wrong game state (game memory is untouched). The picture itself has no hash gate: the copies are "not bit-reproducible between runs (18 of 4.6 M pixels differed by one level at x4)". A tolerance-based image gate (`HG_FRAMES_SHOWN` + `framecmp.py` with a threshold) is missing and should be added so GPU changes to the picture path are caught.

Latency: 60 fps shows each real frame one VBlank later than before, plus up to two pictures waiting in the queue (`fq_keep = 2`), plus the pacing sleep. Nobody has measured end-to-end input latency. It should be measured once (phase 1) so that the in-game "frame rate" option can state the trade-off honestly.

### 2.5 VU1: what "whole-program compilation" has to preserve

The AOT code (`vu1gen.py`) is already decode-free, but every pair still runs the interpreter's bookkeeping in `step()` (`vu1_core.h:421-445`): `vcyc`, `stall_cycles()` over `vf_rdy[32][4]`, `note_writes()`, the flag queue test, `q_wait`/`p_wait` decrements, the `vi[0]`/`vf[0]` reset, `prev_vi` for the branch rule, `branch_pending`, the runaway bound, and a `switch` dispatch whenever the next pc is not the next label. VU1 is about 6.4 of 8.4 ms per VBlank in the Castle Suite (64 % of the EE thread).

What is actually timing-visible in the results: Q and P arrival (DIV/SQRT 7 cycles, RSQRT 13, EFU 11-54, counted in cycles including stalls), and nothing else while `HG_VUFLAGDELAY=0` (the default; the game reads only clip flags). Stalls change results only through Q/P. So "timing worked out at build time" reduces to proving, per program and per Q/P read, whether the value has arrived; the plan in phase 2 does exactly that and keeps dynamic cycle counting only where the proof is inconclusive. `HG_VU1CHECK` compares against `vu1_ref.c`, which keeps the full stall model, so a wrong proof is caught.

A VU1 thread changes ordering: today PATH1 (XGKICK), PATH2 (VIF DIRECT) and PATH3 (GIF channel 2, FIFO) are serialized by the single EE thread. The plan routes all GIF traffic through the one new thread to keep the order. Also note `runtime.c:24` keeps a separate `vumem[]` for EE accesses to 0x11000000-0x1100FFFF; `vu1_mem`/`vu1_code` are different arrays. If the game ever touched VU1 memory directly through that window it would be invisible to VU1. The run logs' hardware-register statistics can confirm there are no such accesses (a one-minute check in phase 2).

### 2.6 Coverage of the game is small

Reached: logos, menus, movies, stage 1 (`ST_02A`, `ST_013`, `ST_00D`), Castle Suite (`ST_021`), `ST_010`, `ST_020`. The checklist has about 125-131 rooms across five chapters (`docs/rooms-checklist.md`), four stalkers, the Chaos Forest, the Water Tower and the House of Truth. Consequences:

- **Serial primitives** (a primitive that reads what it writes, drawn by one GPU invocation in CPU order, `gsvk.c:237`): "none in the test ranges, but other rooms may have some". One full-screen serial sprite would cost tens of milliseconds. A counter already exists (`vk_serial`); the soak over new recordings must watch it.
- **Effect passes at scale**: the render-target offset rule for the copies (`gsvk_tex_is_rt`, `gs.c:1172`) was derived from the Castle Suite's passes; `hardware.md` lists about 40 effect functions (CT16S aliasing, COLCLAMP wrap, Z24 as texture, DATE stencil). Copy 0 stays exact whatever happens; the picture may band in a pass not yet seen.
- **Saving is untested** (`handoff.md`, `phase1-runtime-notes.md`): the MCSERV write path exists but no save has been made from this build. LOAD GAME works from a PCSX2 save. This is the biggest single playability risk and costs an hour to test once a clock is reachable (the Castle Suite has one).
- Reverb depth never changed in reached rooms; the `ST_020` door specks await the user's PCSX2 decision.

### 2.7 Robustness and limits in a release

- Any unknown indirect target, bad jump-table target, unimplemented instruction, `break 7`, or deadlock calls `stop()` and exits with a report. Fine for development; a release needs the same report written to a file plus a message box, and a way to send it back.
- Fixed sizes: 64 guest threads, 256 semaphores, 16 pad sockets, `PRIM_MAX 32768` primitives per GPU submission (a forced `gsvk_finish` stall when full), `UP_BYTES 32 MB`, `RING 2^18` GS entries, `vif.buf[1100]`.
- The GPU path requires `shaderFloat64` (triangle Z interpolated in double, as `gs.c` does). NVIDIA consumer parts run fp64 at 1/64 rate; AMD RDNA at 1/16; some Intel drivers refuse it. Only the RTX 5090 has been tested. Z could later be done in exact integer arithmetic on both sides, but that is a re-verification of the whole GS.
- `HG_HOSTPROF=1` with the GPU modes hangs at boot (the sampler suspends threads inside the driver). Profiling the GPU-mode CPU side needs another profiler (e.g. Windows ETW/WPA or `HG_SPEEDLOG` subsystems only).
- The UI thread spins the last 1.5 ms before each present and the game thread sleeps with `SDL_Delay` (1 ms granularity). The present loop is free-running at 59.94 Hz, not tied to the display's vsync: on a 60.00 Hz monitor one frame is repeated about every 17 s; on 120/144 Hz displays the cadence is uneven. A swapchain (phase 1) makes vsync and VRR possible; the 59.94-vs-display-rate policy is a user question.
- Audio: the 100 ms queue cushion that fixed the ticks is also 100 ms of latency. Acceptable for now; worth an option later.
- Linux: the `ucontext` path in `compat.c` has not been built since the move to Windows on 2026-10-13; the README still promises Linux.

### 2.8 Smaller items noticed

- `rec_table` lookup is a binary search per indirect call (`rec_call`), 9,183 `jalr` sites in the game; fine today, but a direct-mapped cache would be a cheap EE win if it ever shows in a profile.
- `host_vblank` resets pacing when more than 0.25 s behind ("do not try to catch up"), which is right for play but means recordings made in a stalled window still replay fine only because emulated time is independent of the host. Good design; keep it.
- `rr_envs` (the environment recorded with a session) lists 7 variables; GPU and settings-related variables are not recorded. Once settings exist, the settings in effect must go into the recording header.
- The README's status paragraph is a week out of date ("not playable yet", "half of real time on a 2-core VM").
- The generated `funcs_entered.txt`, `hostprof.txt`, `play.log` sit in `recomp/` untracked; harmless, but a `recomp/out/` folder would be tidier.

### 2.9 What is in good shape

The exactness discipline (oracle renderer, lockstep checks, recorded replays, A/B/B/A timing) is better than most emulator projects have. The GPU design (exact copy 0 for everything the game reads, N x N copies for the picture only) is the right separation for a port that must stay faithful. Sound, movies and the IOP are the game's own code and verified bit-exact where a reference existed. Determinism of emulated time is what makes everything else checkable. None of this should be traded away for speed.

---

## 3. The plan

### 3.0 Conventions

- **Gates** are named once here and referenced by number. All must pass before a step is called done, with the results pasted into the step's commit message.

| Gate | Check | Pass |
|---|---|---|
| G1 | `runhash.sh ... 4000` vs `hashA.md5` (software GS) | 702/702 identical |
| G1g | the same with `HG_GSVK=1`, then with `HG_GSVK_SCALE=N`, then with `HG_GSVK_INTERP=1` | 702/702 each |
| G2 | LOAD GAME to the Castle Suite hashes (742 frames) | identical |
| G3 | `soak.sh TAG [env]` over `recordings/20261014-200745` (and every later recording) | all frames identical to the software GS; `vk_serial` count noted |
| G4 | `HG_VU1CHECK=1` over G1's route and over the soak route | 0 mismatches |
| G5 | `vkcheck.sh` and `vkbatch.sh` over the four ranges | 0 differing words, 0 differing batches |
| G6 | `HG_VI_SELFTEST=1` over G1's route and the soak route | 0 differ |
| G7 | `wintest3.sh TAG [env]` + `pacing.py`, Castle Suite, window | SPEED 100 %; present intervals median 16.68 ms, p5-p95 within 16.5-16.9 (8.34 at 120); 0 repeats, 0 out of order (camera cuts excepted) |
| G8 | `abba.py` for every timing claim | the claimed gain exceeds the drift |
| G9 | `HG_UNPACKCHECK=1` for any `dmac.c` change | 0 mismatches |
| G10 | picture gate (new in phase 1): `HG_FRAMES_SHOWN` dumps over fixed VBlank ranges compared with `framecmp.py` against a reference set | exact for copy-0 pictures; at most 1 level, at most 0.01 % of pixels, for upscaled and in-between pictures |
| G11 | build from clean (`make clean && make -j8`) | succeeds; time noted |
| G12 | windowed soak of N minutes with `HG_AUDIO_MUTE=1` | no crash, no deadlock, no audio underrun, speed 100 % |

- **Effort** is in agent-days: one unattended Opus session of about 6-8 hours including test runs. Ranges are wide on purpose.
- **Rules** from `docs/next-session.md` section 4 apply to every step: local git only, no game data committed, identical frames for speed-ups and refactors, no game window unless asked, muted tests.

### 3.1 Phase 1: 4K and 60 fps together at full speed (the user's item A)

Goal: `HG_GSVK=1 HG_GSVK_SCALE=5 HG_GSVK_INTERP=1` at 100 % speed in a window on the 4K screen, with the same pictures as today, and 120 fps unparked.

**Step 1.0: freeze the baseline (0.5 day).**
- Copy `hashA.md5`, the Castle Suite hashes, the soak reference hashes, `soak.sh`, `wintest3.sh`, `vkcheck.sh`, `vkbatch.sh`, `pacing.py` into `tools/verify/` (adjust the hard-coded scratch paths to `$HG_SCRATCH`), with a `tools/verify/REFERENCES.md` that names the commit each reference was made with. Commit.
- Record `HG_PRESENTLOG` pacing and `HG_SPEEDLOG` numbers for x4/x5 with interpolation (today's 74 %/59 %) as the "before".
- Add GPU timestamps per phase in `gsvk.c` (exact batches, copies dispatches, replay submissions, compose) and print them in `gs_dump_stats`. The query pool has 2 slots per command buffer today; make it 8.
- Add the picture gate G10: a script that runs `HG_FRAMES_SHOWN=dir:FROM:TO:PERIOD` over two ranges (stage 1 and the Castle Suite) and compares with `framecmp.py`.
- Gate: G11; nothing else changes.

**Step 1.1: asynchronous in-between pictures (0.5-1 day, optional quick win).**
- In `vi_vblank`, replace the `gsvk_finish()` + `gsvk_compose_fetch` after the replays with `gsvk_submit()` and remember `{id, slot, w, h}` per in-between picture, as `real_pic` already does for the real picture (`gs.c:2383-2396`); fetch at the next VBlank before emitting. Pictures then come out one VBlank later than today; `vi_present` must shift the sequence accordingly (in-between of frame F, then real F, both one VBlank late) so the order rule still holds.
- Gate: G1g, G6, G7 (expect 16.68 ms unchanged, in-between share 0.5), G10. Measure speed at x4/x5; it is probably still short of 100 % at x5 because the CPU copies remain. If x5 reaches 100 % here, step 1.2 is still worth doing for latency and the 4096 limit, but it drops in priority.

**Step 1.2: present from video memory (3-5 days). This is the real fix.**

Design, for the engineer:
1. *Instance and device.* `gsvk_init` creates the instance with the extensions `SDL_Vulkan_GetInstanceExtensions` asks for (`VK_KHR_surface`, `VK_KHR_win32_surface`) whenever SDL is compiled in, and the device with `VK_KHR_swapchain`. Bump `apiVersion` to 1.2 and enable `timelineSemaphore` (Vulkan 1.2 feature; the RTX 5090 has it). Pick a queue family with compute and present support (`vkGetPhysicalDeviceSurfaceSupportKHR` needs a surface; on NVIDIA family 0 has graphics, compute and present, and 16 queues). Create **two queues** from that family: queue 0 for the GS thread (today's work), queue 1 for the UI thread. If the family has one queue, fall back to one queue and a mutex around every `vkQueueSubmit`/`vkQueuePresentKHR`. Initialization order: the window is created on the UI thread at the first VBlank, `gsvk_init` on the GS thread at the first primitive; make surface and swapchain creation lazy on the UI thread once both exist, and keep the SDL-renderer path as a fallback (`HG_PRESENT=sdl`, also used by the software GS and when Vulkan is absent).
2. *Picture slots.* Replace the host-visible `hrb` read-back buffer with 6 `VkImage` slots (`R8G8B8A8_UNORM`, `TRANSFER_DST | TRANSFER_SRC`, sized W x H, re-created when the size changes). `gsvk_compose_rec(slot)` keeps writing the composed picture into the `dvram` out region, then records `vkCmdCopyBufferToImage` into the slot image and a layout transition to `TRANSFER_SRC_OPTIMAL`. Each GS-thread submission signals a timeline semaphore `gs_done` with its submission id (already a monotonically increasing `n_sub_id`). The picture queue entry (`fq`) becomes `{slot, submission id, w, h, tag}` instead of 28 MB of pixels.
3. *The UI thread's present.* Every 1/59.94 s (or on vsync, see 5): take the next entry; `vkAcquireNextImageKHR`; record on queue 1: wait `gs_done >= id` (timeline wait in the submit), transition the swapchain image, `vkCmdBlitImage` slot -> swapchain with `VK_FILTER_LINEAR` into the letterboxed 4:3 rectangle (same rule as `SDL_RenderSetLogicalSize(640, 448)` today: the PS2 picture is stretched to 4:3 whatever its pixel size), blit the overlay (a small `180 x 46` image uploaded only when the F3 text changes, scaled as today), transition to present, signal a binary semaphore, present. Signal a second timeline `ui_done` with the entry's sequence number after the blit so the GS thread never re-uses a slot the UI thread has not consumed (`gsvk_compose_rec` waits on `ui_done` for that slot's previous use).
4. *Dumps and tests.* `HG_FRAMES_SHOWN`, `HG_FRAMES_HR` and the self-test still need pixels: keep a read-back path (slot image -> host buffer) behind those switches only.
5. *Present mode.* `VK_PRESENT_MODE_FIFO_KHR` (vsync) by default, `MAILBOX` as an option. With FIFO the UI thread no longer needs its spin loop: `vkQueuePresentKHR` blocks at the display's rate. The 59.94 Hz emulated clock against a 60.00 Hz display is a user decision (section 4, Q14); implement both: free-running at 59.94 (today's behaviour) and "lock the emulated VBlank to the display's refresh when within 0.5 %".
6. *Window events.* `VK_ERROR_OUT_OF_DATE_KHR`/`SUBOPTIMAL` on resize and F11 -> re-create the swapchain; DPI awareness stays as it is.
7. *Development aids.* Run with `VK_LAYER_KHRONOS_validation` during this step (package `mingw-w64-ucrt-x86_64-vulkan-validation-layers`, needs the user's approval to install); zero validation errors is part of the gate.

Gates: G1g, G5 (unchanged, copy 0 untouched), G6, G7 at native, x4, x5 with interpolation (expect 100 % at x5), G10 against step 1.0's reference pictures, G12 for 20 minutes at x5 + 60 fps. Also measure and record: GPU ms per phase at x5 + 60 fps, CPU ms per VBlank on the GS thread, and end-to-end input latency once (photodiode is overkill: log the VBlank at which a scripted Cross press is injected and the present time of the first picture that shows its effect; two or three numbers in the doc are enough).

**Step 1.3: GPU cost, only if 1.2 does not reach 100 % at x5 (1-3 days).**
Candidates, in order of expected gain per risk, each verified by G5/G10 and timed with the per-phase timestamps:
- The copies build re-runs the per-tile primitive scan for every copy (workgroup z); compute the tile mask once per batch (a tiny pre-pass writing a bitmask buffer) and have every copy read it.
- The `Prim` struct is about 100 words and every invocation copies it into a global `p`; split into a hot part (geometry, formats, test/blend state) and a cold part read only when needed.
- Coarse binning at 32 x 32 before the 8 x 8 tile marking when a batch has more than 256 primitives.
- Compose directly at display size into the slot image (skip the out region copy).

**Step 1.4: 120 fps (0.5 day).** Re-measure `HG_GSVK_INTERP=2` after 1.2; `fq_keep`, slot count (6 is enough for 3 in-between + real + 2 in flight) and the 8.34 ms pacing. Gate: G7 at 8.34 ms, G6.

**Step 1.5: tidy (0.5 day).** Remove the 4096 px limit and the 64 MB static buffers; update `docs/gpu-renderer.md` ("Window" and "Not yet" sections), `play.bat`, `handoff.md`.

Dependencies: none. Risks: queue synchronization bugs (validation layers), driver quirks on fullscreen transitions, the display-rate policy. Effort: 5-9 agent-days.

### 3.2 Phase 2: CPU part 2, VU1 whole-program compilation and the VU1 thread (item B)

Goal: VU1 cost per VBlank cut by 2-3x with identical results, then VU1 off the EE thread, so the EE thread has headroom for everything later (interpolation logging, settings, mods, other machines).

**Status (2026-10-15): steps 2.0 and 2.1 done, step 2.2 deferred by Q2** (VU1 7.3 -> 2.8 ms per VBlank, EE thread
9.8 -> 5.3 ms in stage 1; 0 AMBIGUOUS consumers; details, gates and the thread design in
`docs/renderer-performance.md`, last section).

**Step 2.0: measure and prove (1 day).**
- Baseline with `HG_SPEEDLOG=300` in stage 1 and the Castle Suite: VU1 ms per VBlank (today 6.4-7.3), runs, instructions, AOT handovers (expect 0).
- Write `tools/vu1timing.py` (reads the 19 programs through `vu1gen.programs`): build the control-flow graph per program (B, BAL, IBxx, JR/JALR, E-bit), then for every Q consumer (an upper op with the Q broadcast source, `MULq`, `ADDq`, ...), every P consumer (`MFP`) and every `WAITQ`/`WAITP`, find on every path the nearest preceding DIV/SQRT/RSQRT/EFU and the minimum and maximum elapsed cycles: instructions on the path plus the possible stall range (0 to 3 cycles per pair from the 4-cycle VF latency, up to 13 from a busy FDIV). Classify each consumer: **SAFE** (a WAITQ/WAITP lies between, or minimum cycles >= latency), **OLD** (maximum cycles < latency: the consumer certainly sees the previous Q/P), **AMBIGUOUS** (otherwise). Print the table. Also list every `JR`/`JALR` (dynamic targets) and every MSCNT entry point (the pair after an E-bit pair, plus 0 and every MSCAL immediate the game uses).
- Expectation from the disassembly notes (`hardware.md`): compiler-generated VCL code with `DIV ... WAITQ ... MULq`, so everything is SAFE or OLD. If an AMBIGUOUS site exists, step 2.1 keeps dynamic cycle counting on that path segment only (a counter from the issue to the consumer with the same `stall_cycles` arithmetic, specialized to the registers involved).
- One-minute check for 2.5's note: grep a run log's hardware-register table for accesses in 0x11000000-0x1100FFFF; expect none from game code.

**Step 2.1: generator v2 (3-5 days).**
- `vu1gen.py` emits, per program, one C function with one label per pair and straight-line code per pair: the upper op (the existing always-inline helpers on constant `Dec`, so semantics stay the interpreter's), the lower op, the I-bit immediate, then control flow as `goto`: a branch emits its delay-slot pair inline on both arms and jumps; the E-bit emits the next pair and returns 1; `XGKICK` calls `vu1_kick`; the `prev_vi` branch rule becomes static (the previous pair is known on every edge; the only dynamic case is a branch target reached through JR/JALR, where the delay-slot pair is still static, so it is known too).
- Dropped from the compiled code when 2.0 proves it: `vcyc`, `stall_cycles`, `note_writes`, `vf_rdy`, the flag queue (only when `flag_delay == 0`), `q_wait`/`p_wait` decrements (Q/P commit becomes an assignment at the proven point: at the WAITQ, or at the first SAFE consumer; OLD consumers read the old value by construction). Kept: `vi[0]`/`vf[0]` invariants (write them only where a pair could violate them: a `dest` including vf0 or `it == 0`, decided at generation time), `n_insns` as a per-run addition, the runaway bound as a per-loop back-edge counter.
- Entry: `aot_run_NN(RunSt *)` switches on `s->pc` over the known entry points (0, MSCAL immediates, the pairs after E-bit pairs); anything else hands over to the interpreter as today. JR/JALR to a pc that is not a label also hands over. Handover keeps working because the state is still the shared globals and the interpreter can continue from any pc (it must then run with its own stall model from a drained pipeline, which is what the reference does at run start; document that a handover in mid-program is only exact if no Q/P is pending, and count handovers: the gate requires 0).
- Compile time: emit one function per program (no 64-pair chunks); the generated code per pair is small now, so the earlier 14-minute compile should not return. Gate G11 requires the whole build under 3 minutes.
- Keep `HG_VU1AOT=0` and the old interpreter; keep `vu1_ref.c` untouched (it is the oracle).

Gates: G4 (2 M runs on G1's route and the full soak route: the lockstep oracle keeps the full stall model, so any wrong proof shows as a Q/P mismatch), G1, G2, G3, G8 (`abba.py` with `HG_GSDRAW=0` to isolate the EE thread; expect VU1 6.4 -> 2-3 ms), G11.

**Step 2.2: VU1 on its own thread (3-5 days). Decide after 2.1's numbers (section 4, Q2).**
If 2.1 brings the EE thread to about 4-5 ms per VBlank, the thread is headroom for other machines and later features rather than a need on the 9800X3D. If done:
- A VIF/VU1 thread fed by a ring from the EE thread, in the style of the GS ring (`gs.c:2454-2535`). Entries: VIF1 payload runs (the qwords copied at kick time, because the game may rewrite its buffers after a "completed" DMA; today's synchronous DMA guarantees that), GIF channel 2 payloads and `gif_fifo_write` qwords (PATH3), and the VBlank marker. **All GIF traffic goes through this one thread**, so PATH1/2/3 order is the EE's issue order, and the GS ring keeps a single producer.
- `dma_run` on the EE thread walks the chain exactly as now but copies instead of executing; the `vif` state machine, `vu1_run`, `vu1_kick`, the interpolation hooks and `HG_VU1CHECK` move to the new thread (`check_run` is self-contained).
- Back-pressure: the EE may run at most 2 VBlanks ahead of the VU1 thread (the existing `lag_ok` rule, applied in `gs_vblank` to both consumers); the ring needs to hold a few VBlanks of VIF traffic (measure qwords per VBlank with `dma_dump_stats`; the title screen moved 27 M qwords in 75 s, about 6 MB per VBlank, stages less).
- Hardware-register reads the EE makes about the VU side (`VIF1_STAT`, `VPU_STAT`, `D1_CHCR.STR`) already answer "idle/done" (`hle.c:633`, `626`); nothing changes for the game. DMA completion interrupts stay VBlank-driven (`dma_pending`).
- `HG_VU1DUMP`, `HG_CLIPLOG`, `HG_VTXLOG` keep working (they run on the thread that executes VU1).
Gates: G1, G1g, G2, G3 (identical frames prove the order held), G4 (now on the new thread), G9, G8 (EE thread ms), G7, G12 for 30 minutes.

**Step 2.3: the rest of the EE thread (optional, 1-2 days).** After 2.1/2.2 the EE profile is game code + the VU0 macro library (`interp.c` `ee_cop2`, called from the 26 libvu0 leaves) + IOP/SPU2 (about 1-1.5 ms). Profile with `HG_HOSTPROF=1` in software-GS mode (the GPU hang) or with the Windows Performance Recorder; translate the libvu0 leaves to SSE in `interp.c` if they show. Gate: G1-G4, G8.

Dependencies: none on phase 1 (different files), so 2.0 can run while phase 1's long tests run. Risks: a subtle ordering bug in 2.2 shows up only in a room with PATH3 traffic (the end-of-frame blit and the boot clear are the known PATH3 users; the soak covers both). Effort: 6-12 agent-days.

### 3.3 Phase 3: in-game settings (item C)

**Status (2026-10-15): done** with option (i), the user's choice (Q6): native extra rows in the game's Options page,
found practical by step 3.0 (`docs/options-menu.md`: the page is two hard-coded functions, hooked; every picture,
text, sound and the dialog are the game's own). Settings module, `hg.ini`, live changes and recordings as planned in
3.1; gates in `docs/handoff.md`.

Goal: the game's own Options page offers Resolution (upscaling factor), Frame rate (30/60/120), Keyboard mapping and Controller mapping; values live in a host config file; recordings still replay identically.

**Step 3.0: research the Options menu (1-2 days; produces `docs/options-menu.md` and a decision).**
- Method that worked for the frame loop (`docs/frame-timing.md`): diff `funcs_entered.txt` call counts between a run idling on the main menu and a run idling inside Options, then `HG_TRACE` on the candidates. Identify: the menu's item table (count, label ids or strings, per-item callbacks or vtable slots), the draw function (how labels and values are rendered: which font texture, which GIF packets), the input handler (cursor up/down, left/right value change, confirm/cancel), and where the existing values live in RAM and in the system save (Vibration, sound mode, screen adjust, subtitles, ...).
- Decide with the user between (i) **native extension**: add rows to the game's table and callbacks by hooking the table-reading functions (`set_hook`), rendering through the game's own draw path; (ii) **hybrid**: one new row per added page ("Display", "Controls") whose confirm action opens a host-drawn page composited over the frame in the game's style (font sampled from the game's own font texture or a matching bitmap font), input taken from the pad state; (iii) **host overlay only** (an F-key page, no menu change). Recommended default: (ii), because it needs one hook point per page, cannot corrupt game state, and works in every menu context; (i) only if 3.0 finds a clean table-driven menu.

**Step 3.1: the settings module (2 days).**
- `recomp/settings.c/.h`: a typed struct with every host-facing option (display: scale 1-8 or "auto for this monitor", frame rate 30/60/120, present mode, fullscreen/borderless, aspect handling; controls: keyboard map, controller map, dead zones, stick-to-dpad; audio: latency, master volume if the host mixes; vibration strength); defaults; `hg.ini` next to the executable (portable) or `%APPDATA%\HauntingGroundRecomp\` (decision Q6); environment variables keep overriding the file so every test script works unchanged; one `L()` line at start listing the effective settings.
- Replace the host-facing `getenv` sites (`host_sdl.c`, the `HG_GSVK*` reads in `gs.c:1191-1205`, audio) with the struct; leave the diagnostic switches as they are (a later phase lists them).
- Live changes: scale -> `gsvk_reconfigure()` (finish the GPU, free and re-create `dvram`/slots, re-upload all pages: do it on the GS thread at a VBlank boundary; the game's copy 0 is re-uploaded from `vm` because the GPU memory is lost); frame rate -> toggle `vk_interp` at a frame boundary (the log and snapshot sets must be re-initialized); fullscreen -> at once; mappings -> at once.
- Recording: write the effective settings into the recording header (`rr_envs` + settings); `HG_PADPLAY` applies them and warns on differences, as it does for the environment.

**Step 3.2: the pages (2-4 days depending on 3.0's decision).**
- Display page: Resolution (native, x2 ... x6, auto), Frame rate (30, 60, 120 with a note that 60/120 adds one frame of display delay), Fullscreen, Vsync/present mode. Controls page: an action list (D-pad, Cross, Circle, Square, Triangle, L1/R1/L2/R2, L3/R3, Start, Select, left stick, right stick) with "press a key" and "press a button" capture, defaults, and PlayStation names/glyphs when SDL reports a PS4/PS5 controller, Xbox names otherwise.
- Determinism rule: our pages never write game RAM. Input while a host page is open is not forwarded to the game (the game sees the pad idle), and the page's own input is recorded in the session too (it is pad state) so a replay shows the same pages; settings changes made during a recording are replayed from the header plus the recorded events.

Gates: G1 (settings default = today's behaviour), G3 (the old recording replays identically), a new recording made through the Options pages replays identically (G3 on it), G7 at each frame-rate setting, G12. Manual: the user's checklist (Q5/Q6 answers).

Dependencies: phase 1 (the scale change path should be the swapchain one). Risks: the menu's C++ structure may make (i) impractical; glyph rendering in the game's style takes iteration. Effort: 5-9 agent-days.

### 3.4 Phase 4: native DualSense (item D)

Goal: a DualSense over USB or Bluetooth works out of the box with PlayStation names, the game's vibration reaches the controller, and the extras the user wants (light bar, triggers, touchpad) are implemented.

**Step 4.0: facts and plumbing (0.5 day).** SDL 2.32.10 (installed) has the HIDAPI PS5 driver on by default: `SDL_GameControllerGetType() == SDL_CONTROLLER_TYPE_PS5`, `SDL_GameControllerRumble` (two motors), `SDL_GameControllerRumbleTriggers`, `SDL_GameControllerSetLED`, `SDL_GameControllerSendEffect` (raw DualSense effect report: adaptive triggers), `SDL_GameControllerGetTouchpadFinger`, sensors (gyro/accel), battery level, player index. Over Bluetooth SDL switches the controller to enhanced reports itself. Note the Steam Input conflict: if Steam is running with PlayStation configuration support on, it owns the device; document it.

**Step 4.1: the game's vibration (1-2 days).** The game sets the DualShock 2 actuators through libpad2/libdbc; the HLE answers the `SRData` command (`iop_pad_mc.c:251`) with "stable" and ignores the payload. Find the actuator bytes: run a scene that vibrates (Options has a Vibration setting; panic mode and hits vibrate) with `HG_DUMPSID=80001300` and watch the 0x8000131a payloads and the per-VBlank pad command; cross-check the libdbc code (0x1ee740-0x1ef968) for `SetActAlign`/`SetActDirect` equivalents. Implement: parse small-motor on/off and big-motor 0-255 per VBlank, map to `SDL_GameControllerRumble(pad, big * strength, small ? 0xFFFF * strength : 0, 40 ms)` refreshed every VBlank while active, honour the game's own Vibration option and the settings' strength. Add `HG_RUMBLELOG=1` so a replay prints the actuator sequence; the sequence must be identical across replays (gate).

**Step 4.2: names, mapping, multiple controllers (1 day).** PlayStation names in the mapping page for PS4/PS5 types; default map for PS5 = identity (Cross = Cross); hot-plug of several controllers, the first that presses a button is player 1; dead zones from settings; `SDL_GameControllerAddMappingsFromFile` for a community `gamecontrollerdb.txt` next to the exe.

**Step 4.3: extras per the user's answers (1-3 days).** Light bar: a static colour, or driven by game state (Fiona's panic level needs an address in RAM: find it by diffing RAM dumps while panic rises; `HG_RAMDUMP` exists). Touchpad: click = Select by default. Adaptive triggers: only if the user wants a specific effect (e.g. resistance on L2/R2 when they have no in-game role, probably not). Gyro and the speaker: no (SDL has no DualSense speaker API).

Gates: manual test by the user with the real controller; `HG_RUMBLELOG` identical over two replays; G3 (nothing in the game changed); G12.

Dependencies: phase 3 for the mapping UI and the strength setting (4.1 can be done before, with an environment switch). Effort: 3-6 agent-days.

### 3.5 Phase 5: verification kit and repository hygiene (runs alongside, 1-2 agent-days)

- `tools/verify/check.sh {hash|gpu|vu1|soak|window|all}` running the gates with references from the repo, printing PASS/FAIL and the numbers; a `docs/verification.md` that replaces the scattered instructions; the `recordings/` manifest (which recording covers which rooms; the user's recordings are regression inputs with their permission, Q17).
- Move run-time outputs (`funcs_entered.txt`, `hostprof.txt`, `play.log`, crash reports) to `recomp/out/`; update `.gitignore`.
- README status rewrite (one paragraph that is true today).

### 3.6 Phase 6: readable code

Two different things: the hand-written runtime (refactor, mechanical, verified identical) and the game code (targeted decompilation into named C, verified by replay).

**Step 6.1: runtime refactor (3-5 agent-days, best right after phase 1 so the GPU display code settles first).**
- Split `gs.c` into `gs_mem.c` (local memory, swizzle tables, transfers), `gs_raster.c` (scalar pipeline, primitives), `gs_avx2.c`, `gs_workers.c`, `gs_gpu.c` (page tracking, batches, checks, bridge to `gsvk.c`), `gs_interp.c` (frame interpolation), `gs_display.c` (composition, picture queue, dumps), `gs_thread.c` (ring, VBlank), with a `gs_internal.h` for the shared state. Same for `hle.c` (kernel vs timers vs SIF/IOP command layer vs disc).
- Reformat to at most 120 columns, one declaration per line for file-scope state, one statement per line in anything that is not a table; keep the explanatory comments. No behaviour change: G1, G1g, G2, G3, G4, G5, G6, G8 (no slowdown beyond drift). Do it in several commits, each gated, so a mistake is bisectable.
- Replace the 68 environment switches by `settings.c` (host-facing) and a `debug.c` table (diagnostics) with one documented list; keep the `HG_*` names as the keys.

**Step 6.2: a symbol map (1 agent-day to start, then ongoing).** `data/symbols.txt` (address, name, one-line note; addresses only). Seed it from the docs: libgraph `sceGsSyncPath` 0x10c9a0, the frame driver 0x1beef0, the VBlank wait 0x10d200, the SE play function 0x122c20, the texture manager entry points 0x1bfec0/0x1bb470, the effect dispatcher 0x1b5ec0, the pad class vtable 0x46adb0, `main` 0x20d860, the player object pointer 0x44f818. The generator emits `/* name */` after the function header and a `gen/names.h` with `#define`s; `rec_call` failures, crash reports, `HG_TRACE` and `funcs_entered.txt` print names.

**Step 6.3: the targeted decompilation workflow (set-up 2 agent-days; then per function).**
- Layout: `recomp/game/<area>.c` (e.g. `frame_loop.c`, `camera.c`, `pad.c`, `options.c`) with a typed view of EE RAM through accessor macros (`GPTR(struct Player, 0x44f818)` style), float helpers that reproduce the EE FPU semantics the generated code uses (`fpu_clamp`, `ee_divs`, `ee_cvtw`: an `ee_math.h`), and the same side effects.
- Linking: `data/replaced.txt` lists the addresses the generator must not translate; `bulk2.py` emits their `rec_table` entries pointing at the hand-written functions, so calls stay direct. `HG_DECOMP=0` builds the recompiled copies too (a second table) for A/B.
- Verification: (a) every recording replays to identical frames and identical VU1 kick hashes (G3 extended with `HG_VU1CHECK`'s kick hash per frame); (b) **differential mode** `HG_DECOMPCHECK=1`: for a replaced function, run the recompiled copy on a snapshot of RAM and the registers, then the hand-written one on the live state, and compare every written word and the return value (the pattern of `HG_UNPACKCHECK`); this catches differences a replay would not reach.
- Order of work (see Q11): the frame loop (0x1beef0, 0x1b86a0, 0x1b8750, 0x10d200), the pad class, the options menu (phase 3 may already need it), the camera update and projection set-up, the player update, the render set-up and the effect dispatcher plus its ~40 passes (needed to understand them at scale), the texture manager (needed for texture replacement), the sound command wrappers. Expect hours to a day per function and several weeks for this list; each function is its own gated commit.

### 3.7 Phase 7: the modding layer

**7.1 Data overlay (2-3 agent-days).** The game reads files through CRI ROFS from the CVM directory, which it reads by sector itself (`cd_read` in `hle.c:270`). Implement a virtual disc: parse the CVM directory at start (`tools/verify/cvm.py` already does), and for every file in `mods/<name>/DATA/<path>` serve the replacement's sectors instead, either in place (if it fits the original's extent) or from a relocated LSN range beyond the CVM's end with a patched directory. Load order from `mods/order.txt`. Gate: with no mods, G1-G3; with a mod that replaces one texture file, the replaced texture shows and nothing else changes.

**7.2 Texture replacement (2-4 agent-weeks; the hardest piece).** The constraint is the exact GS: copy 0 must keep sampling the native texels, because game logic and effects read local memory. The design that fits: replacements affect the **copies only** (the picture), like the sub-pixel offsets do today.
- Identification: hook the load-time registration (`FUN_001bfec0`) and the cache re-upload (`FUN_001bb470`) (both documented in `docs/textures.md` section 4) to tag the 8 KB pages an upload fills with a texture id = hash of (file, blob offset, entry index, CLUT/CSA); keep a per-page id table on the CPU and in a GPU buffer.
- Rendering: the copies shader, when a primitive textures from pages with a known id that has a replacement, samples a high-resolution image (a bindless array or an atlas texture) at the same u/v (normalized by the texture's size), with the same wrap/clamp, bilinear and alpha semantics (alpha scaled from 0x80 = opaque), and still runs the exact per-pixel tests and blends. Palette variants (CSA, up to 12 per UI texture) become separate replacement images or an indexed image plus palette. Render targets and runtime CLUT effects keep the native path.
- Tooling: `tools/texdump` (from `tex_hg.py`) writes every texture of a stage with its id as the file name; a mod ships PNGs with those names.
- Gates: G1-G3 unchanged (copy 0); a visual review per room; `vk_serial` unchanged.

**7.3 Function overrides for mods (1-2 agent-weeks).** A small C API (`hg_mod.h`: hook by name or address, read/write guest memory, settings, log) and mods as DLLs in `mods/` loaded at start, or Lua for scripts (decision Q9; GPL implications for DLL mods are a licensing question, Q12).

**7.4 Audio replacement (later).** The offline sample replacement exists (`tools/formats/vagenc.py`); at run time, intercept the bank data on its way to IOP RAM and substitute samples; the README's "Hewie with real dog recordings" is this plus a recording session.

### 3.8 Phase 8: effects at native 4K

What the copies already give: every pass is rendered N x N times with sub-pixel offsets, so the picture is a true 5x supersample of the game's own rendering, including the half-resolution bloom/blur chains (their blur radii stay in native pixels, which is the intended look). What remains native: textures (7.2), movies (512 x 448 and 256 x 224 Sofdec; replacement videos are a data mod), fonts and UI (7.2). "Native 4K shaders" for the ~30 passes are only worth writing where the sub-pixel approach shows an artifact (the banding class fixed by the render-target offset rule). Plan: a room-by-room review at x5 against PCSX2 at 4x internal resolution (recommendation 3, Q3), log every pass that looks wrong, fix the offset rule where possible, and only then rewrite a pass as a dedicated shader behind a setting. Effort: review 2-3 agent-days per chapter; rewrites 1-3 days each.

### 3.9 Phase 9: play coverage, saving, soak

- **Saving** (0.5 agent-day, early): save at the Castle Suite clock from our build, check the files under `memcards/port0`, reload with LOAD GAME, load the same save in PCSX2 (export with `ps2mc.py` into the reference instance's card; the user's own card is off limits), then the reverse. This should happen as soon as the user allows a windowed run or by script.
- **Rooms**: a recording per chapter (the user plays, or the explorer), each becoming a G3 input; the PCSX2 comparison per room per `rooms-checklist.md`; the door specks decision (Q4); reverb when a room turns it on.
- **Soak**: nightly replay of every recording in software and GPU modes; a 2-hour windowed run muted at x5 + 60 fps; memory growth watched (the interpolation logs grow with `vi_grow`; the picture slots are fixed); explorer runs of 100,000 VBlanks with seeds.

### 3.10 Phase 10: launcher and packaging (2-3 agent-weeks)

Users will not have MSYS2 or Python. The build (ingest -> translate -> compile -> link) must run on their PC because the translated C is derived from their disc:
- Bundle a portable toolchain: `zig cc` (one binary, clang-based, MIT) or a portable MinGW, plus the Python embeddable package for the translator; or port `bulk2.py`'s hot path to C to drop Python (later).
- A launcher (SDL or Win32, small): pick the ISO, verify the SHA-256, run the pipeline with a progress bar (about 1.5 minutes on the 9800X3D at `-O1`, allow 10-15 on slower machines), then run the game; settings editor, save import/export (`ps2mc.py` -> C), crash report viewer, "verify installation" button that runs G1 against a shipped hash list (the hash list is derived from our rendering of the user's disc, not from game data: fine to ship).
- Requirements statement: Vulkan 1.2 GPU with fp64 for the GPU renderer; the software GS at native resolution as the fallback; CPU with AVX2.
- Licensing text, third-party notices (SDL2 zlib, glslang BSD, Vulkan headers Apache-2.0).

### 3.11 Phase 11: Linux and Steam Deck (optional, 1-2 agent-weeks)

Keep `compat.c`'s POSIX path building (a CI-less "build on WSL" check once a week is enough), then the Deck: RDNA2 fp64 at 1/16 rate makes the triangle Z path the question; an exact integer Z would remove the fp64 requirement for everybody (a GS-wide change, re-verified by G5).

### 3.12 Effort summary and order

| Phase | Content | Agent-days | Depends on |
|---|---|---|---|
| 1 | 4K + 60 fps at full speed, 120 fps, present from VRAM | 5-9 | - |
| 2 | VU1 whole-program compile; VU1 thread | 6-12 | - (2.0 can overlap 1) |
| 3 | In-game settings | 5-9 | 1 (scale path), 3.0 research first |
| 4 | DualSense | 3-6 | 3 (UI), 4.1 can start earlier |
| 5 | Verification kit, hygiene | 1-2 | alongside 1-2 |
| 6 | Readable runtime; symbol map; decompilation workflow + first functions | 6-10 to set up, then weeks | after 1 |
| 7 | Modding: data overlay, texture replacement, hooks API | 20-35 | 6.2/6.3 for the hook points |
| 8 | Effects review at 4K, targeted pass rewrites | 10-20 | 1, PCSX2 comparison |
| 9 | Saving, room coverage, soak | 5-10 spread out | recordings from the user |
| 10 | Launcher and packaging | 10-15 | 3, 5 |
| 11 | Linux/Deck | 5-10 | optional |

The user's batch (1-4) is 19-36 agent-days. Two cheap items should be pulled forward because they de-risk everything after: step 1.0 (references into the repo, half a day) and the save test (half a day, needs a window or a script).

---

## 4. Questions for the user

Each question has options and a recommended default. Answers decide scope before anything is orchestrated.

1. **Order and overlap.** Keep the order A (4K+60) -> B (CPU part 2) -> C (settings) -> D (DualSense) strictly, or allow phase 2's read-only analysis (step 2.0) and the reference freeze (step 1.0) to run while phase 1's long tests run? Default: strict order for code changes, overlap allowed for analysis and references.
2. **VU1 thread.** If whole-program compilation alone brings the EE thread to about 4-5 ms per VBlank (well under 16.7), do you still want the VU1 thread now (headroom for other machines, more complexity), or defer it until a measurement shows a need? Default: do the compile first, re-measure, then decide; you asked for both, so "yes regardless" is also fine.
3. **Playtest (recommendation 1).** When do you want to play: (a) now at native 60 fps, (b) after phase 1 (4K + 60), (c) after phases 1-4? Each playtest with `play.bat` is recorded and becomes a regression input. Default: (b), 20-30 minutes, F3 overlay on, reaching the Castle Suite clock and saving (that also tests saving).
4. **PCSX2 4K comparison (recommendation 3).** Do it, and when? It needs PCSX2's hardware renderer at 4x internal resolution in our own data folder (`D:\ps2-work\tools\pcsx2-data`, never your `Documents\PCSX2`), pictures only. Options: (a) now for the Castle Suite and stage 1, (b) during phase 8 room by room, (c) not at all. Default: (b), plus one early spot check of the Castle Suite after phase 1.
5. **Door specks in `ST_020`.** (a) You walk PCSX2 there and save a state in our instance's slots (about 2 minutes), (b) allow us to copy one of your own PCSX2 states, (c) format a folder card in PCSX2's GUI and we copy our save in, (d) leave it. Default: (a).
6. **Settings UI style.** (i) Extend the game's own Options list natively (depends on how table-driven the menu is), (ii) hybrid: new rows in the game's Options that open host-drawn pages in the game's visual style, (iii) a host overlay on an F-key only. Default: (ii); (i) if the research step finds it clean. May the added pages differ slightly from the original menus (font rendering, layout)? Default: yes, as long as they look like the game.
7. **Settings list and storage.** Confirm the list: resolution factor (native/x2..x6/auto), frame rate (30/60/120), fullscreen/borderless, vsync/present mode, keyboard map, controller map, dead zones, vibration strength, audio latency. Anything else (e.g. a 16:9 mode is a much larger task: see Q15)? Storage: `hg.ini` next to the executable (portable) or `%APPDATA%`? Default: next to the executable, environment variables still override for tests.
8. **Frame-rate option semantics.** 60 and 120 fps add one frame of display delay (about 17 ms) by construction. Show that in the menu text? Default: yes, one line.
9. **"Easily moddable" scope and order.** Which of: (a) data file overlay (`mods/<name>/DATA/...`), (b) texture replacement (high-resolution PNGs by texture id), (c) function overrides by a C API/DLL, (d) Lua scripting, (e) audio sample replacement, (f) a documented symbol map. Default order: f, a, b, c, e; d only if asked.
10. **Readability refactor timing.** The runtime split/reformat (`gs.c`, `hle.c`, settings module) is a pure refactor verified identical. Do it right after phase 1 (before settings build on it), or after the whole batch? Default: after phase 1, before phase 3.
11. **Decompilation priorities.** Confirm the order: frame loop, pad class, options menu, camera, player update, render set-up and effect passes, texture manager, sound wrappers. Anything you care about more (e.g. Hewie AI, stalker AI for mods)? Default: as listed.
12. **Distribution and licence.** GPL-3.0 stays? Public repository eventually (still local until you say)? May the launcher bundle a compiler toolchain (zig/clang, MIT) and an embeddable Python? Mods as DLLs under GPL terms, or a plugin exception? Default: GPL-3.0, bundled toolchain, decide the mod-licence question when 7.3 starts.
13. **Target hardware for "full speed".** Only your 9800X3D + RTX 5090, or a stated minimum (e.g. 4K60 on an RTX 4070 class, native 60 on any Vulkan 1.2 GPU with fp64)? Default: your machine is the gate; a minimum is documented, not gated, until phase 10.
14. **Display rate policy.** 59.94 Hz emulated VBlank against a 60.00 Hz (or 120/144 Hz) display: (a) keep 59.94 exact, one repeated frame about every 17 s on 60 Hz, (b) lock the emulated VBlank to the display's refresh when within 0.5 % (game 0.1 % faster, no judder), (c) VRR when the monitor supports it. Default: (b) with (a) as a setting; (c) later.
15. **Aspect ratio.** The game is 4:3. A 16:9 mode means changing the projection and culling (camera decompilation) and the full-screen effect passes; out of scope for this roadmap? Default: out of scope; 4:3 letterboxed at 4K.
16. **DualSense features.** Rumble from the game's vibration (default yes), light bar (static colour / panic-linked / off; default static, panic-linked later), adaptive triggers (default no), touchpad click as Select (default yes), gyro (no), speaker (no). USB and Bluetooth (both). Several controllers (first to press a button wins)? Steam Input notes in the docs?
17. **Recordings as test data.** `play.bat` records every session (inputs plus a copy of the memory card). May every recording be used as a regression input and kept under `recordings/` (not committed)? Default: yes.
18. **Unattended runs and installs.** May Opus run muted window tests unattended (as before, at night or when you say the PC is free), and install `mingw-w64-ucrt-x86_64-vulkan-validation-layers` for phase 1? Default: yes to both, with the window tests muted and no window while you are using the PC unless you say so.
19. **Saving test.** May a scripted run save at the Castle Suite clock into `memcards/port0` (your current saves are copied first)? Or do you prefer to do the first save yourself during the playtest? Default: scripted, after a backup of the card folder.
20. **Linux/Steam Deck.** In scope at all this year? Default: keep it building, no feature work.

## 5. The user's answers (2026-10-15)

The user answered the questions in section 4 as follows; these decisions bind the work.

- **Q6 Settings UI:** follow the original: the new settings are **extra rows in the game's own Options menu**, in the
  game's own style (option (i), extending the native Options list). Where the menu code makes a row impossible to add
  natively, report back before falling back to anything else.
- **Q8 Frame-rate note:** yes, one line in the menu saying 60/120 fps add about one frame (17 ms) of delay.
- **All other questions: the recommended defaults**, i.e.: strict order for code changes with overlap allowed for
  analysis and references (Q1); VU1 compile first, re-measure, then decide on the thread (Q2); playtest after phase 1
  (Q3); PCSX2 comparison in phase 8 plus one early Castle Suite spot check (Q4); door specks: the user saves a PCSX2
  state (Q5, waits for the user); settings list as proposed, `hg.ini` next to the executable, env vars override (Q7);
  modding order f, a, b, c, e, Lua only if asked (Q9); runtime refactor after phase 1, before phase 3 (Q10);
  decompilation order as listed (Q11); GPL-3.0, bundled toolchain, mod licence later (Q12); the user's machine is the
  speed gate, a minimum documented (Q13); lock the emulated VBlank to the display within 0.5 %, exact 59.94 as a
  setting (Q14); 16:9 out of scope (Q15); DualSense: rumble yes, static light bar (panic-linked later), no adaptive
  triggers, touchpad click = Select, no gyro/speaker, USB and Bluetooth, first controller to press wins (Q16);
  recordings may be regression inputs, not committed (Q17); muted unattended window tests allowed when the PC is free,
  and installing mingw-w64-ucrt-x86_64-vulkan-validation-layers is allowed (Q18); scripted save test after backing up
  memcards/ (Q19); Linux kept building, no feature work (Q20).

### Later decisions (2026-10-15)

- **Order:** finish the current batch first (phase 1 4K + 60 fps, phase 2 VU1, phase 3 in-game settings, phase 4
  DualSense). Texture upscaling (`docs/research-texture-upscaling.md`) and DLSS (`docs/research-dlss.md`) come
  after; their questions are still open.
- **Release:** the user wants a **public 0.1 build soon after the batch**. A release-readiness step (legal shape of
  the distribution, saving verified, known-issues list, README/licence, crash reports, requirements) goes between
  the batch and textures; its questions are open (see the conversation of 2026-10-15).
- **0.1 release decisions (user, 2026-10-15):** distribution as a **ready-made executable** (accessibility first;
  the user accepts that it contains the recompiled game code; game data still never shipped: players supply their
  own disc image); hosting on **GitHub** (the user creates the repository and pushes, or says so explicitly; nothing
  is pushed before that); **Reddit posts** to recruit playtesters (drafted for the user, who posts them); scope =
  the current batch + saving verified + a known-issues list. The repository itself still never contains game-derived
  data (generated C, ISO contents); the executable is a release asset only.
- **Revised (same day): distribution option (a)**: no game code in any download. A simple launcher (ISO pick and
  check, extraction, recompilation, compilation with a bundled compiler and embedded Python, cached build, progress
  display) ships in 0.1; players only supply their own ISO. Replaces the ready-made executable decision above.
