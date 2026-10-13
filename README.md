# Haunting Ground Recomp Project

A static recompiler that turns **your own copy** of *Haunting Ground* (PS2, NTSC-U SLUS-21075) into a native PC program, with the long-term goal of a remastered Windows and Linux version: 4K rendering, high-resolution textures, 60 fps, and Hewie voiced with real dog recordings.

> **Status: early research.** The recompiled game boots through the logos, title and menu into the first room, where Fiona can be walked around. The 3D is drawn by a software GS through a VU1 interpreter. Sound comes from the game's own IOP sound drivers (Capcom SNDDRV, CRI ADX) running on an emulated IOP with an SPU2 core; the stage ambience is bit-exact against an independent decode. Movies play through an emulated IPU running the game's own Sofdec player (pictures match an independent decoder within IDCT rounding; movie sound is bit-exact). Graphics accuracy is not yet checked against reference frames, and 3D runs below real time. It is **not playable** yet. See the [roadmap](#roadmap), [phase 1 notes](docs/phase1-runtime-notes.md) and [audio notes](docs/phase2-audio-notes.md).

## How it works

No game code or data is ever distributed. You supply your disc image, and the tool builds the game on your machine:

1. **Ingest** – `tools/ingest.py` reads your ISO read-only, verifies it by SHA-256 of the game executable, and copies the two files the build needs into `game/`.
2. **Translate** – `recomp/bulk2.py` translates all 11,499 functions of the PS2 executable (MIPS R5900) into C.
3. **Compile** – the generated C is compiled with gcc or clang.
4. **Link** – the result is linked with a hand-written runtime that stands in for the PS2 hardware and kernel (`recomp/runtime.c`, `hle.c`, `interp.c`).
5. **Run** – game assets are read from your `DATA.CVM` at run time.

## What works today

Measured in the [boot test](docs/boot-test-report.md):

- The recompiled code runs natively from the PS2 entry point. It gets through the C++ static constructors, `main` and the loading of all 12 IOP modules, mounts `DATA.CVM`, and reads real game files from it.
- Controller (DBCMAN/libpad2) and memory card (MCSERV/libmc, backed by a host folder) are emulated at the IOP level. Input is scripted for now (`HG_PAD`).
- Movies (logo, intro, opening cinematic) play: the game's own CRI Sofdec player runs on an emulated IPU with its DMA channels ([movie notes](docs/movies-ipu.md)). `HG_MOVIES=skip` skips them.
- A software GS renders the logos, title, menu and, through a VU1 interpreter running the game's own microprograms, the 3D of stage 1 with its full-screen effect passes. Rendering runs on its own thread with AVX2 paths; in stage 1 the build runs at about half of real time on a 2-core VM ([renderer performance](docs/renderer-performance.md)).
- Sound comes from the game's own IOP drivers on an emulated IOP and SPU2: sound effects, CRI ADX streams (bit-exact against an independent decode) and SNDDRV background music (every note checked against the sequence and bank; see the [audio notes](docs/phase2-audio-notes.md)).

## Building (Linux, experimental)

```sh
python3 tools/ingest.py /path/to/your/HauntingGround.iso game   # NTSC-U v1.01 only for now
# or, to use files already extracted elsewhere: export HG_GAME_DIR=/path/with/SLUS_210.75+DATA.CVM
cd recomp
python3 bulk2.py gen 16          # generate C (~15–40 s)
make -j2                         # compile (~1–3 min)
mkdir -p /tmp/frames
HG_FRAMES=/tmp/frames HG_MAXVB=16000 HG_PAD=1500:start,1800:start,2100:cross \
  ./hgboot -q -t 3000 > run.log  # skip the intro, New Game, opening cinematic, stage 1; frames go to /tmp/frames
```

### Windows (MSYS2 UCRT64, experimental)

Install [MSYS2](https://www.msys2.org/), then in an MSYS2 shell: `pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-pkgconf`. Ingest and generate with any Python 3 as above, then build and run from the **UCRT64** shell (it puts `SDL2.dll` on the `PATH`):

```sh
cd recomp
make -j16                        # builds hgboot.exe
./hgboot.exe                     # window with live input; HG_WINDOW=0 ./hgboot.exe -q for headless runs
```

The Windows build replaces the Linux-only calls in `compat.c`: fibers instead of ucontext, a timer thread instead of SIGPROF/SIGALRM (the profile samples wall time, not CPU time), VirtualAlloc, and its own glob matcher for memory-card directory listings.

With SDL2 installed (`libsdl2-dev`) the build opens a window and takes live input; just run `./hgboot` (no time limit when windowed, Esc quits, F11 toggles fullscreen). `HG_WINDOW=0` forces headless mode.

| PS2 | Keyboard | Gamepad |
|---|---|---|
| D-pad | arrow keys | D-pad |
| Cross / Circle / Square / Triangle | X / C / Z / S | A / B / X / Y |
| Start / Select | Enter / Right Shift | Start / Back |
| L1 / R1 / L2 / R2 | Q / E / 1 / 3 | shoulders / triggers |
| Left stick | I J K L | left stick (right stick too) |

Requirements: Python 3 (with numpy for the format tools), gcc or clang, optionally SDL2, about 2 GB of free disk space. No PS2 BIOS is needed.

## Roadmap

| Phase | Goal | Gate |
|---|---|---|
| 1 | Title screen: controller and memory card, CRI logo movie, 2D graphics | Title screen renders with controller input (**reached**: window with keyboard/gamepad input) |
| 2 | First controllable room: VU1, full 3D graphics, ADX streaming, Capcom sound driver | Fiona walks in the first room |
| 3 | One-click tool: launcher for Windows and Linux that picks the ISO, builds and runs, plus a mod hook system | A stock Windows PC builds from its own ISO |
| 4 | Full playthrough, then remaster features: 4K and effect rewrites, 60 fps through render interpolation, texture and audio replacement | Credits roll and saves load |

## Repository layout

| Path | Contents |
|---|---|
| `recomp/` | Translator (`rec2.py`, `bulk2.py`, `r5900.py`), function-discovery scans, and the runtime (C): CPU glue and kernel (`runtime.c`, `hle.c`, `interp.c`), controller and memory card (`iop_pad_mc.c`), GS and DMA (`gs.c`, `dmac.c`), game-function overrides (`overrides.c`) |
| `data/` | Per-version metadata: function starts, jump tables, syscall map. Addresses only, no game bytes |
| `tools/ingest.py` | Disc reader and verifier |
| `tools/formats/` | Format tools: Sony HD/BD sound banks (`hdbd.py`, `vagenc.py`), CRI ADX (`adx.py`), textures (`tex_hg.py`) |
| `tools/research/` | Analysis scripts from the feasibility study. Their paths assume the original research layout |
| `docs/` | Feasibility findings: executable structure, hardware use, audio, textures, boot test, C vs C# |
| `game/` | **Your** game files go here. Git-ignored and never committed |

## Legal

- This project contains **no** Capcom or Sony code, assets or BIOS files, and none may be contributed.
- You need your own legally obtained copy of the game. PS2 discs are not encrypted, and the tool circumvents no copy protection.
- Generated code and build outputs are derived from your disc and stay on your machine. Never upload them.
- *Haunting Ground* is a trademark of Capcom. This project is not affiliated with or endorsed by Capcom or Sony.

## Credits and prior art

- [PCSX2](https://github.com/PCSX2/pcsx2): the reference for PS2 behaviour. Future runtime components may reuse PCSX2 code under GPL-3.0, with full attribution. This is not an official PCSX2 project; please don't report bugs from this project to them.
- Static recompilation projects that showed the way: N64Recomp/Zelda64Recomp, XenonRecomp/UnleashedRecomp, PS2Recomp, and DQ8-Recompiled.

## Licence

GPL-3.0. See [LICENSE](LICENSE).
