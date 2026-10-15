# Phase 1 runtime notes: controller, memory card, movies, 2D graphics

Date 2026-10-12. Addresses are SLUS_210.75 (NTSC-U). Protocol details come from the game's own EE libraries, decompiled with REA (Ghidra provider, R5900).

## Result

Headless runs (scripted input, frames written to files) now go through the whole front end:

boot notice → memory-card check → CRI/ADX logo → Dolby logo → violence notice → `CAPCOM.SFD` (skipped) → title animation → title screen ("PRESS START BUTTON") → main menu (NEW GAME / LOAD GAME / OPTIONS) → New Game → `OPENING.SFD` (skipped) → stage 1 loads (`O_CRW/CRW_000.TEX`, `BGM/STAGE1_BANK`, `S1_NORMALA/B.SQ`, `PANIC.SQ`, `S1_CHASE.SQ`).

Stage 1 shows only its clear colour: 3D geometry is drawn by VU1 microprograms, which are not executed yet (phase 2).

```sh
cd recomp
export HG_GAME_DIR=/path/with/SLUS_210.75+DATA.CVM     # or put the files in ../game
mkdir -p /tmp/frames
HG_FRAMES=/tmp/frames HG_FRAME_EVERY=60 \
HG_PAD=600:cross,900:cross,1200:cross,1500:cross,2300:start,2700:cross,2900:cross \
./hgboot -q -t 75 > run.log
```

## Run-time switches

| Variable | Meaning |
|---|---|
| `HG_GAME_DIR` | Folder with `SLUS_210.75` and `DATA.CVM` (default `../game`) |
| `HG_PAD` | Scripted input: `vblank[:-end]:button[+button]`, comma-separated. A single VBlank holds for 6 VBlanks. Buttons: select l3 r3 start up right down left l2 r2 l1 r1 triangle circle cross square |
| `HG_MEMCARD_DIR` | Memory-card root; port 0 = `<dir>/port0` (default `../memcards`, git-ignored). Port 1 is empty |
| `HG_MOVIES` | `skip` skips movies (they play by default since the IPU is emulated; see movies-ipu.md) |
| `HG_FRAMES`, `HG_FRAME_EVERY` | Write the displayed frame as PPM every N VBlanks (default 60) |
| `HG_TRACE`, `HG_PEEK`, `HG_DUMPSID` | Debug aids (function-entry trace, memory peek at stop, RPC send-buffer dump) |

## Controller: DBCMAN / libdbc / libpad2 (`recomp/iop_pad_mc.c`)

- libdbc 0x1ee740–0x1ef968 binds sid 0x80001300, then 0x8000131c, 0x8000131e and 0x8000131f. Function numbers are 0x800013xx. Send and receive buffer: 0x4822c0 (0x90 bytes).
  - 0x80001363 module version: libdbc requires `(v >> 4) == 0x31`. The HLE answers 0x310.
  - 0x80001304 SetWorkAddr(0x4845c0): per-socket "connected" words. libpad2 copies them and requires word[socket] == 1.
  - 0x80001301 CreateSocket: buffer at send+0x28. The socket number is returned at recv+0x24.
  - 0x80001303 GetDepNumber, 0x80001317 GetDeviceStatus, 0x8000131a SRData (data at recv+0xc, result at recv+0x8c).
- The libpad2 socket buffer is 2 × 0x80 bytes. The newer half is the one with the larger counter at +0x7c. Layout: +0 state (1 = stable), +2 data length, +3 profile length, +4 word ≠ 0 = valid, +0x1c data, then the profile.
- The game's pad class (vtable 0x46adb0) reads the buttons as `~(data[0] | data[1] << 8) & profile[0..1]` and 16 analog bytes from data[2..17] (rx, ry, lx, ly, then pressures). The remap tables at 0x47e3c0/0x47e3d0 are identity, so the bits are in classic DualShock 2 order. The boot notice waits for Cross (bit 14 of 0x47e37c). Start is bit 3.

## Memory card: MCSERV / libmc (`recomp/iop_pad_mc.c`)

