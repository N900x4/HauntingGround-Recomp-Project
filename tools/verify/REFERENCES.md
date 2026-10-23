# Frozen references and the gate scripts

The reference hashes were kept in `D:\ps2-work\scratch` until 2026-10-15; they are now here so that a reset cannot lose
the oracle. They are md5 sums of our own frame dumps (PPM files written by `HG_FRAMES`), not game data.

| File | What | Made with | Check |
|---|---|---|---|
| `ref/hashA.md5` | New Game on an empty card to VB 4000, a frame every 5 VBlanks (702 frames), software GS | `da1c36f` (Windows port, 2026-10-13), confirmed by every later speed-up | `runhash.sh ./hgboot.exe OUT.md5 4000` |
| `ref/cs.md5` | LOAD GAME from `../memcards` into the Castle Suite to VB 4200 (742 frames), software GS (`cs0.md5` in scratch; `cs4.md5`, the GS workers, identical) | `0d641d9` (GS workers, 2026-10-13) | `HG_ROUTE=cs runhash.sh ./hgboot.exe OUT.md5 4200` |
| `ref/save.md5` | The card files `savetest.sh` writes (save into a new slot, onto an emptied card, overwrite), with the game's clock fixed (`HG_CLOCK`) | 2026-10-15 (release 0.1) | `savetest.sh TAG` |
| `ref/soak.md5` | The user's session `recordings/20261014-200745` replayed to VB 20600, a frame every 20 VBlanks (1,005 frames), software GS; GPU x4 identical (`14bd3e3`). **Re-frozen 2026-10-15 with the SPU2 fix** (reverb end address, envelope rates): identical to the old one up to VB 11920, then Fiona's pose differs by a few pixels (IOP timing follows the sound state; see `docs/handoff.md`); two runs identical, one with `HG_VU1CHECK=1` | SPU2 fix (2026-10-15) | `soak.sh TAG [env]` |

The New Game route uses `HG_MOVIES=skip` and a fixed `HG_PAD` timeline; LOAD GAME needs the user's card in
`../memcards/port0` (save 01) and the recording needs `../recordings/20261014-200745` (neither is in the repository).

## Gate scripts (roadmap section 3.0)

All take outputs into `$HG_SCRATCH` (default `../scratch` next to the repository: `D:/ps2-work/scratch`) and run
from bash: the MSYS2 UCRT64 shell on Windows, any shell on Linux (they then run `hgboot`; the hashes are written
with `md5sum -b`, as the references are, on both). Every
script sets `HG_INI=off` unless `HG_INI` is already set, so the user's `recomp/hg.ini` (the player's resolution,
frame rate, renderer) never changes a gate's result (on 2026-10-16 it made G10 compare x4 pictures with native ones).

| Gate | Script | Pass |
|---|---|---|
| G1/G1g | `hashcheck.sh TAG`: software GS, `HG_GSVK=1`, `+HG_GSVK_INTERP=1`, `HG_GSVK_SCALE=5` against `ref/hashA.md5` (`HG_ROUTE=cs` for `ref/cs.md5`, G2) | all identical |
| G3 | `soak.sh TAG [env]` | identical to `ref/soak.md5` |
| G5 | `gpucheck.sh TAG [batch]`: the GPU against the CPU word for word in four ranges | 0 differing words / batches |
| G6 | `HG_VI_SELFTEST=1` with `HG_GSVK=1 HG_GSVK_INTERP=1` (e.g. a `runhash.sh` run; the summary is in its log) | 0 differ |
| G7 | `wintest.sh TAG [env]` (window, muted, Castle Suite), then `pacing.py $HG_SCRATCH/win_TAG.log` | speed 1.00x, median 16.68 ms (8.34 at 120 fps), no repeats, none out of order |
| G10 | `picgate.sh TAG [env]` (pictures shown, stage 1 and the Castle Suite), then `piccmp.py REF_DIR NEW_DIR` | copy-0 pictures identical; upscaled and in-between ones within 1 level in at most 0.01 % of pixels |
| Options (phase 3) | `optionstest.sh TAG [env]`: the Options menu's extra rows driven by a script (every row changed, both mapping pages, Reset Defaults, live resolution and frame-rate switches); add `HG_PADREC=DIR` to record it, `HG_PADPLAY=DIR` to replay | two runs identical (frames every 5 VBlanks); the recording's replay identical too |
| Saving (0.1) | `savetest.sh TAG`: LOAD GAME (the user's save 02), Circle at the Castle Suite clock, save into slot 03; the same onto a card whose save directory is deleted in the meantime (Create new data file); overwrite slot 03; then LOAD GAME from each card and from the user's own card | every in-room frame after loading identical to loading save 02, positions equal, `icon.sys`/`icon00.ico` identical to the user's card, card files identical to `ref/save.md5`; prints PASS |
| Sound (SPU2) | `audiocheck.sh TAG [REC [EXE]]`: replays a recording (default `recordings/20261015-184308`, New Game to the guest room) headless without drawing from a scratch copy, the mix to a WAV, then `audiocheck.py` | PASS: no stuck tone 250 Hz-12 kHz for >= 8 s, at most 2 s with \|DC\| > 300, at most 15 clicks per 3 min |
| Rumble (phase 4) | `rumbletest.sh TAG [env...]`: the game's Vibration test from the Options menu (Off, On) and the controller page's Vibration and Light bar entries, headless, with `HG_RUMBLELOG=1`; `HG_PADREC=DIR` / `HG_PADPLAY=DIR` record and replay it. Also: `soak.sh TAG HG_RUMBLELOG=1` (the session vibrates: 7 `rumble:` lines) | the `rumble: VBlank N small S big B` lines identical in every run and replay |

Timing (G8): `abba.py BIN_A BIN_B [MAXVB [FROM_VB]]` from `recomp/` (EE thread and VU1 ms per VBlank, A-B-B-A with
the drift; `HG_ROUTE=cs` for the Castle Suite, `HG_GSDRAW=0` to leave the GS out). VU1 lockstep (G4): any run with
`HG_VU1CHECK=1`; the log's last `VU1CHECK:` line counts runs and mismatches, `VU1 AOT:` the handovers (must be 0).

GPU time per VBlank is in every run's log every 600 VBlanks (`GSVK GPU ms per VBlank`); `HG_GSVK_TIMING=1` splits it
by phase (exact batches, upscaling copies, single primitives, compose, transfers; and the same for the replays of
frame interpolation).
