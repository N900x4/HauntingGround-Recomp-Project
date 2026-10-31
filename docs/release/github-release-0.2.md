<!-- Draft of the GitHub release page for 0.2. Tag: v0.2. Title: "Haunting Ground Recomp 0.2 (test release)".
     Assets: HauntingGround-Recomp-0.2-win64.zip (tools/release/mkrelease.py --textures; needs the MSYS2 tree from the
     Windows drive, under Wine or on Windows) and HauntingGround-Recomp-0.2-linux-x64.tar.gz
     (tools/release/mkrelease_linux.py), with the compiler source archives as in 0.1. Fill in the table at the end from
     the built files (sizes, SHA-256) and the commit they were built from. Publish only on the user's word. -->

# Haunting Ground Recomp 0.2 (test release)

The second test build of the native **Windows and Linux** version of *Haunting Ground* (PS2, North American release,
SLUS-21075). As before: no game code in the download; you need your own disc dumped to a plain `.iso`, and the game
is built on your PC on the first start. Thank you to everyone who tested 0.1 and reported back!

## What is new in 0.2

- **Widescreen** (Options > Aspect ratio): the 3D view really widens, up to 21:9; the camera sees more at the sides
  instead of stretching, while menus, text and movies stay 4:3 in the middle.
- **Smoother 60 fps:** the stutters at heavy moments (Debilitas' entrance in the hallway, the chase after it) are gone.
  The renderer no longer waits for the in-between pictures, and on the test PC the hallway now runs with about 25 %
  to spare.
- **Sharper at 4K: AMD FSR 1** (Options > Upscaling): when the rendered picture is smaller than your screen, FSR
  scales it up with sharp edges instead of a soft stretch. That makes lower resolution settings look much closer to
  4K on slower GPUs.
- **The game's glow at full resolution** (Options > Soft focus): the soft glow the game lays over its scenes used to
  turn into doubled edges and lines at high resolutions (outdoors especially, on Fiona). It is now drawn at the
  upscaled resolution: the same soft look, without the doubling. "Original" keeps it as the game draws it.
- **Faster at high resolutions:** the GPU renderer's memory layout and shaders were reworked: x6 costs about a third
  less GPU time.
- **HD text:** the game's own letters, redrawn cleanly at high resolution from the original glyphs (same shapes,
  spacing and soft edge as on the PS2), for all text colours.
- **Keyboard prompts:** playing on the keyboard, the PlayStation button symbols in the game's text (menus, tutorials)
  become keycaps showing *your* keys, and follow your bindings. The Controller page lists the keyboard key for each
  button. Use a controller and the PlayStation symbols come back.
- **If the GPU stops** (a driver reset), the game now carries on with the software renderer instead of a black screen.
- Fixes: the see-through bar behind the examine text in widescreen; a hitch at every start before the Capcom logo;
  the Linux release build's compiler made the 60 fps pictures slow on AMD CPUs.
- Security hardening: replays never follow links in a recording's folders, damaged `.dds` textures and disc images
  are rejected cleanly, and the Windows launcher's buffers are checked.

## Suggested settings

The renderer reproduces the PS2's graphics chip exactly for every sample, so its cost grows with the square of the
resolution factor. For 60 fps on a 4K display: fast GPUs (RTX 4080/5080 class and up) x4 to x6; mid-range (RTX 3060
Ti to 4070 class) x2 to x3 with Upscaling on FSR; older or integrated GPUs native or x2. If the F3 overlay shows less
than 100 % or SKIP in heavy scenes, pick the next lower resolution (these are estimates from one RTX 5090: please tell
us how your GPU does).

## Requirements, reporting bugs

As in 0.1: Windows 10 or 11 (64-bit) or 64-bit Linux, a Vulkan GPU recommended, your own SLUS-21075 disc as `.iso`,
about 6 GB of disk space. To report a bug, open an issue with `logs/play.log`, `logs/launcher.log` and, if you can,
the newest `recordings/<date>/` folder's `pad.txt` **and its `card` folder** (needed to replay a session that loaded
a save). Press **F9** in the game at the moment something looks wrong: it marks that moment in the recording. Never
attach anything from `game`, `bin`, `textures` or `crashes`, or your disc image.

## Downloads

| File | For | Size | SHA-256 |
|---|---|---|---|
| `HauntingGround-Recomp-0.2-win64.zip` | Windows | | |
| `HauntingGround-Recomp-0.2-linux-x64.tar.gz` | Linux | | |
| `HauntingGround-Recomp-0.2-compiler-sources-win64.tar` | Source of the compiler in the Windows zip (not needed to play) | | |
| `HauntingGround-Recomp-0.2-compiler-sources-linux-x64.tar` | Source of the compiler in the Linux archive (not needed to play) | | |

Third-party licences: `THIRD_PARTY_NOTICES.txt` in the download (new in 0.2: AMD FidelityFX Super Resolution 1, MIT).
