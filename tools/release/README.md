# Release tools

How the public Windows release is made and how it works on a player's PC. Distribution option (a): **no game code
or game data in any download**. The zip holds a launcher, the sources, a C compiler and Python; the game is built on
the player's PC from their own disc image.

## Making a release

On the development PC (Windows, MSYS2 UCRT64 as in `docs/building.md`, the python.org Python to bundle, a finished
development build in `recomp/`):

```
python tools/release/mkrelease.py                       # ../scratch/release-win/HauntingGround-Recomp-<version> next to the repository, and the zip
python tools/release/mkrelease.py --out D:/elsewhere --version 0.1 [--no-zip]
```

The output folder must be outside the repository (the script refuses otherwise). About 25 s. It:

1. checks that `hgbuild.py`'s compile and link rules match `recomp/Makefile` (`make -n -B`; flags per object, link
   order and libraries); a Makefile change without the same change in `hgbuild.py` stops it;
2. copies the tracked sources the build needs (`recomp/`, `data/`, `tools/release/`, `LICENSE`) to `engine/src`, and
   compiles our shader (`recomp/vk/gs_sprite.comp`) into `engine/src/prebuilt/gs_*_spv.h` with glslangValidator;
3. stages the compiler into `engine/ucrt64`: gcc, cc1, collect2, as, ld, the headers the build includes (`gcc -M` over
   every kind of translation unit), the libraries and startup objects it links (`ld --trace` on the development
   build's objects), and every DLL those programs load (`objdump -p`, recursively). Each file is mapped to its MSYS2
   package (`pacman -Ql`) for the notices;
4. stages Python into `engine/python`: `python.exe`, `python3XY.dll`, the two Visual C++ runtime DLLs, the standard
   library as a zip without tests, Tk, IDLE, pip, networking and e-mail modules, and a `python3XY._pth` (isolated: no
   user site, no environment variables);
5. builds `HauntingGround.exe` from `launcher.c` (static, no DLLs beyond Windows);
6. writes `README.txt` (the repository's README) with the docs it links (`docs/known-issues.md`, `controller.md`,
   `options-menu.md`, `release/crash-reports.md`), `LICENSE.txt`, `THIRD_PARTY_NOTICES.txt` (from
   `THIRD_PARTY_NOTICES.md` with the package table filled in), `engine/licenses/` and `engine/bundle.json`;
7. zips it (`HauntingGround-Recomp-VERSION-win64.zip`) and prints its SHA-256.

For the first 0.1 build (2026-10-15, never published, before the HD textures): 94 MB unpacked, **36.5 MB zipped**; compiler 82 MB (cc1.exe alone 39 MB), Python 8.7 MB, sources 2.1 MB.

## The release folder on the player's PC

```
HauntingGround.exe        launcher
README.txt  LICENSE.txt  THIRD_PARTY_NOTICES.txt  docs/
engine/                   version.txt, src/, ucrt64/ (compiler), python/, licenses/, bundle.json
-- made at the first start --
game/                     SLUS_210.75, DATA.CVM, MODULES/ copied from the player's disc image (1.6 GB)
bin/                      hgboot.exe (the game), SDL2.dll, libwinpthread-1.dll, vulkan-1.dll, build.txt
hg.ini                    settings (the launcher writes the renderer; the game's Options menu the rest)
memcards/port0/           the memory card (saves)
logs/                     launcher.log, play.log, play.prev.log
recordings/               the input of the last 20 sessions (for exact bug replays)
crashes/                  minidumps, if the game crashed
```

The game runs with `bin/` as its working directory, so its own defaults (`../game`, `../memcards`, `../crashes`,
`../recordings`) all land in the release folder; the launcher adds `HG_INI=..\hg.ini` and `HG_PADREC=auto`.
Environment variables set before starting the launcher still reach the game.

## The first start (`launcher.c`, `setup.py`, `hgbuild.py`)

- The launcher refuses a folder path with non-ASCII characters (the compiler's ANSI paths) and a folder it cannot
  write to, with a message that says to move the folder.
- `setup.py` without `--iso` checks `game/` (sizes, then SHA-256 of every file against `disc.json`); if anything is
  missing it prints `@need-iso`, and the launcher shows its window with **Choose disc image...**.
- With the image: it is opened read-only and never copied whole. ISO 9660 is walked; `SYSTEM.CNF` must boot
  `SLUS_210.75` (other regions get a plain message naming the version); the image must not be shorter than its files
  (cut-off dumps); every needed file is hashed while it is copied, and a mismatch is reported as "not the supported
  release" (the game program differs) or "damaged image / bad dump" (anything else). Compressed images (.zip, .7z, .chd,
  .cso) are refused with a hint to extract or convert them first.
- `recomp/bulk2.py` translates the game program into `build/gen` (about 20 s), `hgbuild.py` compiles with all cores and
  links `hgboot.exe` (64 s on a 9800X3D), the three DLLs are copied next to it, `bin/build.txt` records the release
  and `build/` is deleted. PATH for all of it: the bundled compiler, then `System32` and Windows: nothing the PC has
  installed is used.
- The launcher writes `hg.ini` if there is none: `renderer = gpu` when a Vulkan 1.1 GPU with compute queues is found
  (it names the discrete one, which the game prefers), else `renderer = software` with the reason. If the GPU fails at
  run time, the game falls back to the software renderer by itself.
- Later starts compare `engine/version.txt` with `bin/build.txt`: equal, the game starts at once; different (a new
  release unzipped over the old one), it builds again, reusing the verified `game/` files without asking.
- When the game ends, the launcher reads the last `STOP:` / `==== CRASH` line of `play.log`; anything but the player
  closing the window gives a message with the reason and the files to attach.

Launcher options (for tests and support): `--setup` (build again), `--iso PATH` (no file picker), `--no-start`,
`--batch` (no window or message boxes; exit status: 0 built/played, 1 failed, 2 the game stopped unexpectedly).

## Checks done for 0.1 (2026-10-15)

- End to end, from a fresh copy of the release folder, `PATH=C:\Windows\System32;C:\Windows`, the user's ISO:
  `HauntingGround.exe --batch --no-start --iso ...` built the game in 87-89 s (copy and check 3.5 s, translation 20 s,
  compilation 64 s); the ISO's modification time unchanged.
- The exe it built passes G1: 702/702 frames identical to `tools/verify/ref/hashA.md5` with the software renderer,
  the GPU renderer, GPU + 60 fps and GPU x5, run with no MSYS2 directory on PATH.
- The launcher's game start (headless, `HG_MAXVB`): `hg.ini` read from `..\hg.ini`, recording to `../recordings`, card
  in `../memcards/port0`, the GPU renderer on the discrete GPU, the stop reason reported.
