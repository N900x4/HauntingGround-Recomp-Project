# Phase 2 audio: emulated IOP + SPU2

Date 2026-10-12. Addresses are SLUS_210.75 (NTSC-U) unless an IRX is named.

## Approach

The game's own IOP sound modules run unmodified on an emulated IOP; only the IOP kernel is implemented on the host.

| Layer | Implementation |
|---|---|
| `SNDDRV.IRX` (Capcom Sound Driver 3.11, T. Masuda), `MODHSYN`, `MODMIDI`, `MODMSIN`, `CRI_ADXI.IRX` (CRI ADX driver 9.34), `LIBSD.IRX` | Real code from the disc (`MODULES/`, extracted by `tools/ingest.py`) |
| IOP CPU (R3000A), IRX loader/linker | `recomp/iop_cpu.c`, `recomp/iop_kernel.c` |
| IOP kernel libraries (sysmem, loadcore, intrman, thbase, thsemap, thevent, timrman, sifcmd, sifman, sysclib, stdio, ioman, cdvdman, vblank) | Host functions in `iop_kernel.c`. Function numbers come from ps2sdk's export tables |
| SPU2 | `recomp/spu2.c`: register-level, from the psx-spx SPU documentation plus the SPU2 register map |
| Output | SDL audio (windowed) or `HG_WAV=file` (headless) |

The IOP runs in lock-step with the EE VBlank: 1/59.94 s of IOP time per VBlank, with event-driven idle (thread delays, hardware timers, auto-DMA drain).

## Findings (REA/Ghidra on the ELF and on the IRX modules)

- **Module arguments.** The LOADFILE request is `{arg_len, modres, path[252], args[252]}` (ps2sdk `_lf_module_load_arg`).
  - SNDDRV requires `argc == 11`. The game passes `0xFFFFFFFF 0x1FFFFF 0x0 4167 27 27 27 27 23 1`.
  - CRI gets `thpri=39 spucore=0 sdinit debug=0`.
- **IOP heap.** `sceSifAllocIopHeap` (sid 0x80000003, fn 1) must return real IOP memory. The old stub returned 1, and the EE then copied sound data over address 0, which overwrote the modules.
- **SNDDRV** binds an EE-side RPC server (IOP as client). That needs IOP→EE bind/call, with END packets returning through the game's EE libsif. Its sequencer tick is a 4167 µs hardware timer (240 Hz).
- **Interrupt enable is per thread.** It is part of the R3000 status register in each thread context. A global flag left interrupts off after a thread blocked inside `CpuSuspendIntr`. Handlers also run with the state restored on exit.
- **RPC server threads** must remember their queue separately from their current wait object. A handler that blocks on a semaphore must not lose the queue.
- **CRI ADX streaming (DTX/SJX/SJRMT).**
  - The EE and IOP exchange 0x880-byte DTX blocks every frame. The last 0x40 bytes are a trailer with a sequence counter at +0x3c. The EE only sends again after the counter echoed back is greater (`FUN_001da7f0`).
  - Block payload: word 0 = count, then 16-byte entries `{flags, stream-joint, IOP address, length}`.
    - EE→IOP entries carry new data chunks. The IOP receive callback (CRI_ADXI +0x7af0) puts them into the IOP stream joint.
    - IOP→EE entries return consumed space (send callback +0x7b9c).
  - The command channel (IOP block 0x7d140) carries player commands: 8 = create, 5, 2 = start, 4 = sample rate 0xbb80, 9 = volume.
- **CRI output.** The decoded PCM goes to SPU2 core 0 by auto-DMA (`sceSdBlockTrans`, loop mode, ADMAS). It is modelled as an input area in sound RAM (core c: L at 0x2000 + 0x400c, R at +0x200) that drains at 48 kHz. The DMA completes when only the two-block hardware buffer remains queued.

## Verification

- Title sound effect (SNDDRV voices) against `SYSTEM/TITLE.BD` samples decoded independently with `tools/formats/hdbd.py`: per-channel correlation 0.65–0.78 at the expected start. One channel is phase-inverted, which is the game's Dolby Pro Logic II surround placement.
- Title music stream against `ADX01/AD_51.ADX` decoded with `tools/formats/adx.py`: correlation 0.78. Stage ambience against `ADX00/AD_22.ADX`: correlation 0.50 at gain 0.99.
- The streaming handshake runs every frame while a stream plays. It stops when the game stops the stream (leaving the title).
- **Stage 1 ambience, bit-exact:** CRI's PCM output captured with `HG_ADMAWAV` matches `ADX00/AD_22.ADX` decoded independently with correlation 1.0000 and gain 1.000 over 30 s. The stream loops (1,537 sectors read of 911).
- Stage 1 plays continuous sound from load to the end of a 90 s capture, with output RMS 1,000–2,900 (voices, stream and reverb).

