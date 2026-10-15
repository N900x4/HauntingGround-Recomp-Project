# Known issues (release 0.1)

What is known not to work, not to be finished, or not to be tested yet. Reports about anything here are still
welcome, especially with a recording (see the README, "Reporting bugs").

## How far it has been tested

- **Game coverage.** Tested: the logos, movies, title and menus, New Game and the opening cinematic, the castle grounds
  (`ST_02A`, `ST_013`) and a room beyond (`ST_00D`), LOAD GAME into the Castle Suite, and a 6-minute recorded session
  from there through the next rooms (`ST_010`, `ST_020`). Most of the game, its later chapters, bosses and endings
  have **not** been played on this build. A wrong picture, a missing sound or a stop is possible anywhere beyond.
- **Saving** is verified at the Castle Suite clock (a new slot, an empty memory card, overwriting a slot; reloading
  gives exactly the saved state). The route from New Game to the first clock has not been played through on this build
  by a script; it has only been checked in parts.
- **Hardware.** 0.1 has been tested on one PC: Windows 11, AMD Ryzen 7 9800X3D, NVIDIA GeForce RTX 5090 (plus the
  Ryzen's integrated AMD graphics, seen by the launcher but not used). Untested: AMD and Intel graphics cards as the
  game's GPU, Windows 10, laptops, PCs with fewer than 8 CPU threads, high-DPI and multi-monitor setups beyond the
  basics.
- **Controllers.** A DualSense over USB was detected and drove vibration and the light bar in automated tests; the
  checklist with the controller in hand (feel, Bluetooth, the touchpad) has not been done yet. DualShock 4 and Xbox
  controllers are supported through SDL but were not tried on real hardware.
- **Linux and macOS.** There is no release for them. The source still builds on Linux for developers (experimental,
  not re-checked since 2026-10-13); see `docs/building.md`.

## Graphics

- **Specks by a door** in one castle room (stage `ST_020`): a few dark pixels drift near a doorway. PCSX2 shows some
  isolated dark pixels in the Castle Suite too, so this may be the original's behaviour; it has not been compared in
  that room yet.
- **60 fps is frame interpolation.** The game itself runs at 30 frames per second; the in-between pictures are made
  by moving each triangle halfway between two real frames. Fast camera moves, objects appearing or vanishing, and
  repeated textures (floor tiles) can show small glitches in the in-between pictures. After a camera cut the real
  picture is shown twice instead. 60 and 120 fps add about 17 ms of input delay.
- **Speed limits at high settings** (measured on an RTX 5090): x5 (about 4K) with 60 fps runs at full speed; x6 with 60
  fps (77 %) and x5 with 120 fps (55 %) do not. 120 fps is offered at native resolution only. Slower GPUs will reach
  full speed at lower factors; pick a lower resolution if the F3 overlay shows less than 100 %.
- **Upscaling renders more pixels, not new textures.** Textures, effects and 2D art stay at the original resolution.
- **The picture is paced to 59.94 Hz**, the PS2's rate, not to the display: on a 60 Hz screen one picture is repeated
  about every 17 seconds; variable refresh rate (G-Sync/FreeSync) is not used.

## Settings and controls

- Changing the **renderer** (GPU / software) takes effect at the next start; the other settings change at once.
- The **analog sticks cannot be remapped** (a controller's sticks stay sticks; the keyboard's left stick is four keys).
- Key names are SDL's English names; the game's font has no symbols for the D-pad or keyboard keys.
- **Steam Input**: if Steam's "PlayStation Controller Support" takes over the controller, the game sees an Xbox pad
  (Xbox names, no light bar). See `docs/controller.md`, "Steam Input".
- Adaptive triggers, gyro, the controller speaker and microphone are not used. Only player one plays.

## Sound

- Sound comes from the game's own sound drivers on an emulated sound processor and is checked against independent
  decoders, but room reverb has not been seen switched on in any tested room, so reverb is unverified.

## The launcher and the first start

- The folder path must contain only plain English letters, digits and spaces (the bundled compiler cannot work in
  other paths); the launcher says so and asks you to move the folder.
- Only the North American disc (SLUS-21075, v1.01) is supported. Other regions, revisions, patched or translated
  images and compressed images (.chd, .cso, .zip, .7z) are refused with a message.
- The executables are not code-signed: Windows SmartScreen and some antivirus programs may warn about
  `HauntingGround.exe` or about the program it compiles (`bin\hgboot.exe`). An antivirus scanning every file the
  compiler writes can make the first start much slower.
- The first start needs about 2 GB of disk space and takes 1.5 minutes on a fast PC (more on slower ones: the
  compilation uses every CPU core).

## Not in 0.1

Texture replacement and higher-resolution textures, Hewie's voice replacement, mod loading, effect passes rewritten as
native 4K shaders, a lock to the display's refresh rate, and the Linux release are planned for later versions
(`docs/roadmap-native-port.md`).
