# Handoff: where the project stands (2026-10-13)

Read this first when resuming. It gives the state, the rules and the next steps. Details are in the other docs.

## State

The recompiled game boots, plays the logos and movies, and reaches the title, the menu, New Game, the opening movie
and stage 1. Stage 1 has 3D graphics and sound. Rooms beyond the first load and run without crashes (20,000
VBlanks tested). It runs at about 0.5x real time on this VM (2 vCPUs, no GPU).

| Area | Status | Doc |
|---|---|---|
| Boot, pad, memory-card HLE | Done. Saving is untested (no save point reached yet). | `phase1-runtime-notes.md` |
| VU1, VIF1, GIF, software GS | Done; renderer 3.1x faster, hash-identical to the original | `renderer-performance.md` |
| IOP and sound | The game's own IOP modules run on an emulated IOP plus SPU2. SFX, ADX streams and BGM .SQ are verified. | `phase2-audio-notes.md` |
| Movies | The IPU is emulated, and the game's own Sofdec player runs. Movie audio is bit-exact. | `movies-ipu.md` |
| Exploration | `ST_02A` → `ST_013` → `ST_00D` reached; explorer tools | `exploration-notes.md` |
| Reverb | Unverified: the game sets the depth to 0 at stage init, and no reached room changes it | `exploration-notes.md` |

Last commit before this doc: `bf60152`.

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

- Repo: `~/ps2-work/HauntingGround-Recomp-Project`. Game files: `HG_GAME_DIR="~/ps2-work/Haunting Ground/iso"`.
  Full disc image: `~/ps2-work/Haunting Ground (USA).iso`.
- PCSX2 2.8.2: extracted AppImage at `~/ps2-work/tools/pcsx2/` (`squashfs-root/`, data dir `data/`).
- BIOS: `~/.config/PCSX2/bios/ps2-0220a-20060905.bin`, a US v2.20 BIOS, plus its `.mec` and `.nvm`. Copied
  2026-10-13 from the user's shared folder `/mnt/hgfs/PS2 Games/bios/`; md5 `40c11c063b3b9409aa5e4058e984e30c`.
- Verification scripts: `tools/verify/` (see below).

## Waiting on the user

- **3D acceleration** in the VM (VMware Display → "Accelerate 3D graphics"). Without it PCSX2 has only llvmpipe.
- Suggested: 4 or more vCPUs (now 2). RAM is 8 GB; 12 GB is optional (the VM swaps heavily).
- After that, the user expects me to self-test against PCSX2.

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
cd recomp && make -j2          # build: see the README for the ingest/generate steps
export HG_GAME_DIR="$HOME/ps2-work/Haunting Ground/iso"
# with movies: logos → LOOP_DEMO skipped → title → New Game → OPENING (210 s) → stage at about VB 14400
HG_PAD=1500:start,1800:start,2100:cross ./hgboot
# movies skipped (old timeline, stage at about VB 2200)
HG_MOVIES=skip HG_PAD=600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross ./hgboot
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
