<!-- Draft of the GitHub release page for 0.1. Tag: v0.1. Title: "Haunting Ground Recomp 0.1 (test release)".
     Asset: HauntingGround-Recomp-0.1-win64.zip (made with tools/release/mkrelease.py; 36.5 MB; put its SHA-256 below).
     Fill in <REPO URL> and the checksum before publishing. -->

# Haunting Ground Recomp 0.1 (test release)

The first public test build of a native Windows version of *Haunting Ground* (PS2, North American release,
SLUS-21075). The game's program is statically recompiled to C and compiled on your PC; a hand-written runtime stands
in for the PS2 hardware. It is not an emulator and it does not use PCSX2 code.

**No game code or data is in this download.** You need your own copy of the game, dumped to a plain `.iso`. The
launcher checks your disc image, copies the files the game needs, and builds the game on your PC with the compiler
included in the zip. Nothing needs to be installed and nothing is downloaded. Not affiliated with or endorsed by
Capcom or Sony.

## What is in 0.1

- Builds the game from your own NTSC-U disc image: double-click `HauntingGround.exe`, choose the `.iso`, wait about
  1.5 minutes (longer on slower PCs), play. Later starts go straight to the game.
- GPU renderer (Vulkan), pixel-identical to the project's reference software renderer at native resolution;
  upscaling up to x6 (x5 is about 4K); 60 fps by frame interpolation (120 at native resolution); software renderer as a fallback.
- Settings inside the game's own Options menu (Down past Screen Position): renderer, resolution, frame rate, display,
  V-Sync, keyboard and controller remapping, vibration strength, light bar colour. Saved in `hg.ini`.
- Native DualSense, DualShock 4 and Xbox controller support (USB and Bluetooth), with the game's vibration.
- Saving and loading on a memory card folder (`memcards\port0`), verified at the first save point.
- Every session's input is recorded, so bugs can be replayed exactly.

## Requirements

- Windows 10 or 11, 64-bit; a Vulkan GPU recommended.
- Your own *Haunting Ground* disc, North American release (SLUS-21075), dumped to `.iso`. Other regions are not
  supported yet.
- About 2 GB of free disk space.

## This is a test release

Only the opening, the castle grounds and the first castle rooms have been tested in depth, on one PC (Ryzen 7 9800X3D,
RTX 5090, Windows 11). We are looking for **playtesters**: how far can you get, and what goes wrong on your hardware?
Known issues: [docs/known-issues.md](<REPO URL>/blob/main/docs/known-issues.md).

**Reporting bugs:** open an issue with what happened, your PC (Windows version, CPU, GPU), and `logs\play.log`,
`logs\launcher.log` and, if you can, the newest `recordings\...\pad.txt` from the game folder. Never attach anything
from `game\`, `bin\` or `crashes\`, or your disc image. Details:
[docs/release/crash-reports.md](<REPO URL>/blob/main/docs/release/crash-reports.md).

## Download

`HauntingGround-Recomp-0.1-win64.zip` (36.5 MB). SHA-256: `<SHA-256 of the zip>`.

The zip bundles unmodified GCC, binutils, MinGW-w64, SDL2 and Vulkan loader binaries from MSYS2 and Python 3.14 from
python.org, under their own licences (`THIRD_PARTY_NOTICES.txt` in the zip). The project itself is GPL-3.0; its source
is this repository at the tagged commit, and a copy of the sources the build uses is inside the zip (`engine\src`).

Windows may warn that `HauntingGround.exe` is from an unknown publisher (it is not code-signed): "More info", then
"Run anyway".
