# Haunting Ground Recomp

A native Windows version of *Haunting Ground* (PlayStation 2, North American release, SLUS-21075), built on your PC
from **your own copy of the game**. It is not an emulator: the game's program is translated into C and compiled into
a Windows program, with a hand-written runtime standing in for the PS2 hardware. It runs at up to 4K, at 60 fps
(frame interpolation), with native PlayStation and Xbox controller support.

> **Version 0.1 is an early test release.** The opening, the castle grounds and the first rooms of the castle have
> been tested, and saving and loading work, but most of the game has not been played through on it yet. Expect bugs; reports are
> very welcome (see [Reporting bugs](#reporting-bugs)).

**Not affiliated with or endorsed by Capcom or Sony.** No game code or game data is included in any download. You
need your own legally obtained copy of the game, dumped to a disc image (.iso) yourself.

## What you need

- Windows 10 or 11, 64-bit.
- A graphics card with Vulkan (any NVIDIA, AMD or Intel GPU from roughly the last ten years) is recommended. Without
  Vulkan the game uses its software renderer, which is exact but slower and has no upscaling or 60 fps.
- A disc image of the **North American** disc, *Haunting Ground*, **SLUS-21075**, as a plain `.iso` file that you
  made from your own disc (for example with ImgBurn or any DVD dumping tool). European and Japanese (Demento) discs
  are not supported in 0.1. Compressed images (.chd, .cso, .zip, .7z) must be turned back into a plain .iso first.
- About 2 GB of free disk space and a few minutes for the first start.

Nothing has to be installed: the download includes the compiler and Python it needs to build the game. Nothing is
downloaded while it runs.

## How to start

1. Unzip `HauntingGround-Recomp-0.1-win64.zip` into a folder of your choice, for example `C:\Games\HauntingGround`.
   Use a folder whose path has only plain English letters, digits and spaces, and that you can write to (not
   `C:\Program Files`).
2. Double-click **`HauntingGround.exe`**. Windows may warn that the program is from an unknown publisher (it is not
   code-signed): choose "More info", then "Run anyway".
3. Click **Choose disc image...** and pick your `.iso`. The launcher checks that it is the right disc and that it is
   undamaged, copies the 1.6 GB the game needs into the `game` folder, translates and compiles the game (about 1.5
   minutes on a fast PC, several minutes on a slower one), and starts it. Your disc image is only read, never changed.
4. From then on, double-clicking `HauntingGround.exe` starts the game directly.

If you unzip a newer release over this folder, the next start builds the game again without asking for the disc.
Start `HauntingGround.exe --setup` (for example from a shortcut) to rebuild it yourself.

## Playing

| PS2 | Keyboard (default) | Controller |
|---|---|---|
| D-pad | arrow keys | D-pad |
| Left stick | I J K L | left stick (the right stick works too) |
| Cross / Circle / Square / Triangle | X / C / Z / S | the same buttons (PlayStation) or A / B / X / Y (Xbox) |
| Start / Select | Enter / Right Shift | Options / Create (or touchpad click) on PlayStation, Menu / View on Xbox |
| L1 / R1 / L2 / R2 | Q / E / 1 / 3 | the shoulder buttons and triggers |

**F11** switches between window and full screen, **F3** shows the speed overlay, **Esc** quits.

DualSense, DualShock 4 and Xbox controllers work over USB or Bluetooth. The game's vibration reaches the controller,
and a DualSense's light bar glows dim amber (both can be changed). The first controller to press a button is player
one. If Steam is running with "PlayStation Controller Support" on and the controller acts like an Xbox pad, see
[docs/controller.md](docs/controller.md), "Steam Input".

## Settings

In the game: **Options**, then press Down past *Screen Position* to reach the extra rows:

- **Renderer**: GPU (Vulkan) or Software. Takes effect at the next start. The launcher picks GPU when your PC has a
  working Vulkan graphics card.
- **Resolution**: native (640x448), x2 to x6, or auto (the default: fills your screen, at most x4; x4 is 2560x1792,
  stretched to a 4K screen's height; x5 is about 4K). Changes at once.
- **Frame rate**: 30 (the original), 60 (the default with the GPU renderer) or 120 (120 at native resolution only).
  60 and 120 add about 17 ms of delay.
- **Display**: window or full screen; **V-Sync**: On (smooth, the default: locked to your display, every picture stays
  up equally long on 60/120/240 Hz screens), Exact 59.94 Hz (the PS2's own rate), or Off (may tear).
- **Keyboard** and **Controller**: remap every button (press the key or button you want). The controller page also
  sets the vibration strength and the light bar colour.

Everything is saved in `hg.ini` in the game folder; you can also edit it with Notepad while the game is closed.
Delete it to return to the defaults.

## Where things are

Everything stays in the folder you unzipped:

| Folder or file | What it holds |
|---|---|
| `memcards\port0\` | **Your saves** (the memory card). Back this folder up to keep your progress, and copy it into a new release's folder to carry your saves over. |
| `hg.ini` | Your settings |
| `logs\` | `launcher.log` (the first start and every start), `play.log` (the last session), `play.prev.log` (the one before) |
| `recordings\` | The controller input of your last 20 sessions, so a bug can be replayed exactly |
| `crashes\` | Created only if the game crashes |
| `game\`, `bin\` | Files built from your disc: the game data and the compiled game. **Never share these.** |
| `engine\` | The sources, the compiler and Python used for the build |

## Known issues

The full list is in [docs/known-issues.md](docs/known-issues.md). The main ones:

- Only the opening areas and the first rooms of the castle have been tested in depth; later chapters may have
  graphics or sound problems, or stop.
- 0.1 has been tested on one PC (Windows 11, AMD Ryzen, NVIDIA RTX). Other GPUs (AMD, Intel), Windows 10 and
  slower PCs have not been tried yet.
- 4K together with 60 fps needs a strong GPU; x6 with 60 fps and x5 with 120 fps do not reach full speed even on an RTX
  5090.
- 60 fps is made by frame interpolation: fast movement can show small glitches in the in-between pictures.
- Changing the renderer needs a restart; the analog sticks cannot be remapped.
- A few dark specks flicker near a door in one room of the castle; it is not yet known whether the original does the same.

## Reporting bugs

Please open an issue on the GitHub page (or reply to the Reddit post) with:

1. what happened, and what you did just before (the room, what Fiona was doing);
2. your PC: Windows version, CPU, graphics card;
3. these files from the game folder: **`logs\play.log`** (if the game stopped, the reason and, after a crash, a crash
   report are at its end), **`logs\launcher.log`** (for problems with the first start), and, if you can, the latest
   folder in **`recordings\`** without its `card` and `replay_card` folders (only `pad.txt`: your button presses, so
   the bug can be replayed exactly).

Do not attach anything from `game\`, `bin\` or `crashes\` (the `.dmp` files there hold parts of the game's memory), and
do not attach your disc image. The log files contain no game data, but they do contain folder paths, which may include
your Windows user name. More detail: [docs/release/crash-reports.md](docs/release/crash-reports.md).

## For developers

Building from source, the recompiler, the runtime and the verification tools: [docs/building.md](docs/building.md),
[docs/handoff.md](docs/handoff.md) and the other notes in [docs/](docs/). How the release and its launcher are made:
[tools/release/README.md](tools/release/README.md).

## Legal

- This project contains **no** Capcom or Sony code, assets or BIOS files, and none may be contributed. No PS2 BIOS is
  needed.
- You need your own legally obtained copy of the game. PS2 discs are not encrypted; the tool circumvents no copy
  protection.
- Everything built from your disc (the `game` and `bin` folders, generated code) is derived from your copy and stays
  on your PC. Never upload or share it.
- *Haunting Ground* is a trademark of Capcom. This project is not affiliated with or endorsed by Capcom or Sony.

## Credits

- [PCSX2](https://github.com/PCSX2/pcsx2) was the visual reference for PS2 behaviour; no PCSX2 code is used. This is
  not a PCSX2 project; please don't report its bugs to them.
- Static recompilation projects that showed the way: N64Recomp/Zelda64Recomp, XenonRecomp/UnleashedRecomp,
  PS2Recomp, and DQ8-Recompiled.
- The release bundles GCC, binutils, MinGW-w64, SDL2, the Vulkan loader and Python; see `THIRD_PARTY_NOTICES.txt`.

## Licence

GPL-3.0 (see [LICENSE](LICENSE)). The bundled third-party components keep their own licences
(`THIRD_PARTY_NOTICES.txt`, `engine\licenses\`).
