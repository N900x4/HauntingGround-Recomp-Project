<!-- Draft of the GitHub release page for 0.2 (the version number is the user's to confirm). Tag: v0.2.
     Title: "Haunting Ground Recomp 0.2 (test release: Linux, HD textures)".
     Assets: HauntingGround-Recomp-0.2-win64.zip (tools/release/mkrelease.py; not built yet: needs the MSYS2 tree on the
     Windows drive) and HauntingGround-Recomp-0.2-linux-x64.tar.gz (tools/release/mkrelease_linux.py; 1,633.7 MB on
     2026-10-26, SHA-256 below; rebuild after the last commit). Both under GitHub's 2 GB per file.
     Fill in <REPO URL> and the checksums before publishing. -->

# Haunting Ground Recomp 0.2 (test release)

A native **Windows and Linux** version of *Haunting Ground* (PS2, North American release, SLUS-21075). The game's
program is statically recompiled to C and compiled on your PC from your own disc; a hand-written runtime stands in for
the PS2 hardware. It is not an emulator and it does not use PCSX2 code.

**No game code is in this download.** You need your own copy of the game, dumped to a plain `.iso`. The launcher checks
your disc image, copies the files the game needs, and builds the game on your PC with the compiler included in the
download (about a minute on a fast PC). Nothing needs to be installed and nothing is downloaded.

**HD textures are included.** The `textures` folder holds high-resolution versions of the game's own textures
(Capcom's artwork), upscaled with AI models; they are used only together with your own disc. Delete the folder, or
set *HD textures* to Off in the Options menu, to play with the original textures. Not affiliated with or endorsed by
Capcom or Sony.

## What is new in 0.2

- **Linux release** (64-bit, glibc 2.34 or later: Ubuntu 22.04, Fedora, Debian 12, Arch, SteamOS desktop mode): unpack,
  run `HauntingGround.sh`, choose the `.iso`. Needs python3 and SDL2 (installed on most desktops).
- **HD textures** for the stages, the characters and the menus (BC7-compressed: a quarter of the video memory of
  plain images), with an *HD textures* On/Off row in the Options menu.
- **Smoother 60 fps**: the IOP (sound and streaming processor) now keeps the console's timing, which removes the
  frames the game used to hold for 50 ms, and keeps cutscene sound in sync with the picture (it used to drift ahead).
- **Fewer stutters**: heavy scenes no longer make the GPU renderer wait on palette loads; when the renderer still
  cannot keep up, the in-between pictures pause for a few seconds instead of the whole game slowing down. The sound no
  longer runs late after a hiccup.
- The loading screen's spinning globe no longer shows shards in the in-between pictures.

## Requirements

- Windows 10 or 11, 64-bit; or Linux, 64-bit (see above). A Vulkan GPU is recommended.
- Your own *Haunting Ground* disc, North American release (SLUS-21075), dumped to `.iso`. Other regions are not
  supported yet.
- About 6 GB of free disk space (most of it the HD textures and the game files copied from your disc).

## This is a test release

Tested in depth: the opening, the castle grounds, the first castle rooms and the first meeting with Debilitas, on one
PC (Ryzen 7 9800X3D, RTX 5090; Windows 11 and CachyOS Linux). We are looking for **playtesters**: how far can you get,
and what goes wrong on your hardware? Known issues: [docs/known-issues.md](<REPO URL>/blob/main/docs/known-issues.md).

**Reporting bugs:** open an issue with what happened, your PC (Windows version or Linux distribution, CPU, GPU), and
`logs/play.log`, `logs/launcher.log` and, if you can, the newest `recordings/.../pad.txt` from the game folder. Never
attach anything from `game`, `bin`, `textures` or `crashes`, or your disc image. Details:
[docs/release/crash-reports.md](<REPO URL>/blob/main/docs/release/crash-reports.md).

## Downloads

| File | For | Size | SHA-256 |
|---|---|---|---|
| `HauntingGround-Recomp-0.2-win64.zip` | Windows | `<size>` | `<SHA-256>` |
| `HauntingGround-Recomp-0.2-linux-x64.tar.gz` | Linux | 1.6 GB | `<SHA-256>` |

The Windows zip bundles unmodified GCC, binutils, MinGW-w64, SDL2 and Vulkan loader binaries from MSYS2 and Python
from python.org; the Linux archive a GCC toolchain from Bootlin and the SDL2 and Vulkan headers. Each keeps its own
licence (`THIRD_PARTY_NOTICES.txt` in the download). The project itself is GPL-3.0; its source is this repository at
the tagged commit, and a copy of the sources the build uses is inside each download (`engine/src`). The HD textures
are not covered by the project's licence (see above).

Windows may warn that `HauntingGround.exe` is from an unknown publisher (it is not code-signed): "More info", then
"Run anyway".
