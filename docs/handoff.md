# Handoff: where the project stands (2026-10-15)

Read this first when resuming. It gives the state, the rules and the next steps. Details are in the other docs.

## State

The recompiled game boots, plays the logos and movies, and reaches the title, the menu, New Game, the opening movie
and stage 1. Stage 1 has 3D graphics and sound. Rooms beyond the first load and run without crashes (20,000
VBlanks tested). It now runs on the user's Windows PC (MSYS2 build), at 0.51x real time in stage 1 and 0.68x in the
Castle Suite. The GS thread is the limit.

| Area | Status | Doc |
|---|---|---|
| Boot, pad, memory-card HLE | Done. LOAD GAME works: a save made on PCSX2 (extracted with `tools/verify/ps2mc.py`) loads into the Castle Suite (`ST_021`) at the saved position. Saving from our build verified (`tools/verify/savetest.sh`: new slot, empty card, overwrite; reloads identical). | `phase1-runtime-notes.md` |
| VU1, VIF1, GIF, software GS | Done; checked against PCSX2: stage 1 has 99.7% of pixels within 8 levels, the Castle Suite matches | `renderer-performance.md`, below |
| IOP and sound | The game's own IOP modules run on an emulated IOP plus SPU2. SFX, ADX streams and BGM .SQ are verified. | `phase2-audio-notes.md` |
| Movies | The IPU is emulated, and the game's own Sofdec player runs. Movie audio is bit-exact. | `movies-ipu.md` |
| Exploration | `ST_02A` → `ST_013` → `ST_00D` reached; explorer tools | `exploration-notes.md` |
| Reverb | Unverified: the game sets the depth to 0 at stage init, and no reached room changes it | `exploration-notes.md` |
| Settings | Extra rows in the game's own Options menu (resolution, frame rate, display, vsync, key and button mapping); `hg.ini` next to the exe; environment variables override | `options-menu.md` |
| Controllers | DualSense / DualShock 4 / Xbox through SDL (USB and Bluetooth); the game's vibration on the motors (strength setting); static light bar; touchpad = Select; first to press is player 1. Hardware checklist pending with the user | `controller.md` |

Last commit before this doc: `bf60152`.

### 2026-10-15 (night): release 0.1, saving verified

- `tools/verify/savetest.sh` (headless, about 90 s): LOAD GAME of the user's save 02 from a copy of the card, Circle at
  the Castle Suite clock (the save point), "Save current game?" YES, slot 03; the same onto a card whose save directory is
  deleted while Fiona stands in the room (the game asks "Create new data file?" and writes the directory, `icon.sys`,
  `icon00.ico` and the save file); an overwrite of slot 03 ("Game data already present", YES); then LOAD GAME from each
  card and from the user's own card. Result: every in-room frame after loading each new save identical to loading save
  02, same stage and position as at the save, `icon.sys`/`icon00.ico` byte-identical to the ones PCSX2 wrote, card
  files deterministic (`ref/save.md5`). The saved slot differs from save 02 in 9 bytes (checksum, play time, a timer).
- `HG_CLOCK=SECONDS` (`iop_pad_mc.c`, tests): the game's clock is that fixed start plus emulated time without recording,
  so saves and their dates repeat. Unset, nothing changes.
- The New Game route to the first clock is not scripted (it needs the opening rooms' story steps); the save path the
  test exercises is the same game code.

### 2026-10-15 (late): roadmap phase 4, native DualSense (`docs/controller.md`)

- **The game's vibration, decoded**: the vibration mixer `0x2d42a0` sets (small on/off, big 0-255) through
  `0x1be1f0`. The pad task `0x1be4b0` reads libvib's actuator profile (`sceVibGetProfile`, DBCMAN SRData command
  `0x01038002`) and sends changes with `sceVibSetActParam` (a no-wait RPC to DBCMAN `0x8000131e/f`, command
  `0x0103400b`, block `{profile size, data size, data, profile}`, data packed per profile bit: bit 0 small 1 bit, bit
  1 big 8 bits). `iop_pad_mc.c` decodes it and hands the motors to the host each VBlank (`host_rumble`).
  `HG_RUMBLELOG=1` logs the changes per VBlank (`=2` adds the bytes).
