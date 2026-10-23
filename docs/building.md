# Building from source (developers)

Players do not need this: the release's `HauntingGround.exe` does all of it (see the README). This page is for
working on the recompiler and the runtime. The state of the project, its rules and the verification gates are in
[handoff.md](handoff.md) and [next-session.md](next-session.md).

## How it works

No game code or data is ever distributed. You supply your disc image, and the build happens on your machine:

1. **Ingest**: `tools/ingest.py` reads your ISO read-only, checks it by the SHA-256 of the game executable, and copies
   the files the build and the game need into `game/` (`SLUS_210.75`, `DATA.CVM`, `MODULES/`). The release launcher
   does the same in `tools/release/setup.py`, checking every file against `tools/release/disc.json`.
2. **Translate**: `recomp/bulk2.py` translates all 11,499 functions of the PS2 executable (MIPS R5900) into C
   (`recomp/gen/part*.c`, `table.c`), and `vu1gen.py` compiles the 19 VU1 microprograms into C (`gen/vu1_aot_*.c`).
3. **Compile and link** with the hand-written runtime that stands in for the PS2 hardware and kernel
   (`recomp/runtime.c`, `hle.c`, `gs.c`, `gsvk.c`, `iop_*.c`, `spu2.c`, `ipu.c`, `host_sdl.c`, ...).
4. **Run**: game assets are read from `game/DATA.CVM` at run time; IOP modules from `game/MODULES`.

## Windows (MSYS2 UCRT64; the main platform)

Install [MSYS2](https://www.msys2.org/), then in an MSYS2 shell:

```sh
pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-pkgconf \
          mingw-w64-ucrt-x86_64-vulkan-devel mingw-w64-ucrt-x86_64-glslang
```

Ingest and generate with any Python 3 (3.10 or later; only the standard library is used), then build and run from
the **UCRT64** shell (it puts `SDL2.dll` on the `PATH`):

```sh
python tools/ingest.py "/path/to/Haunting Ground (USA).iso" game    # NTSC-U v1.01 only
cd recomp
python bulk2.py gen 16          # generate C, including the 19 VU1 programs (about 20 s)
make -j16                       # builds hgboot.exe (about 75 s from clean on 16 threads)
./hgboot.exe                    # window with live input; HG_WINDOW=0 ./hgboot.exe -q for headless runs
```

`recomp/play.bat` starts a windowed session with logging and input recording, and lists the useful environment
variables. From PowerShell or another shell:
`MSYSTEM=UCRT64 CHERE_INVOKING=1 C:\msys64\usr\bin\bash.exe -lc "cd /d/.../recomp && make -j8"`.

The Windows build replaces the Linux-only calls in `compat.c`: fibers instead of ucontext, a timer thread instead of
SIGPROF/SIGALRM, VirtualAlloc, and its own glob matcher for memory-card directory listings.

If you change the Makefile's compile or link rules, make the same change in `tools/release/hgbuild.py`
(`tools/release/mkrelease.py` checks that they agree before it builds a release).

## Linux (developers; CachyOS / Arch, 2026-10-23)

Packages (Arch names): `gcc make python sdl2` (or `sdl2-compat`), `vulkan-headers vulkan-icd-loader glslang`, a
Vulkan driver; for the gates `python-numpy python-pillow`, and `gamescope` for window tests without a screen.

```sh
python3 tools/ingest.py "/path/to/Haunting Ground (USA).iso" game    # NTSC-U v1.01 only
cd recomp
python3 bulk2.py gen 16
make -j8                        # builds hgboot (about 70 s from clean on 8 threads)
./hgboot                        # window with live input; HG_WINDOW=0 ./hgboot -q for headless runs
```

The objects go to `recomp/obj-linux/` (`obj/` on Windows), so a drive shared by both systems keeps both builds. The
window uses SDL's Vulkan surface (Wayland or X11, whichever SDL picks), presented from video memory like the
Windows build. On a 64-bit Linux `long` is 64 bits (32 on Windows): the IOP's `strtol` is clamped to 32 bits so that
both builds set up the sound chip the same way (`recomp/iop_kernel.c`, `f_strtol`); without that, 271 of the 702
frames of `hashA` came one VBlank late.

All gate scripts in `tools/verify` run from bash on Linux as on MSYS2 (`hgboot` instead of `hgboot.exe`, scratch
`../scratch` next to the repository unless `HG_SCRATCH` is set); every gate gave the frozen Windows results on
2026-10-23 (see `handoff.md`). Window tests without a screen (the display off):

```sh
PL=58 gamescope --backend headless -W 3840 -H 2160 -r 120 -- sh tools/verify/wintest.sh TAG HG_GSVK=1
python3 tools/verify/pacing.py ../scratch/win_TAG.log
```

gamescope's virtual display keeps exact time (a timer), so its numbers show the game's own pacing, not a monitor's.
The DualSense needs no extra udev rule on systems with Steam's (`/usr/lib/udev/rules.d/60-steam-input.rules`,
USB and Bluetooth). There is no Linux release yet: `tools/release` builds the Windows one.

## Requirements

Python 3 (the format tools in `tools/formats` also need numpy), gcc or clang, SDL2, optionally Vulkan and glslang,
about 2 GB of free disk space. No PS2 BIOS is needed.

## Repository layout

| Path | Contents |
|---|---|
| `recomp/` | Translator (`rec2.py`, `bulk2.py`, `r5900.py`, `vu1gen.py`, `vu1timing.py`), function-discovery scans, and the runtime (C): CPU glue and kernel (`runtime.c`, `hle.c`, `interp.c`), controller, memory card and recordings (`iop_pad_mc.c`), GS (`gs.c`, software) and the Vulkan GS (`gsvk.c`, `vk/`), DMA (`dmac.c`), VU1 (`vu1*.c`), IOP and sound (`iop_*.c`, `spu2.c`), movies (`ipu.c`), window and input (`host_sdl.c`), settings (`settings.c`), game-function overrides (`overrides.c`, `game/`) |
| `data/` | Per-version metadata: function starts, jump tables, syscall map. Addresses only, no game bytes |
| `tools/ingest.py` | Disc reader and verifier |
| `tools/release/` | The release: launcher, first-start build, release builder ([README](../tools/release/README.md)) |
| `tools/verify/` | Verification gates and their frozen references ([REFERENCES.md](../tools/verify/REFERENCES.md)) |
| `tools/formats/` | Format tools: Sony HD/BD sound banks, CRI ADX, textures |
| `tools/research/` | Analysis scripts from the feasibility study |
| `docs/` | Notes: state and rules (`handoff.md`), renderer, audio, movies, options, controllers, known issues |
| `game/` | **Your** game files go here. Git-ignored and never committed |

## Roadmap (original phases)

| Phase | Goal | State |
|---|---|---|
| 1 | Title screen: controller and memory card, CRI logo movie, 2D graphics | reached |
| 2 | First controllable room: VU1, full 3D graphics, ADX streaming, Capcom sound driver | reached |
| 3 | One-click tool: launcher that picks the ISO, builds and runs | release 0.1 (Windows) |
| 4 | Full playthrough, then remaster features: 4K, 60 fps, texture and audio replacement | 4K and 60 fps done; playthrough in progress |

The detailed plan is [roadmap-native-port.md](roadmap-native-port.md).
