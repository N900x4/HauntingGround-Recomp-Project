# Handover: the night of 2026-10-25 (playtests 6, 6b, 7) and the release plan

**Read this first**, then `docs/next-session.md` (rules in its section 4, paths in section 7) and `docs/handoff.md`
(history). The user's auto-memory (`hg-*` files) holds the same decisions in short form.

## Progress (night of 2026-10-26, unattended; newest first)

- **Version is 0.1** (the user, 2026-10-26: "Its 0.1 not 0.2"; 0.1 was never published, so this is the first public
  release, now with Linux and the HD textures). `docs/release/github-release-0.1.md` is the one release page (the 0.2
  draft merged into it and removed); README, known-issues, the Reddit drafts and both mkrelease defaults say 0.1. The
  user wants the launch prepared soon: **`docs/release/launch-checklist.md`** is the plan. Linux archive rebuilt as
  `scratch/release-linux/HauntingGround-Recomp-0.1-linux-x64.tar.gz` (1,633.7 MB, sha256
  e50f87096e927847046009d8e63ddde4536b1c53f2935b54b887f560a5592c91, from 94391f3; the 0.2-labelled one deleted).

- **Linux release built and verified** (`scratch/release-linux/HauntingGround-Recomp-0.2-linux-x64.tar.gz`, 1,633.7
  MB, sha256 c55a5354...9b5c, built from 36d1e27 with the 0.2 README; rebuild it from the latest commit with `tools/release/mkrelease_linux.py
  --gcc D:/ps2-work/tools-ext/bootlin-dl/x86-64--glibc--stable-2024.05-1 --gcc-licenses .../bootlin-dl/licenses
  --textures D:/ps2-work/texwork/release_textures`). Unpacked into an empty folder: first start with the user's ISO in
  64 s (copy 1 s, translate 17 s, compile 45 s), ISO untouched, the built game identical to hashA (software and GPU),
  the launcher writes hg.ini, records, finds ../textures by itself; kdialog progress checked in gamescope. Compiler:
  **Bootlin gcc 13.3** (zig's clang took 4-8 min per compiled VU1 program: dropped); `recomp/glibc23.c` keeps the game at
  glibc 2.34. Archive is .tar.gz (the toolchain has symlinks).
- **Windows bundle not built**: the Windows drive (`/dev/nvme0n1p3`) is not mounted and mounting needs the user's
  password (a udisks attempt at 01:20 was cancelled; a polkit dialog may have flashed). `mkrelease.py` (now with
  `--textures`) is Windows-only (MSYS2 bash/pacman). Options for the user: boot Windows ~15 min and run it there, or
  mount the drive read-only (`udisksctl mount -b /dev/nvme0n1p3 -o ro`) so a session can port the staging to Linux and
  test under Wine.
- README, known-issues and `docs/release/github-release-0.2.md` written for 0.2 (version number to be confirmed).

- **All gates passed in IOP sync mode 2 and the work is merged into `main`** (fast-forward; the WIP branch is gone):
  determinism + `HG_VU1CHECK` 0 mismatches; G1/G2 all renderers identical (hashA, cs re-frozen); G5 0 differing
  batches; G6 0 of 1,737; G3 soak unchanged; saving identical; audiocheck PASS (now in mode 2); options test two runs
  identical (with the new HD row); rumble; G10 24/24 against the existing sets (kept); G7 in headless gamescope at
  120 Hz: 99.97 % held 2 refreshes, speed 1.00.
- **BC7 pack done**: `D:\ps2-work\texwork\release_textures` (1,389 `.dds` + 5 `.png`, 1.9 GB, about 1.5 GB zipped);
  in game 46-58 dB against the PNGs, video memory 602 -> 152 MB, decode waits 947 -> 5 ms. Encoder and checker built
  in `D:\ps2-work\tools-ext` (bc7enc_rdo + ISPC 1.31.0, bc7check), zig 0.16.0 there too (sha256 checked).
- **HD for players**: `../textures` by default, `hd = on|off`, the Options row (picture checked).
- **Linux build works**: zig-built game identical frames (G1), glibc <= 2.29; `HauntingGround.sh` and `setup.py`
  Linux path written but **not yet run end to end** (next: a Linux mkrelease, then a fresh-folder first start with
  the user's ISO in batch mode, then the Windows bundle).

## 0. Where the code is

- (Superseded: see Progress above.) Branch **`wip-2026-10-25-night`** (local git only, never pushed) held tonight's fixes in one commit on top of
  `main` (`73f668d`). They are **built and tested on recordings, but the regression gates have NOT been run** and
  the references are NOT re-frozen. `main` is unchanged. Work on the branch; merge it into `main` only after
  section 3, step 1 passes (rewrite the commit message then if useful).
- The binary `recomp/hgboot` in the working tree is built from that branch.
- The user is a layman about the internals but decides everything user-facing. No heavy runs (builds, replays, gates)
  while the user is on the PC; they say when they sleep or go to work. Never push, upload or publish without
  their explicit go-ahead.

## 1. What tonight changed (all in the WIP commit)

| Problem (user's words) | Cause | Fix | Where | Checked |
|---|---|---|---|---|
| Playtest 6: sound cuts off, audio late, hiccups | HD texture ids recomputed texel by texel (`read_px` through the swizzle) at every re-upload, twice: the GS thread's CPU time doubled (+2 ms/VBlank), the game ran at 81-86 % | A quick 64-bit hash of the image's memory blocks (in block order, only the bits the format uses) memoised against the slow id (`hd_blocks`, `hd_memo`) | `gs.c` (HD helpers near the top) | `HG_HDTEX_CHECK=1` computes both: 0 mismatches over 108,763 ids; 99.4 % from the hash; replay 78 s -> 64 s (HD off 58 s) |
| (same) | A draw needing an HD image still being decoded **waited** (28 waits, 0.6 s): stalls, then a sound backlog | With the window: draw the original until it is decoded (`hdtex_get` returns -1, asked again). Headless: still waits (gates deterministic). `HG_HDTEX_WAIT=1/0` forces | `hdtex.c/h`, `gs.c gsvk_hd` | playtest 7: 0 waits, 32k draws of the original while decoding |
| Audio stayed ~250 ms late after a stall | The ring only drains through ±0.6 % resampling (40 s per 250 ms) | Backlog shedding: >80 ms over target for 0.5 s -> new sound left out until within 20 ms (one skip) | `host_sdl.c host_audio_push` | log line `the backlog left out` |
| Playtest 6b: **cutscenes out of sync** | **IOP sync mode 1 ran the IOP 1.7 % fast**: it took IOP time = EE cycles / 8, but the EE's emulated time counts 300 M/s (`HG_EE_MIPS`), not 294.9. 814.2 samples per VBlank instead of 800.8; ring pinned at +0.6 %, chunks dropped, movie sound drifting 12 ms/s | **Mode 2** (default): IOP cycles = EE cycles × (IOP_CLOCK/59.94) / `ee_vb_period` (128-bit), inverse rounded up. Recordings: `iopsync 2`; `iopsync 1` still replays mode 1 | `iop_kernel.c` (`ee_iop`, `ee_to_iop`, `iop_to_ee`) | 799.8 samples/VBlank (boot accounts for the rest); Debilitas movie in sync (±7-17 ms); **user confirmed in playtest 7: "cutscene lag has been solved"** |
| Playtest 6b: stutter when first meeting Debilitas | (a) a draw's Z pages were marked GPU-*written* whenever Z test was on, even with **ZMSK** (read-only). The Z buffer (0x3380, Z24, 512 x 448) covers pages 504-505 where the game keeps two CLUTs (0x3f04, 0x3f3c): every CLUT load read the page back and waited for all GPU work (17,343 read-backs per session) | ZMSK: Z pages into the *read* set | `gs.c gsvk_take` | read-backs 17,343 -> 0 for the whole session; **needs G1/G5/G10** |
| (same) | (b) the 60 fps in-between replays: the GS thread waits for the GPU, the EE thread waits 12-15 ms/VBlank on a full ring (headless: 1.84x real time without interpolation, 0.89x with) | Adaptive interpolation rule **"slow"**: the game waits > `HG_VI_SLOW` ms (3) per VBlank on the GS thread (checked every 10 VBlanks) -> in-between pictures off 2 s; doubling to 20 s if it recurs within 10 s; ignored while `HG_FF` fast-forwards | `gs.c vi_adapt_tick` | Debilitas scene and the chase: 0 late pictures, 0 underruns in replays |
| Playtest 7: slowdown while running from Debilitas (33-50 % speed for seconds) | KWin presented only every 3rd refresh (25 ms, 40 Hz) for a few seconds; the game follows the display's ticks, so it slowed down with it. **Not reproducible in gamescope** (the replay of that moment is clean) | Six presents in a row late -> the display lock is dropped at once (game paced by the host clock; the usual 1 s check locks again) | `host_sdl.c` (after the `ivl` update) | **not yet seen live** (needs the user's playtest) |

Debug tools added (keep them; documented in code comments):

- `HG_FF=N`: a replay with the window runs unpaced and silent until VBlank N, then real time (the user's "save
  state" request: recordings + fast-forward replace save states; a real save state was judged too big: EE threads are
  fibers on host stacks, GPU state in Vulkan).
- `HG_IOPSYNC_AT=N:M`: IOP sync mode M from the game's VBlank N (replay a mode-1 recording exactly to a cutscene,
  then mode 2's clock). `HG_IOPSYNC=0/1/2` overrides.
- `HG_HOSTPROF_FROM=N`: the guest-function profile from VBlank N (top 25 now).
- `HG_SPEEDLOG`: now also "waiting for the GS thread N ms per VB" (the producer's ring waits) and a DMA/VIF column
  (that column subtracts GS-thread time, so it goes negative with MTGS: ignore it).
- `HG_DLLOG=N`: CLUT read-backs from VBlank N with the draw that last wrote the page (fbp, Z buffer, scissor).
- `HG_HDTEX_CHECK=1`, `HG_HDTEX_WAIT`, `HG_VI_SLOW` (above).

Recordings (in `recordings/`, the user's; not in git):

| Recording | Mode | What |
|---|---|---|
| `20261025-204742` | iopsync 1 | playtest 6 (4 min) |
| `20261025-210234` | iopsync 1 | playtest 6b: Debilitas' first scene, the stutter at VB ~18,650-19,100, movie `DE000E.SFD` from VB 19,125. Replay: `HG_FF=18600 HG_IOPSYNC_AT=18600:2` |
| `20261025-221224` | iopsync 2 | playtest 7 (8 min): chase start VB ~24,100; the KWin 40 Hz stretch ~VB 24,700-25,100 (clean in replay) |

How the iteration was done (reuse it): `gamescope --backend headless -W 3840 -H 2160 -r 120 -- env HG_INI=off
HG_GSVK=1 HG_GSVK_SCALE=4 HG_GSVK_INTERP=1 HG_HDTEX=/mnt/NewVolume/ps2-work/texwork/hd HG_AUDIO_MUTE=1 HG_PADREC=
HG_PADPLAY=../recordings/REC HG_FF=N HG_MAXVB=M HG_OVERLAY=1 ./hgboot -q`, then grep the log for `^smooth:`,
`^av: ` (movie A/V), `in-between pictures`, `underrun`. Headless without the window (`HG_WINDOW=0`) for
`HG_SPEEDLOG`/`HG_HOSTPROF` (unpaced).

## 2. Playtest results (user's words)

- 6 (HD on, before the fixes): "the sound issue has returned with it cutting off and the audio delay remains,
  smoothness is getting closer but there are still hiccups, textures do look like an improvement".
- 6b: "smoothness is 90% ther, good job!, cutscenes are still out of sync and there are some stutters when i first
  meet debilitas".
- 7: "cutscene lag has been solved!, now there is only a couple of slowdowns after the debilitas introduction,
  initially when i started running away from him" (both causes fixed above; the second only mitigated). Asked about
  desktop activity at that moment: "nothing i noticed".
- Playtest 7 numbers: 99.63 % of pictures on time (6: 93.3 %), 10 not ready (755), 17 underruns (117), A/V -11 ms
  average (-149). F9 marks not yet examined: playtest 7 at 279.6 s, 282.0 s (stage 21 movie) and 404.9 s; 6b at
  127.6 s and 314.8 s; 6 at 40-42 s (opening movie). Ask the user what they marked (pop-in is still open).

## 3. Next steps, in order (the user approved this plan; they sleep/work while it runs)

1. **Gates, then merge** (`tools/verify/REFERENCES.md`). Mode 2 changes emulation timing, so:
   - determinism: two runs of the New Game route identical; `HG_VU1CHECK=1` 0 mismatches, 0 AOT handovers;
   - G1 (`hashcheck.sh`, all four renderers identical to each other) and G2 (`HG_ROUTE=cs`); G5 (`gpucheck.sh`) and
     G6 (self-test) for the ZMSK change; G3 `soak.sh` (its recording has no `iopsync` line: old timing, must still
     match `ref/soak.md5`); `savetest.sh`; `audiocheck.sh` (it forces `HG_IOPSYNC=${HG_IOPSYNC:-1}`, i.e. mode 1
     now: decide whether it should test mode 2, and say so in REFERENCES.md); `optionstest.sh`, `rumbletest.sh`;
     G10 `picgate.sh` (references need re-making for mode 2); G7 `wintest.sh` in headless gamescope (`PL=58`).
   - Re-freeze `ref/hashA.md5`, `ref/cs.md5`, `ref/save.md5` and the G10 sets in mode 2 (keep the mode-1 sets as
     `*_20261025_iopsync1`), update REFERENCES.md, `docs/frame-timing.md` (mode 2), `docs/textures.md` §8 (quick
     hash, no waits with the window), `docs/gpu-renderer.md` (ZMSK pages, the "slow" rule, the display-lock drop),
     `docs/handoff.md` (an entry for this night). Then merge into `main` and commit (author and trailers per
     next-session.md section 4).
2. **Texture pack size**: `texwork/hd` is 2.4 GB (1,394 PNGs); GitHub allows 2 GB per release file and the user
   wants **one bundle** per platform. First lossless: re-compress the PNGs, store fully opaque ones as RGB. If the
   bundle is still over 2 GB: **BC7** (the user's choice; then `hdtex.c` loads a block-compressed format and the
   GPU samples BC7 images; check the device supports it; mipmaps per level).
3. **HD for laymen**: a `textures/` folder next to the game (release layout: next to `bin/`) loads automatically when
   `hd_textures` is unset; an "HD textures: on/off" row in the Options menu (`docs/options-menu.md`); a missing
   folder = original textures. Gates with HD off must stay identical.
4. **Linux release** (new): the same model as Windows (the game is built on the player's PC from their disc
   image) with a **bundled portable compiler: zig cc** (the user approved downloading it once; put it in its own
   empty directory per the download rules), SDL2 shared library bundled, a Linux launcher (shell or C). Test a fresh
   first start from an empty folder with the user's ISO (`/mnt/NewVolume/PS2 Games/Haunting Ground (USA)/Haunting Ground (USA).iso`;
   on Windows `D:\PS2 Games\...`).
5. **Windows release**: `tools/release/mkrelease.py` was written for Windows (MSYS2 UCRT64, python.org Python). The
   PC now runs Linux; the MSYS2 tree is on the mounted Windows drive (`/run/media/<user>/<windows drive>/msys64`).
   Stage from there, test the launcher and the game under Wine; a final check on real Windows needs the user (about
   10 minutes in Windows).
6. **Both bundles include the HD textures** (the user's decision 2026-10-25: "keep it in a bundle"; a Nexus upload
   of the pack is planned for 1.0). Rewrite `docs/release/github-release-0.1.md` and the README: they promise "no
   game code or data in this download", which is no longer true for the textures. The DMCA risk was explained to
   the user once; do not re-argue it. Version: the next release (0.2 or 0.1 re-cut: ask the user).
7. Known-issues list for the release: brief 30 fps moments in heavy scenes (the "slow" rule), the compositor
   stutter (mitigated), text at native resolution (fonts), pop-in (open).

## 4. Waiting for the user

- A short playtest of the final bundle (the display-lock drop live; F9 marks explained).
- Hand checklists: Options menu (12 points, `docs/options-menu.md`), DualSense (13 points, `docs/controller.md`,
  never tried on Linux).
- Release paperwork: GPL-3.0 confirmation, compiler source archives (permission to download), the ISO's redump
  check, creating the GitHub repo and pushing. 120 Hz / x5, the `strtol` decision (memory `hg-strtol-decision`),
  the font for HD text, a long mode-2 recording for a new soak reference.

## 5. Roadmap after the release

Async interpolation replays (the real fix for heavy scenes; the "slow" rule is a stop-gap), pop-in, fonts (a vector
font redrawn into the sheets; the user picks the font), texture-pack polish (VRAM freeing, per-room preload),
readable code (split `gs.c`/`hle.c`, symbol map, decompile the frame loop), the modding layer (`mods/` overlay, C
API), effects as native shaders, Steam Deck. The roadmap artifact (https://claude.ai/artifact/7uDTrwve81SoLzZG2YFvqQ)
is dated 15 Oct; republish it with today's state if the user asks.
