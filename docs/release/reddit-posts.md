# Reddit drafts for the 0.1 playtest call

Drafts for the user to post (nothing has been posted). The repository: https://github.com/N900x4/HauntingGround-Recomp-Project. Before
posting, check each subreddit's rules on self-promotion and on posts about recompilation and disc images (r/emulation
and r/PS2 both forbid links to game files; these posts contain none). r/HauntingGround: check that it exists and is
active; if not, r/survivalhorror is an alternative for draft 3.

---

## 1. r/emulation

**Title:** Haunting Ground (PS2) static recompilation: first test build (0.1, Windows and Linux), looking for playtesters

**Body:**

I've been working on a static recompilation of *Haunting Ground* (PS2, NTSC-U). Instead of emulating the EE, all
11,499 functions of the game's executable are translated to C and compiled into a native Windows or Linux program; a runtime
written from scratch stands in for the rest of the PS2 (VU1, GS, IOP and SPU2, IPU for the movies, pad and memory
card). No PCSX2 code is used (PCSX2 was used as a visual reference).

What 0.1 has:

- a Vulkan compute GS that matches the project's software GS bit for bit at native resolution, with upscaling up to
  x6 (x5 is about 4K) and 60 fps via frame interpolation (the game logic stays at 30);
- VU1 microprograms compiled ahead of time, with the timing proven at build time;
- the game's own sound drivers running on an emulated IOP + SPU2;
- settings added as extra rows in the game's own Options menu; native DualSense/DS4/Xbox support with vibration;
- an optional HD texture pack (the game's own textures, AI-upscaled, BC7), bundled and switchable in the Options menu;
- deterministic input recording, so any bug report can be replayed frame for frame.

**No game code is distributed.** The download is a launcher plus the sources and a bundled GCC (and Python on Windows),
plus the HD textures: you point it at your own .iso of the NTSC-U disc (SLUS-21075), it checks the image and builds the game on your PC (about
a minute on a fast machine). Nothing gets downloaded.

It's a test build: the opening, the grounds, the first castle rooms and the first Debilitas encounter are tested, and saving/loading is verified,
but most of the game hasn't been played on it, and it has only run on one PC so far (9800X3D + RTX 5090, Windows 11 and CachyOS). I'm looking
for people willing to play further and on other hardware (AMD/Intel GPUs, Windows 10, other distros, the Steam Deck, older CPUs) and to report what
breaks. Known issues and how to report: https://github.com/N900x4/HauntingGround-Recomp-Project

Happy to answer questions about how it works.

---

## 2. r/PS2

**Title:** I made a native PC version of Haunting Ground that builds from your own disc: looking for testers

**Body:**

*Haunting Ground* never got an official PC release, so I've been building a native version: the game's own program is converted to PC code
(static recompilation), and you build it on your PC from your own copy of the game. Version 0.1 is a first test
release for Windows and Linux.

- Up to 4K resolution and 60 fps (or the original 30), window or full screen
- HD textures included (can be switched off)
- DualSense, DualShock 4 and Xbox controllers, with the game's vibration; full key and button remapping
- All settings in the game's own Options menu
- Saves on a virtual memory card in the game folder

You need your own NTSC-U disc (SLUS-21075) dumped to an .iso. The download contains no game code: the launcher reads
your .iso, checks it, and builds the game (about a minute, one time).

It's early: the start of the game is tested, much of it isn't yet, and it has run on one PC only. If you have the
game and some time, I'd love reports on how far you get and what goes wrong (the game folder has logs that make bugs
easy to track down). Link and details: https://github.com/N900x4/HauntingGround-Recomp-Project

Not affiliated with Capcom or Sony.

---

## 3. r/HauntingGround (or r/survivalhorror)

**Title:** Haunting Ground PC port project: test version 0.1 is out, need playtesters

**Body:**

Hi all. I've been working on a fan-made native PC version of Haunting Ground (it's a recompilation of the original PS2
game, not a remake and not an emulator), and the first test build is ready.

What you get: the original game running natively on Windows or Linux, up to 4K, with HD textures, optionally at 60 fps, with PlayStation or
Xbox controllers (vibration included) and settings in the game's own Options menu.

What you need: your own North American copy (SLUS-21075) dumped to an .iso file. No game code is included or
downloaded; a small launcher builds the game from your disc on your PC.

What I need: people to play it! So far it's been tested through the opening, the first rooms of the castle and the first meeting with Debilitas, and
saving works, but nobody has played much further on it. If you can play through more of the game and report
anything odd (graphics, sound, crashes, controls), that would help a lot. Reporting is simple: describe what
happened and attach two log files from the game folder.

Link, instructions and known issues: https://github.com/N900x4/HauntingGround-Recomp-Project

(Fan project, not affiliated with or endorsed by Capcom.)