- **One answer to the game changed, on purpose**: the HLE answered every SRData read with `0x01`, so the profile
  said "small motor only" and the game never sent the big motor. It now answers `0x03` (a DualShock 2) for the
  profile command (`HG_VIBPROFILE=1` gives the old answer). Effect on the game: the profile byte in the pad object,
  six more instructions a frame, two data bytes instead of one. The sends happen at the same VBlanks. All references
  stay identical (below).
- **Host** (`host_sdl.c`): every controller opened (up to 8). The first to press a button is player 1 (player LED;
  the only one read; the next to press takes over when it goes). PS4/PS5 extended reports on, for Bluetooth rumble
  and light bar. Rumble = the big motor on the low-frequency motor and the small one on the high-frequency motor,
  times the strength, refreshed every 100 ms and lasting 250 ms (it stops by itself if the game stops).
  `SDL_GameControllerSetLED` with the light bar colour. Touchpad click = Select. Radial dead zone. A
  `gamecontrollerdb.txt` next to the exe is loaded. The connection log line gives type and features, and flags
  Steam's virtual controller.
- **Settings**: `vibration` (off/low/medium/high, default high), `lightbar` (amber default `401a00`, red, blue,
  white, off, `RRGGBB`), `deadzone` (18 %), `touchpad_select` (1). The first two are entries on the Options
  controller page (below Select, edited like the display rows) and are written to a recording's `# set` header.
  Names: "DualSense" (Create, Mute) for PS5, "PlayStation" (Share) for PS3/PS4 (`padtype` 3 is new).
- **The user's DualSense on this PC (USB, nobody holding it)**: SDL 2.32.10 opens it through HIDAPI with Steam
  running; type PS5, rumble, light bar and touchpad reported. SetLED and two short low rumbles returned success.
