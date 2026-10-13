# Handoff: where the project stands (2026-10-13)

Read this first when resuming. It gives the state, the rules and the next steps. Details are in the other docs.

## State

The recompiled game boots, plays the logos and movies, and reaches the title, the menu, New Game, the opening movie
and stage 1. Stage 1 has 3D graphics and sound. Rooms beyond the first load and run without crashes (20,000
VBlanks tested). It now runs on the user's Windows PC (MSYS2 build), at 0.51x real time in stage 1 and 0.68x in the
Castle Suite. The GS thread is the limit.

| Area | Status | Doc |
|---|---|---|
| Boot, pad, memory-card HLE | Done. LOAD GAME works: a save made on PCSX2 (extracted with `tools/verify/ps2mc.py`) loads into the Castle Suite (`ST_021`) at the saved position. Saving from our build is untested. | `phase1-runtime-notes.md` |
| VU1, VIF1, GIF, software GS | Done; checked against PCSX2: stage 1 has 99.7% of pixels within 8 levels, the Castle Suite matches | `renderer-performance.md`, below |
| IOP and sound | The game's own IOP modules run on an emulated IOP plus SPU2. SFX, ADX streams and BGM .SQ are verified. | `phase2-audio-notes.md` |
| Movies | The IPU is emulated, and the game's own Sofdec player runs. Movie audio is bit-exact. | `movies-ipu.md` |
| Exploration | `ST_02A` → `ST_013` → `ST_00D` reached; explorer tools | `exploration-notes.md` |
| Reverb | Unverified: the game sets the depth to 0 at stage init, and no reached room changes it | `exploration-notes.md` |

Last commit before this doc: `bf60152`.

### Session 2026-10-13, later (move to Windows)

- **Windows port** (commit `da1c36f`): MSYS2 UCRT64 build, `recomp/compat.c` replaces the Linux-only calls (see the
  README). Checked: two `runhash.sh` runs to VB 4000 give identical hashes (702 frames), and `HG_VU1CHECK` 2,000,000
  runs with 0 mismatches. These Windows hashes are in `D:\ps2-work\scratch\hashA.md5`. They are the reference for
  later speed-ups. They have not been compared with Linux output, because the VM's hashes were lost.
- **Speed, measured** (headless, `HG_SPEEDLOG=300`, steady state, 9800X3D):

  | Scene | Speed | ms per VBlank | Main thread | GS thread |
  |---|---|---|---|---|
  | Stage 1 (New Game, VB 4200–6000) | 0.51x real time | 33.0 | VU1 16.1 ms; waits for the GS about half the time | 94% busy |
  | Castle Suite (LOAD GAME, VB 4200–6000) | 0.68x real time | 24.6 | VU1 12.0 ms; waits for the GS about half the time | 93% busy |

  Before the getenv fix below, the speeds were 0.34x and 0.33x. The UCRT's `getenv` takes a lock and scans the whole
  environment. Uncached `getenv()` calls on hot paths (VU1 CLIP, every triangle, and the SIF/RPC logs) cost 25–35% of
  the GS and VU1 time. They are now cached per call site (`GETENV_ONCE` in `hle.h`). Frames and VU1 lockstep were
  unchanged.
- **Profiler**: `HG_HOSTPROF=1` (Windows) samples the main and GS thread PCs every 1 ms into `hostprof.txt`;
  `python tools/verify/hostprof.py hostprof.txt hgboot.exe [top] [--lines N]` names them.
- **Where the time goes**: the GS thread is the limit in both scenes. About half of it is sprites, mostly the
  full-screen effect passes (`sprite_row_avx2`, `draw_sprite`, `sprite_spans`). The other half is triangles
  (`v_texel`, `draw_triangle_avx2`, `v_pixel`, `v_sample`). The main thread waits for it (`prod_wait`) more than half
  the time. The 9800X3D has 16 hardware threads and two are in use, so the obvious next step is to split GS
  rasterizing across threads (for example by screen bands, each band in primitive order, so the output stays
  bit-identical).

### Session 2026-10-13 (PCSX2 reference comparison)

- **PCSX2 reference is set up**: data dir `~/ps2-work/tools/pcsx2/data` (BIOS path set, PINE on, software renderer,
  raw screenshots). Save states: slot 1 = main menu, 2 = opening movie start, 4 = stage 1 start. The user's memory
  card `Mcd001.ps2` holds a save at the first clock ("Castle Suite", 9:25).
