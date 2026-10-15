# Next session: state, findings and the brief (written 2026-10-15)

Read this first, then `docs/handoff.md` (history, rules, paths) and `docs/gpu-renderer.md` (the GPU renderer, 60 fps
and upscaling in detail). The repo is clean at the commit that added this file.

## 1. The goal

A **native PC port of Haunting Ground (SLUS-21075) that runs at 4K and 60 fps and is easy to read and mod**. The
approach so far: a static recompilation (all 11,499 MIPS functions translated to C) with a host runtime, plus a
targeted decompilation of only the parts that are changed (rewrites checked by replaying recorded sessions to
identical frames). A full decompilation was judged a multi-year job; readable C is to grow function by function.

## 2. The user's brief for the next session (verbatim intent)

1. **Fable (the newest Fable model available, `fable` in the Agent tool; this environment lists Fable 5.1) reviews
   everything done so far** (code, docs, git history, this file) and writes a **detailed plan to the end goal**: a
   native PC port at 4K/60 fps, easily readable and moddable.
2. **Fable then asks the user clarifying questions before orchestrating anything.**
3. Fable then **orchestrates**, with **Opus 5.5 doing the work**, in this order:
   - **Recommendation 2: 4K and 60 fps together at full speed** (see section 6, item A);
   - **Recommendation 4: CPU part 2** (VU1 compiled as whole programs with timing worked out at build time; VU1 on
     its own thread) (section 6, item B);
   - both **completed and verified** (rules in section 4) before going on;
   - then **in-game settings**: the game's own settings/options page gains **resolution** (upscaling factor) and
     **frame rate** (30 / 60, later 120), **keyboard mapping** and **PlayStation controller mapping**;
   - finally **native PS5 controller (DualSense) support**.
   (Recommendations 1 and 3, the user's playtest and a PCSX2 4K comparison, are the user's call; ask about them.)

## 3. Where things stand (all committed, local git only)

