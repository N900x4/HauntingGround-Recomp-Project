# Bug and crash reports: what to attach (release 0.1)

For players, the short version is in the README ("Reporting bugs"). This page says what each file holds, why it is
safe or not to share, and how a developer uses it.

## What to attach

| File (in the game folder) | When | What it holds | Safe to share publicly |
|---|---|---|---|
| `logs\play.log` | always | The last session's log: settings, controller and GPU names, module loading, timing; at the end the reason the game stopped (`STOP: ...`), and after a crash the crash report (`==== CRASH`: the exception, the game function and VBlank, the stage and Fiona's position, and the call chain with function addresses) | yes (see below) |
| `logs\play.prev.log` | if the problem was in the session before | the same, one session older | yes |
| `logs\launcher.log` | problems with the first start or with starting at all | every launcher start; the disc check (the files' names, sizes and "sha256 ok"), the compiler's messages, the Vulkan devices found, how the game ended | yes |
| `recordings\<date-time>\pad.txt` | to make a bug replayable | the settings in effect and every change of the controller state with its VBlank; nothing else | yes |
| `recordings\<date-time>\card\port0\BASLUS-21075HG\BASLUS-21075HG` | when the session loaded a save (its replay needs it: without it the replay goes its own way from the first load) | your save data as it was when the session started | yes (it is your save, written by the game) |
| `hg.ini` | settings problems | your settings | yes |

## What not to attach

| File | Why |
|---|---|
| `crashes\*.dmp` | A Windows minidump. It includes the program's data segments, which hold the emulated sound processor's memory with the game's own sound modules, and parts of the game's memory: game data. The crash report in `play.log` has what is needed. A developer may ask for a dump privately; never post one. |
| `recordings\...\card\...\icon.sys`, `icon00.ico` | The game's save icon (Capcom's artwork), not your data. |
| anything in `game\` or `bin\` | Built from your disc: the game's program and data, and the compiled game. |
| your disc image | Obviously. |

## Checked for 0.1

- `play.log` was read through for a full session and a crash test: it holds technical lines only (module names and
  version banners the sound drivers print, such as "CRI ADX Driver Ver.9.34", RPC and DMA counters, file offsets inside
  `DATA.CVM`, addresses of the game's functions). No game text, pictures, sound or code is written to it.
- `launcher.log` holds the disc image's path, the names, sizes and check results of the copied files, compiler warnings
  about our own runtime sources, and the GPU names. No file contents.
- `pad.txt` holds only button states, stick positions and settings.
- Press **F9** in the game at the moment something looks wrong: the log notes that moment ("mark 1 ... VBlank ..."), so it can be found in the replay.
- Privacy: the logs contain folder paths (the disc image's path, the game folder), which can include the Windows user
  name. Players can replace it before posting.

## How a developer uses them

- `play.log`'s `==== CRASH` block names the guest function (`0x...`) and the host call chain; the release builds the
  same program as the source of that release (`engine\src\COMMIT.txt` gives the commit), so the addresses match a
  developer build of that commit from the same disc.
- A recording replays exactly: `HG_PADPLAY=<folder> hgboot.exe` with a build of the same commit (the recording's
  `card` folder, or an empty one for a session that began with New Game, is copied to `replay_card` first). The
  launcher keeps the last 20 recordings.
- `launcher.log` covers the first start: which step failed (disc check, translation, compilation) and the messages.
