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

## Bugs found on the way (fixed)

- An exponential envelope decrease must use an arithmetic shift (`step * level >> 15`). Truncating division stalls release at level 1, so ENVX never reaches 0 and SNDDRV never frees the voice.
- `ReferThreadStatus` on the EE must report SUSPEND (8) / WAITSUSPEND (0xC). CRI's EE helpers resume their server thread only when they see those values. Without them, ADX streaming stalled after the first buffer.
- Scripted input must be timed in emulated VBlanks. A Start press meant for the title paused the stage, and pausing mutes all sound.

## Known gaps

- Reverb is implemented from the PS1 formula with the PS2 register mapping, but it is not yet compared against hardware. The 24 kHz resampling uses the documented FIR.
- No reference recordings yet (PCSX2 + the user's BIOS planned) to compare full mixes.
- SNDDRV's BGM sequences (`BGM/*.SQ`) are loaded in stage 1 but not yet heard or verified.
- Speed: in 3D the host runs below real time (the software GS dominates; see the README). Headless WAV capture is unaffected because time is emulated.