| Area | State |
|---|---|
| Recompiled game | Boots, menus, movies (emulated IPU), stage 1, Castle Suite, many rooms; LOAD GAME works; saving untested. 20,600-VBlank recorded session replays deterministically. |
| Sound | Game's own IOP modules on an emulated IOP + SPU2; beeps fixed (audio queue cushion). |
| CPU side (Phase 4) | VIF unpack fast path done (-12 % EE thread). **VU1 whole-program compile and VU1 thread: not started.** VU1 is about 2/3 of the EE thread (about 6.4 of 8.4 ms per VBlank in the Castle Suite). |
| Software GS | Exact reference renderer, 4 worker threads, full speed at native resolution. |
| **GPU GS** (`HG_GSVK=1`) | Vulkan compute rasterizer, **bit-exact** with the software GS: 742,414 primitives checked word for word; frames to VB 4000 identical to `hashA` (702/702); the user's whole recorded session at x4 identical (1,005/1,005). Faster than the software GS. |
| **Upscaling** (`HG_GSVK_SCALE=2..8`) | Exact native memory (copy 0) + N x N sub-pixel copies (supersampling), made on the GPU. **x5 (3200x2240, 4K screens) at full speed**, x4 full, x6 78 % (GPU-bound). No window size limit since phase 1 (the SDL path keeps 4096 px). |
| **60 fps** (`HG_GSVK_INTERP=1`) | Frame interpolation on the GPU: each 30 Hz frame replayed with its triangles moved halfway towards the previous frame, shown between the real frames. **Full speed at native resolution**; presents every 16.68 ms, each picture once, in order. Game memory untouched (hashes identical); self-test 1,496/1,496. |
| **4K + 60 fps together** (phase 1, 2026-10-15) | **x5 + 60 fps at full speed in the window** (was 59 %), x4 + 60 full (was 74 %); presents every 16.7 ms, each picture once, in order. x6 + 60: 77 % (GPU-bound, 19.8 ms GPU per VBlank). Details and the table: `docs/gpu-renderer.md`, "Speed, phase 1". |
| 120 fps (`HG_GSVK_INTERP=2`) | **Full speed at native resolution** (presents every 8.4 ms, 0 repeats); with upscaling GPU-bound (x5: 55 %). |
| Window / input | SDL2 window and input on a UI thread; with `HG_GSVK=1` the pictures stay in video memory and the UI thread blits them into a **Vulkan swapchain** (mailbox; `HG_VSYNC=1` FIFO, `HG_PRESENT=sdl` the old SDL texture path). The game thread paces emulated time to 59.94 Hz (locking to the display's refresh, roadmap Q14: not done). Keyboard and SDL GameController mapping is hard-coded in `recomp/host_sdl.c` (see its header comment). F3 speed overlay, F11 fullscreen, Esc quits. |
| Verification kit | `tools/verify`: frozen references (`ref/hashA.md5`, `cs.md5`, `soak.md5`, see `REFERENCES.md`) and the gate scripts `hashcheck.sh`, `gpucheck.sh`, `soak.sh`, `wintest.sh` + `pacing.py`, `picgate.sh` + `piccmp.py`. Picture references for G10 (x5 + 60, native 60) are in `D:\ps2-work\scratch\pic_ref_*` (game pictures: never committed). |
| Settings | Everything is environment variables (`recomp/play.bat` lists them). No in-game options yet. |
| Open user decision | Door specks in `ST_020` need PCSX2 in that room (user saves a state there / allows use of their states / formats a folder memory card). |

## 4. Rules (from the user; they override defaults)

- Commit to **local git only**; no remote, no push until the user says so; don't install gh or touch GitHub.
- Commits: `git -c user.name=N900x4 -c user.email=348903573+N900x4@users.noreply.github.com commit ...`, message ending with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` (or the model that did the work).
- Never commit game-derived data (ISO, CVM, ELF, IRX, generated C in `recomp/gen/`, binaries, decoded audio or
  textures). Licence leaning GPL-3.0; no PCSX2 code copied.
- Don't modify the user's ISO, other folders, system packages; anything needing their decision waits for them.
- **Speed-ups and refactors must be verified identical**: frame hashes and the `HG_VU1CHECK` lockstep check.
- PCSX2 is the **visual** reference only, never for sound. The user's own PCSX2 data
  (`C:\Users\USER\Documents\PCSX2`) and running PCSX2 process are off limits.
- **Do not start the game window for the user unless they ask in that message**; avoid heavy runs while they may be
  using the PC unless they said it's fine. Mute test windows (`HG_AUDIO_MUTE=1`).

## 5. How to work here (tools, checks, gotchas)

- **Build** (MSYS2 UCRT64; the plain Bash tool lacks make/cmp on PATH):
  `MSYSTEM=UCRT64 CHERE_INVOKING=1 C:\msys64\usr\bin\bash.exe -lc "cd /d/ps2-work/HauntingGround-Recomp-Project/recomp && make -j8"`
  (from PowerShell). Shaders compile with `glslangValidator` into `gen/gs_*spv.h` (three builds of
  `vk/gs_sprite.comp`: single, `-DBATCH`, `-DBATCH -DCOPIES`). MSYS2 has no `cmp`/`diff`: the scripts compare with
  `sort A B | uniq -u`.
- **Editing**: heredocs in the Bash tool collapse `\\n`; write Python edit scripts to the scratchpad with the Write
  tool and run them. PowerShell `Set-Content -Encoding utf8` adds a BOM (strip it). Don't run make while
  `hgboot.exe` is running (the link fails silently in filtered output; check the exe timestamp).
- **Gate scripts** (since phase 1, in `tools/verify`, see `tools/verify/REFERENCES.md`): `hashcheck.sh TAG` (G1/G1g,
  `HG_ROUTE=cs` for G2), `gpucheck.sh TAG batch` (G5), `soak.sh` (G3), `wintest.sh TAG env...` then `pacing.py LOG`
  (G7, speed included), `picgate.sh TAG env...` then `piccmp.py REF NEW` (G10). GPU ms per VBlank are logged every
  600 VBlanks; `HG_GSVK_TIMING=1` splits them by phase.
- **Exactness checks** (the older copies in `D:\ps2-work\scratch`, run from the MSYS2 shell):
  - frame hashes: `HG_SCRATCH=/d/ps2-work/scratch sh ../tools/verify/runhash.sh ./hgboot.exe OUT.md5 4000`
    (from `recomp/`), compare with `hashA.md5` (702 frames). Add `HG_GSVK=1` (and `HG_GSVK_INTERP=1`,
    `HG_GSVK_SCALE=N`) to check the GPU paths: copy 0 must stay identical.
  - GPU vs CPU word for word: `vkcheck.sh TAG` (per primitive) and `vkbatch.sh TAG` (batches), four ranges.
  - interpolation replay: `HG_VI_SELFTEST=1` (replay without movement = the real frame).
  - whole recorded session: `soak.sh TAG [env...]` (recording `recordings/20261014-200745`).
  - VU1 lockstep: `HG_VU1CHECK` (see `docs/handoff.md`), required for any VU1 work.
  - A/B timing: `tools/verify/abba.py`.
- **Window tests**: `PL=SECONDS bash /d/ps2-work/scratch/wintest3.sh TAG [env...]` (Castle Suite via LOAD GAME,
  sound muted, F3 overlay, presents logged), then `pacing.py win_TAG.log`; capture with
  `tools/verify/wincap.py "Haunting Ground Recomp" out.png`. `HG_SPEEDLOG=300` prints per-VBlank costs.
- **Debug switches** of the GPU renderer: see the table in `docs/gpu-renderer.md` ("Switches").
- Known hazards: runs with the Vulkan validation layer loaded hang at process exit and stay unkillable in the task
  list (the exe is then locked: rename it to build again); `HG_HOSTPROF=1` + GPU modes hung at boot; glslang's `precise` analysis hangs if a function rewrites
  many fields of the global primitive struct (that is why the copies shader is a separate build without `precise`);
  never read back the primitive/CLUT rings (video memory over PCIe: microseconds per read).

## 6. The next technical steps, as I see them

**A. 4K + 60 fps at full speed (recommendation 2).** Today the GS thread waits for the GPU when it reads back the
in-between picture(s) at each completed frame (`vi_vblank` -> `gsvk_finish` -> `gsvk_compose_fetch`), and every
picture goes GPU -> CPU -> SDL texture -> GPU. Steps: (1) collect the in-between pictures asynchronously like the
real ones (`real_pic` with `gsvk_submit`/`gsvk_wait`, slots 0-2 vs 4-5), one more VBlank of delay; (2) better,
present straight from video memory: a Vulkan swapchain owned by the UI thread (or Vulkan-SDL interop) that blits
the composed picture, no read-back at all; (3) then measure GPU time per frame at x5 with interpolation (two full
renders per 30 Hz frame) and, if needed, cut shader cost (per-pixel struct loads, coarse binning). 120 fps follows
from the same work. Verify: hashes, self-test, `pacing.py` (each picture once, in order, 16.68 ms).

**B. CPU part 2 (recommendation 4).** VU1 microprograms are compiled ahead of time in 64-pair chunks
(`tools/vu1gen.py`, `gen/vu1_aot_*.c`); next: whole-program compilation with static pipeline/stall timing so the
cycle-accurate bookkeeping disappears, then VU1 on its own thread (XGKICK ordering with the GIF/GS ring must stay
exact). Verify with `HG_VU1CHECK` lockstep and frame hashes; time with `abba.py`.

**C. In-game settings.** Find the game's options screen (Haunting Ground has an Options menu: vibration, sound,
screen adjust...). Approach: locate its menu code and data (strings in the ELF, the menu state machine), add entries
by hooking the recompiled functions (`set_hook` in `recomp/overrides.c`) or by rewriting the menu functions as named
C (the targeted decompilation); store settings in a host config file (not the PS2 memory card), applied live where
possible (resolution = `HG_GSVK_SCALE` copies need re-allocation; frame rate = `HG_GSVK_INTERP`). Remapping screens
for keyboard and controller write to the same config. Recorded sessions must still replay identically (settings
are host-side; anything touching game state must be deterministic).

**D. Native DualSense.** SDL2 already supports PS5 controllers via SDL_GameController (hidapi); "native" support
likely means: correct button glyphs/names, rumble mapped from the game's DualShock 2 vibration commands, optionally
light bar, adaptive triggers, touchpad. Check the SDL2 version in MSYS2 (`SDL_HINT_JOYSTICK_HIDAPI_PS5`,
`SDL_GameControllerRumble`, `SDL_GameControllerSetLED`); vibration commands come through the pad emulation in
`recomp/iop_pad_mc.c`.

**Toward the end goal** (for Fable's plan): a launcher and packaging; the decompilation of the frame loop, camera,
input and render setup into named C (roadmap "Decompiling what we change"); texture replacement for mods; effect
passes as native 4K shaders; a mod-loading layer (data files, function overrides); saving from our build verified;
long soak tests; the PCSX2 visual comparisons of later rooms.

## 7. Where things are

- Repo `D:\ps2-work\HauntingGround-Recomp-Project`: runtime in `recomp/` (`gs.c` GS + GPU modes + interpolation,
  `gsvk.c/h` Vulkan, `vk/gs_sprite.comp` shader, `host_sdl.c` window/input/audio, `hle.c` kernel/timing,
  `iop_pad_mc.c` pad/memory card/recording, `dmac.c` DMA/VIF, `vu1*.c` VU1, `overrides.c` function hooks),
  tools in `tools/`, docs in `docs/` (`handoff.md`, `gpu-renderer.md`, `frame-timing.md`, `rooms-checklist.md`, ...).
- Scratch and test artifacts: `D:\ps2-work\scratch` (scripts listed above, reference hashes, pictures).
- Roadmap page (artifact): https://claude.ai/artifact/7uDTrwve81SoLzZG2YFvqQ (its source is in the previous
  session's scratchpad; republishing from a new session needs that URL).
- Disc image `D:\PS2 Games\Haunting Ground (USA)\Haunting Ground (USA).iso`, BIOS `D:\PS2 Games\bios\`.