### Background music (stage 1), checked 2026-10-13

- **Format.** `BGM/*.SQ` is Sony's SCEI sequence: `IECSsreV` / `IECSuqeS` / `IECSidiM` chunks, then a MIDI-style
  event stream with running status. Two packing rules:
  - If the last data byte of an event has bit 7 set, no delta time follows (delta 0). Otherwise a variable-length
    delta follows.
  - Note-off (`8n`) carries only the key.
  - Parser: `tools/formats/sq.py`. Note-ons and note-offs balance in all four stage-1 files.
- **What plays.** From stage start, `S1_NORMALA` and `S1_NORMALB` play together, both on `BGM/STAGE1_BANK`.
  - That bank is resident in SPU2 RAM at halfword 0x10000; it matches the disc file byte for byte.
  - At the same moment, 22 voices start at volume 0 with the chase-music samples. That's the game preparing the
    chase music silently.
- **Check** (`tools/research/bgm_check.py`, using the key-on log `HG_KONLOG`). Every note of both sequences matches
  a key-on over a full 164 s pass plus the loop:
  - 47 + 9 notes; worst timing error 3.3 ms, under one 240 Hz driver tick.
  - Each key-on starts at the right bank sample (Vagi offset).
  - Transposition is exact against the sample's native rate (root key 60).
  - The envelope settings match the bank's Smpl records.
- **Loop.** NRPN CC99 = 0 at tick 1 and CC99 = 1 at 163.2 s mark the loop. The second pass starts 163.2 s after
  the first.
- **Pitch bend over the loop.** Channel 2 bends fully down by 157.7 s, and its reset lies after the loop end.
  So from the second pass on, that channel plays 2 semitones low. This is in the game's data, and the real driver
  reproduces it.

### Reverb

- **Configuration.** The game configures core 1 for `SD_EFFECT_MODE_STUDIO_2`; the coefficients written are the
  "Studio Medium" preset from psx-spx. 12 of the first 66 key-ons route voices to the effect send.
- **The effect return volume (EVOL) stays 0.** It is set through SNDDRV RPC function 0x150000 (`+0x6934`, which
  writes EVOL through `sceSdSetParam` at `+0xb830`).
  - The EE wrapper is `FUN_0020ef50(snd, core, depth)`.
  - Stage init (`FUN_0039d310`) explicitly calls it with depth 0 for both cores.
  - The room script entry (`FUN_00209390`) does the same for core 1.
  - So stage 1's opening room is dry by design, unless a later room script raises the depth.
  - No reverb was audible anywhere in the tested content (title, menu, first room), so the reverb implementation
    remains unverified.

## Bugs found on the way (fixed)

- An exponential envelope decrease must use an arithmetic shift (`step * level >> 15`). Truncating division stalls release at level 1, so ENVX never reaches 0 and SNDDRV never frees the voice.
- `ReferThreadStatus` on the EE must report SUSPEND (8) / WAITSUSPEND (0xC). CRI's EE helpers resume their server thread only when they see those values. Without them, ADX streaming stalled after the first buffer.
- Scripted input must be timed in emulated VBlanks. A Start press meant for the title paused the stage, and pausing mutes all sound.
- (2026-10-15, playtest 2) The reverb end address (EEA) must be masked to the 2 MB decode. SNDDRV writes `0x3FFF`
  (top of RAM as byte address >> 17); unmasked, core 0's reverb writes swept all of sound RAM every 43.7 s and turned
  the sample banks into residue: voices looped on garbage blocks (a constant beep at the voices' pitches) and
  one-shots and the stream input area scratched. The verifications above were not affected: they checked key-ons
  and the stream's DMA input, not what the voices read back from RAM.
- (same) Decay and release shifts are the register fields themselves (decay bits 7-4, release bits 4-0, step -8);
  they were passed as `shift << 2`, so releases lasted minutes instead of seconds. Envelope waits above shift 26 now
  last `1 << (shift - 11)` samples. `tools/verify/audiocheck.sh` checks a replay's mix for stuck tones, DC and clicks.

## Known gaps

- Reverb is implemented from the PS1 formula with the PS2 register mapping, but it is not yet compared against hardware. The 24 kHz resampling uses the documented FIR.
- No reference recordings yet (PCSX2 + the user's BIOS planned) to compare full mixes.
- Reverb depth is 0 in all tested content (see above); the reverb path needs a room that sets a depth, or a reference recording.
- Speed: in 3D the host runs below real time (the software GS dominates; see the README). Headless WAV capture is unaffected because time is emulated.