- **Fixed, found by comparing against PCSX2 GS dumps** (`tools/verify/gsdump.py`):
  - VU1: WAITQ/WAITP stall the whole instruction pair, so MULq next to WAITQ sees the new Q (the brown beam in stage 1
    was the last vertex of every batch projected with the previous vertex's 1/w).
  - VU1: pipeline stalls are modelled (4-cycle VF latency, FDIV busy; Q/P count cycles). Fixes the bright streak by
    Fiona. `HG_VUSTALL=0` turns it off. Both interpreters, lockstep clean.
  - GS: Z32/Z24/Z16/Z16S textures use the depth layouts (blocky fog behind the stage 1 gate).
  - MCSERV GetDir: maxent -1 means no limit; without this LOAD GAME saw no save.
  - VU0 macro mode sets MAC/status flags; `HG_FPRZ=1` (opt-in) rounds toward zero like the EE FPU.
- **The user says PCSX2's sound is not accurate**: do not use it as the audio reference.
- **Stage number**: player object `*(0x44f818)` + 0x30 (0x2A = `ST_02A`, 0x21 = `ST_021`). The whole outside area
  walked so far is `ST_02A`.
- Tools: `tools/verify/pine.py` (PCSX2 memory over PINE, RAM dump, save/load state), `gsdump.py`, `vudis.py`,
  `framecmp.py`, `ps2mc.py`; `HG_VTXLOG`, `HG_VU1DUMP`, Z buffer in `HG_PIXWATCH`.
- Method that worked: same frame in both (static camera), PCSX2 GS dump vs our `HG_VTXLOG`/`HG_PIXWATCH`; the
  first vertex that differs points at the bug.
- **Outside area**: two explorer runs of 30,000 VBlanks each (seeds 1 and 2) covered `ST_02A` (gate, garden paths,
  fountain, terraces) and `ST_013` (upper terrace with the two lamps and the stairs to the castle door), with no crash or
  hang. Frames look clean; the `ST_013` wedge holes are gone. `tools/verify/pcsx2_teleport_cmp.py` teleports Fiona in
  PCSX2 (PINE) to each explorer position and compares windows: where the cameras agree, the images agree. Limits:
  cameras have history (hysteresis), so a teleport often gets a different camera; a monologue text box pauses PCSX2
  (the tool taps Cross first).
- **Next**: compare the room interiors after `ST_013` (outfit change, `ST_021`) from the loaded save; sound against a
  reference the user trusts (not PCSX2); saving from our build; the faint haze offset over the far wall in stage 1.

## Rules (from the user)

- Commit to local git only; no remote and no push until the user says so. Don't install gh or touch the user's
  GitHub account.
- Never commit game-derived data: ISO, CVM, ELF, IRX, generated C, binaries, decoded audio or textures.
- Don't modify the user's ISO, other folders, or system packages. Anything that needs sudo or a decision from the
  user waits for them.
- Licence leaning GPL-3.0; all code is written from scratch (no PCSX2 code copied).
- Speed-ups must be verified identical: frame hashes and the `HG_VU1CHECK` lockstep check.
- The user's goal is the first room with sound and effects accurate to the developers' vision, **before** they test
  controls.

## Paths

Since 2026-10-13 the project lives on the user's Windows 11 PC (Ryzen 7 9800X3D, RTX 5090); the Linux VM is shut down.

- Repo: `D:\ps2-work\HauntingGround-Recomp-Project`. Game files: ingested into `game/` (the default; no
  `HG_GAME_DIR` needed). Disc image: `D:\PS2 Games\Haunting Ground (USA)\Haunting Ground (USA).iso`.
- Build: MSYS2 UCRT64 at `C:\msys64` (gcc 16, make, SDL2, pkgconf). Run make and `hgboot.exe` from the UCRT64 shell,
  or from another shell with `MSYSTEM=UCRT64 /c/msys64/usr/bin/bash.exe -lc '...'`.
- BIOS: `D:\PS2 Games\bios\ps2-0220a-20060905.bin` (US v2.20, plus `.mec` and `.nvm`).
- PCSX2: the user's Windows install, data in `C:\Users\USER\Documents\PCSX2`. The VM's save states, memory card and
  settings are in `D:\ps2-work\HauntingGround-Recomp-transfer\pcsx2\`; not yet copied into the Windows install.
- Verification scripts: `tools/verify/` (see below). Paths are repo-relative now; `pine.py` (Unix socket) and
  `pcsx2_teleport_cmp.py` (X11 `import`) are still Linux-only.

## Waiting on the user

- A decision on whether to parallelize the GS rasterizer next (see the Windows session above) or to do something else
  first.
- PCSX2 on Windows is not set up for the comparison tools yet. It needs:
  - the VM's save states copied into `Documents\PCSX2\sstates`;
  - PINE turned on (it uses TCP on Windows);
  - Windows ports of `pine.py` and of the window capture in `pcsx2_teleport_cmp.py`.

  The user still wants me to self-test against PCSX2, for graphics only.

## Next steps, in order

1. **PCSX2 reference comparison.** Boot the same disc in PCSX2 with the BIOS, then capture frames and audio at the
   same points (title, first room, `ST_013`). Questions to answer:
   - Are the dark wedge holes near the camera in `ST_013` real? (VU1 near-plane ADC culling)
   - What does the first room look like: the brown diagonal beam, effect passes?
   - Mix levels and reverb.
   - VU flag latency (`HG_VUFLAGDELAY`).
2. **Play further.** Route to the first save point: **the clock in the room after Fiona changes from the towel into
   her normal outfit**. Fiona's position is at `*(0x44f818)+0x10`. Area loads show up in the log as CVM offsets
   (`tools/verify/cdfiles.py`).
3. **Memory card.** Save at the clock, check the files under `HG_MEMCARD_DIR`, then reload with LOAD GAME.
4. **Reverb.** Watch for a depth change in later rooms (`HG_TRACE=20ef50`, `HG_SPUREGLOG=78c-78e`).
5. **Speed**, after the accuracy work: VU1 ahead-of-time compilation, texture fetch.
6. **Soak testing**, in parallel.

## Common runs

```sh
cd recomp && make -j16         # in the MSYS2 UCRT64 shell; see the README for the ingest/generate steps
# The memory card in ../memcards/port0 holds the user's saves, so the main menu opens on LOAD GAME.
# New Game timelines need an empty card: HG_MEMCARD_DIR=<empty dir>.
# with movies: logos → LOOP_DEMO skipped → title → New Game → OPENING (210 s) → stage at about VB 14400
HG_MEMCARD_DIR=/d/ps2-work/scratch/mc_empty HG_PAD=1500:start,1800:start,2100:cross ./hgboot.exe
# movies skipped, New Game → stage 1 (3D from about VB 2400)
HG_MEMCARD_DIR=/d/ps2-work/scratch/mc_empty HG_MOVIES=skip HG_PAD=600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross ./hgboot.exe
# movies skipped, LOAD GAME → Castle Suite (ST_021) from the user's save (3D from about VB 3000)
HG_MOVIES=skip HG_PAD=600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross,2400:cross,2600:cross,2800:cross ./hgboot.exe
```

Useful switches:

| Variable | Use |
|---|---|
| `HG_MAXVB` | Stop after this many VBlanks. |
| `HG_WINDOW=0` | Headless run. |
| `HG_FRAMES=dir HG_FRAME_EVERY=n` | Dump frames. |
| `HG_GSDRAW=0/N` | No drawing, or drawing from VBlank N. |
| `HG_MTGS` | Run the GS on its own thread. |
| `HG_EE_MIPS` | EE speed; the default is 300. |
| `HG_EXPLORE=vb`, `HG_EXPLORE_SEED` | Automatic wall-following explorer. |
| `HG_RAMDUMP=dir:vb,...` | Dump RAM at the given VBlanks. |
| `HG_PIXWATCH=x,y,vb` | Watch one pixel. |
| `HG_BIGTRI`, `HG_CLIPLOG` | Diagnostics for large triangles and clipping. |
| `HG_IPULOG`, `HG_IPUDUMP` | IPU log and picture dumps. |
| `HG_ADMAWAV` | Movie and stream audio to a WAV file. |
| `HG_VU1CHECK=1` | VU1 lockstep check. |
| `HG_SPEEDLOG=n` | Every n VBlanks: emulated speed against real time, host ms per VBlank by subsystem. Headless runs are unthrottled; with a window the game is paced to real time. |

## Verification scripts (`tools/verify/`)

These are one-off checks from the sessions, kept so they survive a reset. Paths point at this machine's layout.

| Script | What it does |
|---|---|
| `runhash.sh BIN OUT.md5 MAXVB [GSDRAW]` | Hashes a frame every 5 VBlanks. The game is run with `HG_MOVIES=skip`. Scratch files go in `$HG_SCRATCH` or `/tmp`. |
| `gscmp.sh BIN OUTDIR` | Renders VBlanks 2380–2520 and compares them with `$HG_SCRATCH/ref1`. |
| `cvm.py` | DATA.CVM directory reader. |
| `cdfiles.py` | Maps the CVM offsets in a log to file names, which shows which areas loaded. |
| `m2parse.py`, `m2var.py`, `mbtest.py` | Offline MPEG-2 slice parser that uses `ipu.c`'s VLC tables. |
| `avref.py` | Compares `HG_IPUDUMP` pictures with libavcodec, called through ctypes. |
| `sfdaudio_check.py`, `adxcmp.py`, `bankcmp*.py` | ADX and sound-bank output compared with independent decodes. |
| `sqparse.py` | BGM .SQ sequence parser (the real one is `tools/formats/sq.py`). |
| `irxdis.py`, `irximp.py` | Disassemble an IRX module and list its imports. |

The reference frame hashes were made at the old 150 MIPS default and lived in the session's tmpfs scratch, so they
are gone after a reboot. To check a speed-up, regenerate the references first with the binary from before the
change.