- **Gates**: G1/G1g 702/702 identical (software, GPU, +60, x5); G2 Castle Suite 742/742 each; G3 soak with
  `HG_VU1CHECK=1`: 1,005/1,005 identical, 17.6 M runs, 0 mismatches, 0 handovers; the session vibrates (7 motor
  changes) and two replays log the same lines; `tools/verify/rumbletest.sh` (the game's Vibration test): two runs, a
  recording and its replay identical; `optionstest.sh` two runs identical, only the 37 controller-page frames differ
  from phase 3; clean build 74 s; muted window runs with no controller and with the DualSense (player 1, rumble sent
  at the game's test). Details: `docs/controller.md` section 5.
- **Manual checklist** for the user (USB and Bluetooth): `docs/controller.md` section 8.

### 2026-10-15 (evening): roadmap phase 3, in-game settings as native rows in the game's Options menu

- **Research (3.0, `docs/options-menu.md`)**: the Options page is a C++ object whose rows are hard-coded in two
  functions, "choose a row" `0x393bd0` and "draw the page" `0x391450`; the row byte `obj+0xa8c56` is touched only by
  them and the init. Every picture (panel pieces, icons, arrows, cursor), text, sound and the Reset Defaults dialog is a
  game function callable from host code. Native rows were judged practical (the user's option (i), Q6).
- **The rows** (`recomp/game/options.c`, hooks on those two functions): Down past Screen Position turns to a page with
  Renderer (next start), Resolution (native, x2-x6, auto), Frame rate (30/60/120; 120 at native), Display (window /
  full screen), V-Sync (fast / on / off), Keyboard and Controller; a note line "60 and 120 fps add about 17 ms of
  delay". Keyboard and Controller open mapping pages (press a key / a button; Esc cancels; swaps; PlayStation names when
  SDL reports a PS3/PS4/PS5 pad, Xbox names otherwise). The game's own panel, glyphs, bars, colours, sounds, footer and
  YES/NO dialog; the same in the in-game sub-screen's Options tab (L1/R1 still switch tabs). Strings live on the guest
  stack during the hook; panel rectangles through a piece-table entry restored within the call: game memory unchanged.
- **Settings** (`recomp/settings.c/.h`): a typed struct, `hg.ini` next to the exe (`HG_INI=path|off`), environment
  variables override (`HG_GSVK`, `HG_GSVK_SCALE`, `HG_GSVK_INTERP`, `HG_VSYNC`, `HG_SCALE`, new `HG_FULLSCREEN`), one
  `settings:` log line at start. Without `hg.ini` everything is as before. Host settings never touch game memory or
  the memory-card save.
- **Live changes**: resolution and frame rate at the next VBlank on the GS thread (`vk_reconfigure`: GPU finished, read
  back into vm, local memory re-made with `gsvk_resize`, re-uploaded, interpolation restarted; 2-27 ms); the window's
  presenting rate follows; vsync re-makes the swapchain (`gsvk_present_mode`); full screen and the maps at once.
- **Recordings**: the header gets `# set key=value` for every setting; host-originated changes (F11, captured keys and
  buttons, the controller kind) become `set VB key=value` lines; replays apply both and save nothing.
  `HG_HOSTEV=VB:key=value,...` scripts host changes for tests; `HG_PAD` now takes up to 256 events.
- **Gates**: G1/G1g software, GPU, +60, x5: 702/702 identical to `hashA` each; G2 (Castle Suite) the same
  four: 742/742; G3 `soak.sh` with `HG_VU1CHECK=1`: 1,005/1,005 identical to `soak.md5`, 17.6 M runs, 0 mismatches,
  0 handovers, and again on the GPU at x5 + 60: 1,005/1,005; G5 `gpucheck.sh batch`: 0 batches differ in the four
  ranges; `tools/verify/optionstest.sh` (every new row, both mapping pages, both Reset Defaults, live switches to x3,
  60 fps, native and 120 fps): two runs, a recording of it and its replay all give the same 672 frame hashes, and the
  same run in a muted window (full screen and vsync switched live) too; G7 window, Castle Suite: x5 30 fps median
  16.68 ms, x5 + 60 fps 16.74 ms (p5-p95 16.41-16.94), native 120 fps 8.39 ms, each 1.00x, 0 repeats, 0 out of order;
  clean build 76 s. The hooked functions never run in the reference routes or the user's recorded session.
- **Manual checklist for the playtest**: `docs/options-menu.md`, section 11.

### 2026-10-15 (afternoon): roadmap phase 2, VU1 compiled as whole programs

- **Timing analysis** (`recomp/vu1timing.py`, commit `bbd813d`): for every Q consumer (MULq/ADDq) and P consumer
  (MFP) of the 19 programs, all paths explored under both copies of the timing model. Q: 195 SAFE, 5 OLD,
  0 AMBIGUOUS; P: 58 SAFE, 13 OLD, 0 AMBIGUOUS. No JR/JALR, no path leaves a program, the two models never stall
  differently. The OLD ones are the game's own software pipelining.
- **Generator v2** (`recomp/vu1gen.py`, `cd507d1`, `83bf07a`): one C function per program, no cycle bookkeeping;
  Q/P committed where the proof says; flags that are overwritten before anyone can see them only feed the sticky
  status bits; XGKICK in one call to the GS ring (`gif_kick`). Programs the proof would not cover fall back to the
  v1 per-pair code (none do).
- **Checked**: `HG_VU1CHECK` 2.02 M + 1.36 M + 17.66 M runs (New Game, Castle Suite, the whole recorded session),
  0 mismatches, 0 handovers; frames identical to `hashA`/`cs.md5` (software, GPU, +60, x5) and to `soak.md5`;
  `gpucheck.sh batch` clean; window x5 + 60 fps at 1.00x (median 16.72 ms, 0 repeats, 0 out of order).
- **Speed** (`abba.py`): VU1 7.3 -> 2.8 ms per VBlank in stage 1, 6.9 -> 2.7 in the Castle Suite; EE thread
  9.8 -> 5.3 and 9.0 -> 4.8 ms (`HG_GSDRAW=0`). Table and details: `docs/renderer-performance.md`, last section.
- **VU1 thread: not built** (the user's rule Q2: compile, re-measure, decide). The EE thread uses about a third of its
  budget; the design for later is in `docs/renderer-performance.md`.
- `tools/verify/abba.py` now prints VU1 ms too and takes `HG_ROUTE=cs` (Castle Suite from `../memcards`).

### Day 2026-10-15: 60 fps and 4K (the user at work)

- **60 fps works** (`HG_GSVK=1 HG_GSVK_INTERP=1`): each 30 Hz frame is replayed on the GPU with its triangles moved
  halfway towards the previous frame, into separate copies of local memory, and shown between the real frames.
  Castle Suite, windowed: 100 % speed, a present every 16.68 ms, every picture once and in order. The game's own
  memory is untouched (frames identical to `hashA`); `HG_VI_SELFTEST=1` (replay without movement = the real frame)
  passes 1,496/1,496. How it matches triangles between frames, and the limits: `docs/gpu-renderer.md`, "60 fps".
- **4K**: `HG_GSVK_SCALE=5` (3200x2240; a 4K screen shows the 4:3 picture at 2880x2160) runs at full speed: the
  upscaling copies are now made by the GPU, any factor 2-8, and the window's picture is collected one VBlank late
  instead of waiting for the GPU. x6 78 %.
- **Not yet**: 4K and 60 fps together (x4: 74 %, x5: 59 %); 120 fps (`HG_GSVK_INTERP=2` works but at 87 % speed,
  parked at the user's request). The fix for both: read the in-between pictures back asynchronously too, then
  present from video memory.
- **Phase 4 (CPU) second part not started**: VU1 whole-program compilation and VU1 on its own thread. Not needed for
  60 fps (the game logic stays at 30), but it is the CPU headroom for everything else.
- The window now runs on its own UI thread (input and presents); `play.bat` lists the new options.
- Speed-up found on the way: `gsvk_prim` read back video memory over PCIe for every primitive; normal GPU rendering
  9.5 -> 8.4 ms per VBlank.

### Night 2026-10-15 (unattended): roadmap Phases 4 and 5

- **Phase 4, VIF1 unpack fast path (`341113c`).** `unpack_fast()` in `dmac.c` covers every unpack except
  `vl == 3` and `wl > cl`; DIRECT, MPG, UNPACK and STROW payloads go over in bulk. `HG_UNPACKCHECK=1` compares it
  with the old generic routine: 76.7M unpacks, 0 mismatches. Frames identical (`hashA`, Castle Suite 742/742).
  EE thread in stage 1: 11.20 → 9.8 ms per VBlank (−12.3 %, `tools/verify/abba.py`, drift 0.04 ms).
- **Phase 4, not done:** VU1 programs compiled as a whole with timing worked out at build time, and VU1 on its own
  thread. The VU1 code is now about 64 % of the EE thread, so this is the next CPU win; it needs a static timing
  analysis of the microprograms first.
- **Phase 5, GPU renderer: a bit-exact Vulkan compute GS, and a live mode** (`docs/gpu-renderer.md`).
  - Every primitive type and format is drawn on the GPU exactly as `gs.c` draws it, checked word for word over the
    4 MB local memory (`HG_GSVKCHECK`): 742,414 primitives in menus, stage 1, Castle Suite and `ST_020`, 0 differ.
  - `HG_GSVK=1` draws the game with the GPU only. Frames to VB 4000: 702/702 identical to `hashA`. Per VBlank:
    stage 1 9.47 ms against 10.3–10.5 with the software GS, Castle Suite 8.42 against 8.63; the GS thread is now
    15–29 % busy, and the EE thread is the limit. `play.bat` takes `set HG_GSVK=1` (untried in a window).
  - Packages installed with the user's approval: MSYS2 `mingw-w64-ucrt-x86_64-vulkan-devel` and `...-glslang`.
  - Do not combine `HG_HOSTPROF=1` with `HG_GSVK`/`HG_GSVKCHECK`: such a run hung during boot.
- **Phase 5, upscaling, first version** (`HG_GSVK=1 HG_GSVK_SCALE=2|4|8`): N x N sub-pixel copies of local memory
  besides the exact one, interleaved into an N times larger picture; render targets read with the copy's offset.
  Windowed, Castle Suite: x2 (1280x896) 100 % speed, x4 (2560x1792) 96 %. Copy 0 stays exact (frames identical to
  `hashA` at x4, and the user's whole recorded session, 20,600 VBlanks, 1,005/1,005 frames identical to the
  software GS: `D:/ps2-work/scratch/soak.sh`). Pictures: `D:\ps2-work\scratch\castle_suite_x4_2560x1792.png` and
  `castle_suite_native_vs_x4_closeup.png`. Textures stay native; 4K output and more speed are next.

### Night 2026-10-14/15 (unattended): playtest 1 findings, Castle Suite, frame timing, crash reports

- **Beeps (playtest 1): fixed.**
  - The game's own mix, replayed with `HG_WAV`, has no faulty tones. `tools/verify/tones.py` finds only menu music
    onsets, interaction sounds and footsteps; clips are in `D:\ps2-work\scratch\beeps`.
  - The cause was the live SDL queue: 0–49 ms deep on the real device, giving 20 underruns (ticks) in 3 minutes.
    Playback now starts, and restarts after an underrun, with 100 ms queued, and drops only beyond 250 ms. Result:
    0 underruns and 0 drops, queue 51–99 ms (`023da10`). `HG_AUDIO_MUTE=1` (test only) sends silence to the real
    device.
- **Dark drifting specks by a door (playtest 1): open.**
  - They are in stage `0x20` (Castle Suite `0x21` → `0x10` → `0x20`). The traced pixel comes from a real dark,
    alpha-tested texture strip (`tbp 15360`, T8). It is not a coverage gap, and VU1 rounding (`HG_VU1CHOP`) does
    not change them.
  - In the Castle Suite, PCSX2 shows isolated dark pixels too (11 of our 14 at the same places), so these are partly
    genuine.
  - Deciding needs PCSX2 in stage `0x20`. Its Fiona cannot open the Castle Suite's far door with save 01; the door was
    open in the user's save 02, which exists only on our card. A folder memory card made from our card is seen as
    unformatted (no `_pcsx2_superblock`).
  - Ways forward: the user walks PCSX2 there and saves a state (about 2 minutes); permission to use their own PCSX2
    states; or format a folder card in PCSX2's GUI and copy our save in.
- **Castle Suite against PCSX2: passes.** Save 01 loaded in both: 99.8–99.9 % of pixels within 8 levels, mean
  difference 1.6 (`docs/rooms-checklist.md`).
- **Frame timing: the game runs at 30 fps.** There are two hard-coded VBlank waits per frame; the frame driver is
  `0x1beef0`. 60 fps needs frame interpolation (`docs/frame-timing.md`).
- **Crash reports and the F3 overlay:** see the commits. Also fixed: the window is DPI aware (it looks smaller on
  scaled displays; F11 or `HG_SCALE`).
- **PCSX2 navigation, as learned:**
  - A PINE teleport moves Fiona's picture, but she cannot walk afterwards. Teleport only for screenshots.
  - `pcsx2_walk.py` steers her by her live position, with the D-pad learned from her movement. It followed the user's
    whole Castle Suite route.
  - Pad keys reach PCSX2 only while its window has focus; hotkeys (F8, Space) work anyway.
  - The VM's state 1 has a stale memory card directory: cold-boot instead (`pcsx2_ref.py start 0`).
  - MSYS2 rewrites `path:number` in environment variables: pass Windows-style paths (`D:/...`).

### Session 2026-10-14, input recording, playtest speed, room checklist

- **Input recording:**
  - `HG_PADREC=DIR` (or `auto`: `../recordings/YYYYMMDD-HHMMSS`) writes `DIR/card` (the memory card as it was at the
    start, file times kept) and `DIR/pad.txt` (start time and every pad-state change with its VBlank).
  - `HG_PADPLAY=DIR` replays it from a fresh copy of that card (`DIR/replay_card`), ignoring live and scripted input.
  - While recording or replaying, the game's clock (CDVD RTC, memory card file dates) is the start time plus emulated
    time. The load screen picks the newest save by date, so this matters for logic, not only for display.
  - `play.bat` records every session; `replay.bat DIR` replays one in a window. For a headless replay at full speed,
    from the UCRT64 shell: `HG_PADPLAY=DIR HG_WINDOW=0 ./hgboot.exe -q`.
  - Checked: a LOAD GAME → Castle Suite session recorded in a window (paced) and replayed headless gave 311 of 311
    identical frames.
  - A replay holds only while the build runs the game the same way; any change to emulated timing invalidates older
    recordings.
- **Playtest speed:**
  - The real setup (window, paced) holds 59.94 VB/s in stage 1 with 11.4 ms of work per VBlank. The worst single
    VBlanks take 22–24 ms and are absorbed by the pacing.
  - `HG_SPEEDLOG` now leaves the pacing sleep out (busy, worst).
  - The EE and GS threads run one step above normal priority on Windows.
  - Earlier dips to 55 VB/s happened while another heavy process (a research agent) shared the 8 logical CPUs.
- **Room checklist:** `docs/rooms-checklist.md`, 131 rooms from three GameFAQs walkthroughs and the Fandom wiki,
  cross-checked. It covers clocks (save points), cutscenes, effects and stalker areas per room. Unverified: TCRF
  reportedly calls `ST_00D` an unused stage, yet our explorer reached a stage numbered 0x0D from `ST_013`. Check
  this when mapping stage numbers to rooms.

### Session 2026-10-14, PCSX2 reference on Windows

- The user's own PCSX2 2.8.1 install runs a separate, unattended instance: `-datapath D:\ps2-work\tools\pcsx2-data`
  (data in its `PCSX2\` subfolder: the VM's settings with PINE on slot 28011, the software renderer, copies of the
  BIOS, the VM's memory card and save states 1–5). The user's own `Documents\PCSX2` has their personal Haunting Ground
  states in the same slot numbers and must not be touched.
- `tools/verify/pcsx2_ref.py start SLOT | shot OUT.png | stop`: boots the ISO from a save state with the main window
  hidden (`-nogui`) and the game window minimized. Shots are PCSX2's own F8 screenshots at raw internal resolution
  (640x448, `ScreenshotSize = 2`). Window captures would include the user's RivaTuner overlay, so they are not used.
  Fully windowless running is not possible: PCSX2 always creates a game window.
- `pine.py` uses TCP 127.0.0.1:28011 on Windows (`PINE_SLOT`); `wincap.py` finds windows, captures them and posts
  keys; `pcsx2_teleport_cmp.py` works on both systems.
- Check: our stage 1 start frame against PCSX2's state 4 matches 99.8% within 8 levels (the VM gave 99.7%). A
  teleport run over 10 explorer positions took 39 s; the later positions differ by camera history, as before.

### Session 2026-10-14, GS workers: full speed

- `HG_GSTHREADS=n` (default: half the CPUs, at most 8; 0 turns them off): the GS thread parses and sets up draws, and
  n workers rasterize. Each worker draws every primitive but only its own 8-row bands. The output is identical to
  serial drawing because dependencies are tracked per 8 KB page:
  - texture reads of pages that queued draws write (and the reverse) wait for the workers;
  - writes to pages that queued draws write with a different layout wait too;
  - draws that read their own frame buffer run serially. Sprites use their exact texel rectangle, from the
    renderer's own expressions; triangles use the whole texture.
  - CLUTs are snapshotted per load, and FOGCOL per draw.
- Checked against serial drawing, frame for frame:
  - New Game → stage 1: 702 frames to VB 4000, identical to `hashA.md5`;
  - LOAD GAME → Castle Suite: 742 frames;
  - logos, movies and title: 502 frames.
- Speed with 4 workers (Windows reports 8 logical CPUs; 5 or 6 workers are slower):

  | Scene | Speed | EE thread per VBlank |
  |---|---|---|
  | Stage 1 | 1.49x real time | 11.2 ms (VU1 7.3) |
  | Castle Suite | 1.63x real time | 10.2 ms (VU1 6.9) |

  The EE thread is now the limit. About 4% of draws are drawn serially (self-reading passes), and there are about
  25 waits for the workers per frame (each effect pass reads the previous pass).
- With a window the game is paced to real time. That is full speed for playtesting at native resolution.

### Session 2026-10-13, VU1 ahead-of-time compilation

- `recomp/vu1gen.py` (run by `bulk2.py`) extracts the 19 VU1 programs from the ELF and writes `gen/vu1_aot_NN.c`.
  Every instruction pair is the interpreter's own `step()` (`recomp/vu1_core.h`) on constant words, so decode and
  dispatch fold away and the semantics cannot drift. Functions are cut at 64 pairs; one big function per program
  took 14 minutes to compile, and now the whole build takes 1.5 minutes. After an MPG upload the program in micro
  memory is identified by its code. Anything outside the compiled code goes to the interpreter with the exact
  state. `HG_VU1AOT=0` turns it off; it is not used with a non-default `HG_VUSTALL` or `HG_VUFLAGDELAY`.
- Checked: frames identical to `hashA.md5`, and `HG_VU1CHECK` (which now checks the compiled code against
  `vu1_ref.c`) found 0 mismatches in 2,000,000 runs, with no handovers to the interpreter.
- Speed in stage 1 with drawing off (`HG_GSDRAW=0`): VU1 went from 9.9 to 7.4 ms per VBlank, and the whole EE
  thread needs 11.2 ms per VBlank (1.49x real time). The GS thread is now the only thing between us and full speed.

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
- PCSX2: the user's Windows install (2.8.1). Their own data is `C:\Users\USER\Documents\PCSX2`: hands off. Our
  reference instance uses `D:\ps2-work\tools\pcsx2-data` through `tools/verify/pcsx2_ref.py` (see the session above).
- Verification scripts: `tools/verify/` (see below). Paths are repo-relative, and the PCSX2 tools work on Windows.

## Waiting on the user

- The door specks in `ST_020` need PCSX2 in that room: the user saves a state there (about 2 minutes), allows us to
  use their own PCSX2 states, or formats a folder memory card in PCSX2's GUI so our save can be copied in.
- A playtest with `set HG_GSVK=1` (GPU renderer), then `set HG_GSVK_INTERP=1` (60 fps) or `set HG_GSVK_SCALE=5`
  (4K), F11 for fullscreen.
- The user's playtest (controls, live sound, saving from our build).

## Next steps, in order

GPU and speed (the roadmap's Phases 4 to 6; details in `docs/gpu-renderer.md`):

- GPU renderer: x4 upscaling to full speed with margin (shared-memory primitive loads, double-buffered command
  buffers), 4K output, present from video memory; check the effect passes room by room at x4.
- VU1: whole-program compilation with build-time timing, then its own thread (the EE thread limit).

Accuracy and play (from before):

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
