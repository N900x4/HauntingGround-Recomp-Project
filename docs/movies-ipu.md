# Movies: IPU emulation for the game's own Sofdec player

Date 2026-10-13. Movies are CRI Sofdec `.SFD` files: an MPEG system stream with MPEG-2 video and ADX audio. The
game's own player code (CRI MWPLY, recompiled) decodes them. The runtime emulates the hardware it uses:

- the IPU;
- the IPU DMA channels 3 and 4;
- the scratchpad DMA channels 8 and 9;
- the EE timers.

So playback, sound, timing and presentation are the game's own.

## How the player uses the hardware (traced with REA and `HG_IPULOG`)

- **Setup.** At start-up: IPU reset, MPEG-1 mode, `SETIQ` (intra and non-intra), `SETVQ`, `SETTH 0/0`.
- **Bitstream feed.** Channel 4 runs in chain mode over a ring of DMA tags. The player watches the available bits
  (`IPU_BP`: IFC×128 + FP×128 − BP) and channel 4's QWC and CHCR; it refills when fewer than 32 bits remain and
  the channel is idle.
- **Headers.** Parsed with `FDEC n`, which skips n bits and then peeks 32. Values come from IPU_CMD and IPU_TOP.
  The streams are **MPEG-2**: the sequence and picture-coding extensions select 10-bit DC precision, the
  non-linear quantiser scale and intra VLC table B.15. The player writes these into IPU_CTRL (IDP, QST, IVF,
  PCT).
- **Macroblocks.**
  - VDEC handles the macroblock address increment (0x22 = stuffing, 0x23 = escape), the macroblock type, motion
    codes and dmvector; FDEC reads motion residuals.
  - Then BDEC, whose 768-byte RAW16 output goes over channel 3 into a scratchpad double buffer (0x1800/0x3300).
- **Motion compensation** runs on the EE. Reference macroblocks are gathered into the scratchpad by a channel 9
  chain (four `ref` tags). The player keeps its pictures as RAW8 macroblocks **column by column** (28 per
  column for 512×448).
- **Colour conversion.** Per picture, the player stops channel 4, issues `BCLR`, and points channel 4 at the RAW8
  picture (896 macroblocks). `CSC` converts it to RGB32 over channel 3. The game then restores the bitstream DMA.
- **Pacing.** The player's thread sleeps between frames. EE timer 2 (BUSCLK/256, compare and overflow interrupts)
  drives CRI's time base.

## What was needed

| Piece | Where | Notes |
|---|---|---|
| IPU | `recomp/ipu.c` | <ul><li>FIFO/BP/TOP model.</li><li>VLC tables B.1–B.4, B.9–B.15.</li><li>MPEG-1/2 dequantisation, DC precision, mismatch control, reference IDCT.</li><li>CSC with SETTH alpha.</li><li>Commands stall when input is empty or output full, and resume as DMA moves data (CSC output is 64 qwords per macroblock).</li></ul> |
| IPU DMA | `recomp/ipu.c`, `recomp/hle.c` | <ul><li>Channel 4: normal and chain mode, progressive.</li><li>Channel 3: progressive.</li><li>CHCR writes without STR stop a channel (the player suspends the bitstream DMA around CSC).</li></ul> |
| Scratchpad DMA | `recomp/hle.c` | Channels 8/9 were not executed at all (only 1/2 were). Without them motion compensation used zero references. |
| EE timers | `recomp/hle.c` | Counters on emulated time; compare/overflow interrupts as INTC causes 9–12; the idle scheduler advances to the next timer or VBlank event. |
| EE speed | `recomp/runtime.c` | Default 300 M guest instructions/s (was 150). At 150 the player fell behind in emulated time and the IOP padded the movie sound. |

## Verification

- **Bitstream.** `CAPCOM.SFD` (181 pictures) and `OPENING.SFD` (6,300 pictures, 176,400 slices) parse with the
  IPU's VLC tables exactly up to every next start code. This is an offline parser that shares the tables. Bugs
  found this way: the motion-code entries for 11–16 were wrong, and table B.15 was missing.
- **Pictures.**
  - The player's YCbCr pictures (CSC input, dumped with `HG_IPUDUMP=dir`) were compared with an independent
    decode by the system's libavcodec, called through ctypes.
  - All 156 pictures captured of `CAPCOM.SFD` (the run stopped before its end) are within ±2 levels: 70–76 dB
    PSNR, 0 to about 1,400 samples per picture off by 1–2. The rest is IDCT rounding (the IPU's exact IDCT
    arithmetic is not documented).
- **Sound.** The movie audio the IOP sends to the SPU2 (`HG_ADMAWAV`) is bit-identical to the SFD's ADX track,
  decoded independently:
  - all 209.6 s of `OPENING.SFD`, every sample;
  - the checked 15 s of `LOOP_DEMO.SFD`.
- **End to end.** With `HG_PAD=1500:start,1800:start,2100:cross`, the opening plays to its end and stage 1
  starts at about VBlank 14,400. The whole 16,000-VBlank run takes 132 s of host time.

## Using it

- **Default boot sequence:**
  1. logos;
  2. `CAPCOM.SFD`;
  3. the title background movie;
  4. the `LOOP_DEMO.SFD` intro (Start skips it to the title);
  5. title and menu;
  6. New Game, then `OPENING.SFD` (210 s);
  7. stage 1.
- **Scripted run:** `HG_PAD=1500:start,1800:start,2100:cross` reaches the opening movie at about VBlank 2160.
- **Movies off:** `HG_MOVIES=skip` restores the skip, which also restores the old timeline.
- **Diagnostics:**
  - `HG_IPULOG=1` logs rare commands, IPU DMA starts and stops, and the last events at exit; `=2` logs every
    command.
  - `HG_IPUDUMP=dir` writes each CSC input picture.
