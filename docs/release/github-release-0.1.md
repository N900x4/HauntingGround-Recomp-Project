<!-- Draft of the GitHub release page for 0.1, the first public release. Tag: v0.1.
     Title: "Haunting Ground Recomp 0.1 (test release)".
     Assets: HauntingGround-Recomp-0.1-win64.zip (tools/release/mkrelease.py --textures, run on Linux with the MSYS2
     tree from the Windows drive under Wine) and HauntingGround-Recomp-0.1-linux-x64.tar.gz
     (tools/release/mkrelease_linux.py), both built from 22108be (after the Windows test: the Esc fix in the launcher, no
     default note in the Options menu). Both under GitHub's 2 GB per file. -->

# Haunting Ground Recomp 0.1 (test release)

The first public test build of a native **Windows and Linux** version of *Haunting Ground* (PS2, North American
release, SLUS-21075). The game's program is statically recompiled to C and compiled on your PC from your own disc; a
hand-written runtime stands in for the PS2 hardware. It is not an emulator and it does not use PCSX2 code.

**No game code is in this download.** You need your own copy of the game, dumped to a plain `.iso`. The launcher checks
your disc image, copies the files the game needs, and builds the game on your PC with the compiler included in the
download (about a minute on a fast PC). Nothing needs to be installed and nothing is downloaded.

**HD textures are included.** The `textures` folder holds high-resolution versions of the game's own textures
(Capcom's artwork), upscaled with AI models; they are used only together with your own disc. Delete the folder, or
set *HD textures* to Off in the Options menu, to play with the original textures. Not affiliated with or endorsed by
Capcom or Sony.

## What is in 0.1

- **Windows:** unzip, double-click `HauntingGround.exe`, choose the `.iso`, play. **Linux** (64-bit, glibc 2.34 or
  later: Ubuntu 22.04, Fedora, Debian 12, Arch, SteamOS desktop mode): unpack, run `HauntingGround.sh`, choose the
  `.iso`; needs python3 and SDL2 (installed on most desktops). The first start builds the game; later starts go
  straight to it.
- GPU renderer (Vulkan), pixel-identical to the project's reference software renderer at native resolution;
  upscaling up to x6 (x5 is about 4K); 60 fps by frame interpolation (120 at native resolution); software renderer as
  a fallback.
- **HD textures** for the stages, the characters and the menus (BC7-compressed: a quarter of the video memory of
  plain images), with an *HD textures* On/Off row in the Options menu.
- Settings inside the game's own Options menu (Down past Screen Position): renderer, resolution, frame rate, display,
  V-Sync, HD textures, keyboard and controller remapping, vibration strength, light bar colour. Saved in `hg.ini`.
- Native DualSense, DualShock 4 and Xbox controller support (USB and Bluetooth), with the game's vibration.
- Sound and cutscenes in sync: the sound processor keeps the console's own timing.
- Saving and loading on a memory card folder (`memcards/port0`), verified at the first save point.
- Every session's input is recorded, so bugs can be replayed exactly.

## Requirements

- Windows 10 or 11, 64-bit; or Linux, 64-bit (see above). A Vulkan GPU is recommended.
- Your own *Haunting Ground* disc, North American release (SLUS-21075), dumped to `.iso`. Other regions are not
  supported yet.
- About 6 GB of free disk space (most of it the HD textures and the game files copied from your disc).

## This is a test release

Tested in depth: the opening, the castle grounds, the first castle rooms and the first meeting with Debilitas, on one
PC (Ryzen 7 9800X3D, RTX 5090; Windows 11 and CachyOS Linux). We are looking for **playtesters**: how far can you get,
and what goes wrong on your hardware? Known issues: [docs/known-issues.md](https://github.com/N900x4/HauntingGround-Recomp-Project/blob/main/docs/known-issues.md).

**Reporting bugs:** open an issue with what happened, your PC (Windows version or Linux distribution, CPU, GPU), and
`logs/play.log`, `logs/launcher.log` and, if you can, the newest `recordings/.../pad.txt` from the game folder. Never
attach anything from `game`, `bin`, `textures` or `crashes`, or your disc image. Details:
[docs/release/crash-reports.md](https://github.com/N900x4/HauntingGround-Recomp-Project/blob/main/docs/release/crash-reports.md).

## Downloads

| File | For | Size | SHA-256 |
|---|---|---|---|
| `HauntingGround-Recomp-0.1-win64.zip` | Windows | 1,574.6 MB | `d761e87bdb75ce5d5f2ef989db39d8d9ae7e499ff463a66462c6569b721e77a5` |
| `HauntingGround-Recomp-0.1-linux-x64.tar.gz` | Linux | 1,633.7 MB | `74d5a91e7614ebc92dcd4d437742c2d10ea4bacf32eedd73087cec5c403bce0f` |
| `HauntingGround-Recomp-0.1-compiler-sources-win64.tar` | Source of the compiler in the Windows zip (not needed to play) | 334.6 MB | `dabe5331537fb6838f0ad90c8496c999a05e770ed60adf1eec71335c429498d4` |
| `HauntingGround-Recomp-0.1-compiler-sources-linux-x64.tar` | Source of the compiler in the Linux archive (not needed to play) | 263.1 MB | `2f01eba0da4814dc08564a3eb1c1223f4b4f4b5f68865ead09da8d2a740ae698` |

The Windows zip bundles unmodified GCC, binutils, MinGW-w64, SDL2 and Vulkan loader binaries from MSYS2 and Python
from python.org; the Linux archive a GCC toolchain from Bootlin and the SDL2 and Vulkan headers. Each keeps its own
licence (`THIRD_PARTY_NOTICES.txt` in the download); the source code of the bundled compilers (GPL and LGPL) is attached
here as the two `compiler-sources` files. The project itself is GPL-3.0; its source is this repository at
the tagged commit, and a copy of the sources the build uses is inside each download (`engine/src`). The HD textures
are not covered by the project's licence (see above).

Windows may warn that `HauntingGround.exe` is from an unknown publisher (it is not code-signed): "More info", then
"Run anyway".