- libmc 0x110938–0x1121xx, sid 0x80000400, "XMC" command numbers. Descriptor A (0x30 bytes) is at 0x47bd80, descriptor B (0x414 bytes, with a name) at 0x47bdb0. The result word is at 0x47d2c0 and the end parameter block at 0x47c200.
- Init (0xfe) requires MCSERV ≥ 0x20a in recv[1] and MCMAN > 0x20d in recv[2]. Otherwise libmc disables itself, which was the old boot stop #12.
- GetInfo end callback 0x111450: `*type = end[0]`, `*free = end[1]`, `*format = end[0x24]` (byte offset 0x90). Read end callback 0x111048 copies unaligned head/tail bytes. The HLE writes all data directly and reports zero head/tail.
- Implemented: GetInfo, Open/Mkdir (mode 0x40), Close, Seek, Read, Write, Flush, ChDir, GetDir (wildcards, `.`/`..` inside directories), SetFileInfo (rename only), Delete, Format/Unformat, GetEntSpace.
- [OBS] Saving works (2026-10-15, `tools/verify/savetest.sh`): at the Castle Suite clock the game saves into a new slot, overwrites a slot, and on an empty card creates the directory, `icon.sys`, `icon00.ico` (both byte-identical to the files PCSX2 wrote) and the 77,168-byte save file. A slot saved right after loading save 02 differs from save 02 in 9 bytes only (offsets 0x0-0x1, 0x894-0x897 play time, 0x100d-0x100f); loading any of these saves gives frames identical to loading save 02. The card file is the game's: a 0x170-byte header (slot table) and 12 slots of 0x1900 bytes from 0x170.
- The boot memory-card step is 0x2bf2f0 (state machine on the mc object `*(0x44ff00)`): it lists `/BASLUS-21075HG/*` and checksums the save header.

## Movies (`recomp/overrides.c`)

- Every SFD goes through a CRI MWPLY handle (static handle 0x3e893c, function table 0x3e8758): +0x18 StartFname 0x23b618, +0x1c Stop 0x23b8e8, +0x20 GetStat 0x23c9a8 (3 = play end).
- The decoder (0x2a2xxx) drives the IPU registers directly. Without an IPU it parses forever inside one frame.
- With the skip on, StartFname starts nothing and GetStat reports "play end". The game's movie object (vtable 0x46ea60, update 0x2b6510) then destroys itself and clears `*(0x44e958)`, which is what the boot step 0x37f7d0 waits for. Start also skips the logo movie in the real game.
- Superseded (2026-10-13): the IPU is now emulated and movies play through the game's own player; this skip is only used with `HG_MOVIES=skip`. See [movies-ipu.md](movies-ipu.md).

## Graphics (`recomp/gs.c`, `recomp/dmac.c`)

- DMA channels 1 (VIF1) and 2 (GIF) run synchronously when CHCR.STR is set. They support normal and source-chain modes (refe/cnt/next/ref/refs/call/ret/end, TTE for VIF1). Completion interrupts are still delivered at the next VBlank.
- The VIF1 stream is parsed fully. DIRECT/DIRECTHL go to the GIF. UNPACK/MPG/MSCAL are counted and skipped. In the 75 s front-end run all VIF1 traffic was DIRECT (27 M qwords: the full-screen image uploads), with 0 MSCAL.
- GS: swizzled local memory for CT32/24/16/16S, T8/T4/8H/4HL/4HH and Z formats. The T8/T4 column tables are checked for bijectivity at start-up; exact hardware order is [INF]. It only matters when a texture is read in a different format than it was written, which the front end never does.
- The rasterizer is float barycentric, with top-left handling only approximate. Bilinear filtering follows TEX1.MMAG. Dithering, mipmapping, AA1 and SIGNAL/LABEL are not modelled.

## Open items

1. Window and live input (`recomp/host_sdl.c`, SDL2) added 2026-10-12. Window output verified by screenshot; live keyboard/gamepad input not yet tried by a person.
2. Audio: SNDDRV and CRI ADX still get stub RPC replies, so there is no sound.
3. VU1 microprograms and UNPACK into VU1 memory (phase 2, 3D).
4. Save-game write path (memory card) is untested.
