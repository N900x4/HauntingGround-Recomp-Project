# Haunting Ground Recomp Project

A static recompiler that turns **your own copy** of *Haunting Ground* (PS2, NTSC-U SLUS-21075) into a native PC program, with the long-term goal of a remastered Windows and Linux version: 4K rendering, high-resolution textures, 60 fps, and Hewie voiced with real dog recordings.

> **Status: early research.** The recompiled game boots natively and reaches the game's boot screens, but nothing is drawn yet and there is no sound. It is **not playable**. See the [roadmap](#roadmap).

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
- The game's frame loop runs and builds real graphics command lists. Nothing renders them yet.
- 1,104 distinct game functions have run natively, with no mistranslated returns in about 1 billion calls.
- It stops at the boot notice screen, which waits for a controller press, and then at a later step that probably needs the memory card or the CRI logo movie.

## Building (Linux, experimental)

```sh
python3 tools/ingest.py /path/to/your/HauntingGround.iso game   # NTSC-U v1.01 only for now
# or, to use files already extracted elsewhere: export HG_GAME_DIR=/path/with/SLUS_210.75+DATA.CVM
cd recomp
python3 bulk2.py gen 16          # generate C (~15–40 s)
make -j2                         # compile (~1–3 min)
./hgboot -q -t 60 > run.log      # run for 60 s; a summary is printed at the end of the log
```

Requirements: Python 3 (with numpy for the format tools), gcc or clang, about 2 GB of free disk space. No PS2 BIOS is needed.

## Roadmap

| Phase | Goal | Gate |
|---|---|---|
| 1 | Title screen: controller and memory card, CRI logo movie, 2D graphics | Title screen renders with controller input |
| 2 | First controllable room: VU1, full 3D graphics, ADX streaming, Capcom sound driver | Fiona walks in the first room |
| 3 | One-click tool: launcher for Windows and Linux that picks the ISO, builds and runs, plus a mod hook system | A stock Windows PC builds from its own ISO |
| 4 | Full playthrough, then remaster features: 4K and effect rewrites, 60 fps through render interpolation, texture and audio replacement | Credits roll and saves load |

## Repository layout

| Path | Contents |
|---|---|
| `recomp/` | Translator (`rec2.py`, `bulk2.py`, `r5900.py`), function-discovery scans, and the runtime (C) |
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
