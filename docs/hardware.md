# Section B: hardware dependence (VU, GS, IOP, timing)

Target: `iso/SLUS_210.75`. Addresses are EE virtual addresses. File offset = VA - 0x100000 + 0x80.
Tags: [OBS] observed directly, [INF] inferred, [UNK] unknown.

All scripts are in `feasibility/poc/hw_*` and must be run from the project root. Raw outputs are in `feasibility/poc/hw_out/`.

| Script | Purpose |
|---|---|
| `hw_vu_find.py FILE` | Scans any file for VIF MPG packets |
| `hw_vudis.py [--dis N]` | VU micro-mode disassembler and per-program statistics |
| `hw_eescan.py hw\|cop3\|vifc\|refs LO HI` | Finds EE hardware-register refs, COP0/COP2/syscall stats, vifcode immediates and address constants |
| `hw_adscan.py` | Finds GS A+D register writes in code |
| `hw_adextract.py` + `hw_gsdecode.py` | Decode the FRAME/TEX0/ALPHA/TEST values found in decompiled packet builders |
| `hw_HwQ.java` | Headless-Ghidra dump script, run on a scratch copy of the project |
| `hw_ghidra_funcs.tsv` | Ghidra's function list: 7,340 functions |

The code range used for the statistics is 0x100000–0x3a1b00. There are also two small islands: 0x3ea800–0x3ec000 (kernel-mode helper) and 0x464000–0x46a000. Everything from 0x3a1b00 up is data.

## Contents

1. [Vector units](#1-vector-units)
2. [GS usage](#2-gs-usage)
3. [IOP side](#3-iop-side)
4. [Timing](#4-timing)
5. [Open items](#5-open-items-with-rough-cost)
6. [Bottom line for section B](#bottom-line-for-section-b)

---

## 1. Vector units

### 1.1 VU1 microcode

The game has 19 VU1 microprograms. They are all in the ELF and none are on the disc.

- [OBS] There is one contiguous block at 0x3a1b80–0x3ac7b0, inside .data.
  - It contains 19 DMA packages.
  - Each package is a `CNT qwc=0` tag carrying `BASE`/`OFFSET` vifcodes, followed by a `RET` tag carrying 1–3 `MPG` vifcodes.
  - All MPGs load at VU address 0 and continue at 0x100/0x200. The largest program is 611 instructions (#15). 0x262 doublewords is beyond VU0's 4 KB micro memory, so these are VU1 programs.
  - Listing: `hw_out/vu_programs.txt`.
- [OBS] A disc scan found zero MPG packets. Files checked: PCK, PAC, BIN, DP, TEX and the character files in `data/`.
- [OBS] Model files hold raw arrays, not VIF streams. `tools/hgmodel.py` reads s16 positions /4096, s16 normals /32768 and u16 weights. The EE builds every VIF tag at runtime.

**Double-buffer settings.** Each program comes with one of three settings, used with `XTOP`:

| BASE | OFFSET |
|---|---|
| 0x00 | 0x200 |
| 0x20 | 0x1f0 |
| 0xc8 | 0x19c |

**Common structure.** All 19 programs are built the same way [OBS, disassembly `hw_out/vu_prog0.dis`]:

- **Entry 0 (initialisation).** Reads the constant block from vi1 into vf1–vf13: two 4x4 matrices, a 3x3 normal matrix and a scale. Then it hits the E-bit.
- **Batch entry.** Execution continues after the E-bit and branches back to the batch loop (`B -> 0x11` in #00). Each batch does:
  1. `XTOP`.
  2. Reads the GIF tag.
  3. Per vertex: `ITOF12` positions/UVs, `ITOF15` normals.
  4. `MULAx/MADDAy/MADDAz/MADDw` matrix transform.
  5. `CLIP`, then `FCAND` plus `ISW` to set the ADC bit on clipped vertices. This culls; there is no real clipper.
  6. `DIV`/`WAITQ` perspective divide and `FTOI4` screen coordinates.
  7. One `XGKICK` per batch.

**Instruction usage across all 19 programs:**

| Feature | Use |
|---|---|
| `XGKICK` | Exactly 1 per program |
| `XTOP` | 1–2 per program |
| `XITOP` | None |
| EFU (`ESIN`/`EATAN`/`EEXP`/`ERSQRT`) | None of these, but **`ERLENG` is used** (108 K times in the first room; found by the VU1 interpreter 2026-10-12, the static scan missed it) |
| `ILWR`/`ISWR`, `LQI`/`SQD` | None (only one `SQI`) |
| `MFP` | 3–8 per lit program. [UNK] No EFU op writes P; possibly harmless compiler output. |
| Lit variants | `MAXx vf, vf, vf0` (N·L clamp) and `MINIi 255` colour clamp |

**Program roles.** These come from the EE code that calls each package with a DMA `CALL` tag (`0x5000_0000 | addr<<32`) [OBS]:

| Packages | EE caller | Role |
|---|---|---|
| #01 (0x3a1ff0), #07 (0x3a5c60) | 0x1bd280 | 2-target morph (writes `1.0-w`, `w`), lit / unlit flag variant |
| #15 (0x3aa050), #08 (0x3a60f0) | 0x1bcd30 | 16-target blend-shape morph (`w0 = 1 - Σw1..15`). Faces. |
| #16 (0x3ab3a0), #17 (0x3abcf0) | 0x1bc8d0 | Morph variant |
| #11–#14 (0x3a77f0, 0x3a80c0, 0x3a8a70, 0x3a9520) and #05/#03/#04/#09 | 0x1bd830 | Skinning with 1/2/3/4 bone weights. Matrix palette is unpacked as V4-32. Two material-mode sets. |
| #02 (0x3a2ef0), #06 (0x3a58a0) | 0x1bc370 | Rigid mesh with 3 directional lights (normal matrix plus light vectors at +0x190) |
| #00, #06, #07, #10 | 0x267560 / 0x267d60 / 0x2677c0 / 0x267ab0, via dispatcher 0x268090 | Second renderer family [INF: stage / props] |
| #18 (0x3ac3f0) | 0x33b560, 0x33d9f0 | `PRIM=0x5c`: textured, alpha-blended triangle strip. Uses `MSCAL` + `MSCNT`. [INF: effect / decal] |

**Geometry stream.** The EE builders `FUN_002b86a0` and `FUN_002b89c0` send geometry like this:

- `STROW` + `STMASK`.
- Per batch of at most 48 vertices: `UNPACK V3-16`/`V2-16`/`V4-8`/`S-8` with `REF` tags into the model arrays, then `MSCNT` (0x17000000).
- `FLUSHA` (0x13000000) at the end.

Microcode is re-sent through `CALL` on every draw. There is no residency cache [OBS], which makes it easy to reason about.

**Sync tricks.** None exotic were found.

- [OBS] No `MSKPATH3`/`MARK` vifcodes. The `lui 0x0600/0x0700` hits in `vifcode_lui.txt` are file-loader flag constants (0x6000000/0x7000000), checked in decompiles of 0x398850 and 0x2cb9a0.
- [OBS] No `DIRECTHL`.
- [OBS] VPU-STAT (`cfc2 $29`) is polled only inside libgraph `sceGsSyncPath`, at 0x10ca84, 0x10caa4 and 0x10cc80.

### 1.2 VU0

- [OBS] Zero `VCALLMS`/`VCALLMSR` in code. The 4 raw hits are all in data. No VU0 microprogram exists.
- [OBS] VU0 macro mode (COP2) is confined to a libvu0-style library at 0x10daa8–0x10e7f0 (about 26 small leaf functions) plus 0x2e2da0 and 0x2e2dd0.
  - In code there are 97 COP2 arithmetic ops, 74 LQC2, 31 SQC2, 16 QMTC2 and 2 QMFC2.
  - These leaves are called heavily. For example, 0x10dd58 has 833 xrefs and 0x10daa8 has 328 (`hw_ghidra_funcs.tsv`).
  - [INF] These are sceVu0-style matrix/vector helpers. They can be reimplemented as about 30 SSE/NEON functions.
  - The rest of the game math is FPU.

### 1.3 Assessment

[INF] All 19 programs are short, regular, branch-light transform/light/kick loops. They have no EFU use, no VU0↔EE concurrency and no `XGKICK` interleaving tricks. Two options:

- **Interpreter:** a VU1 interpreter is about 2 kLOC.
- **Native rewrite:** each program can be rewritten as CPU or GPU vertex shaders, in families: rigid-lit, skinned 1–4, morph-2, morph-16 and effect.

The VIF state used is limited to `STCYCL`, `STROW`, `STMASK`, `BASE`, `OFFSET` and `UNPACK` with V3-16, V2-16, V4-8, S-8, V4-32 and V4-16.

---

## 2. GS usage

### 2.1 Paths

- **PATH1** [OBS]: geometry from VU1 `XGKICK`.
- **PATH2** [OBS]: VIF `DIRECT`. This carries:
  - Per-draw state: PRIM, ALPHA 0x44, TEX0, TEST (`FUN_002b72d0`).
  - All texture/CLUT uploads: `BITBLTBUF`/`TRXREG` with `IMAGE` data via `REF` (0x1bab80, 0x1bb010 T4+CLUT, 0x1bb230 T8, 0x1bb780).
  - All post-processing passes.
- **PATH3** [OBS]: GIF DMA channel (`sceDmaGetChan(2)` at 0x1b80c0) is used only for:
  - The end-of-frame blit packet at 0x47d300 (`FUN_001b75e0`).
  - The VRAM clear at boot (`FUN_001b7ed0`).

  The main chain goes over VIF1 through `sceDmaSend` (0x1b87d0).

### 2.2 Video mode

- [OBS] Uses `sceGsResetGraph(0, 1, 2|3, 0)`: interlaced, NTSC or PAL, FFMD=0 (field).
- [OBS] A `'P'` option calls `ResetGraph(0, 0, 0x50, 0)`: 480p progressive (0x1bb9e0). Display height is 0x1c0 = 448.
- [OBS] PMODE/DISPLAY registers are written only by libgraph (0x10c440).

### 2.3 VRAM layout

From `FUN_001b80c0`, `FUN_001b79f0` and `FUN_001b71e0` [OBS]:

| FBP / ZBP | VRAM word address | Use |
|---|---|---|
| FBP 0 | 0x00000 | 640x448 display/front buffer, CT32/24 |
| ZBP 0xa0 | 0x50000 | Z24 depth buffer |
| FBP 0x110 | 0x88000 | 512x448 CT32 3D render target |
| 0x180, 0x19c, 0x1a0, 0x1b8, 0x1f0 | 0xc0000–0xf8000 | 256- and 128-wide scratch targets used by effects |
| ZBUF 0x1bc | — | Alternate depth buffer |

### 2.4 Effects found

Pass table: `hw_out/gs_passes_by_func.txt`. Effect dispatcher is at 0x1b5ec0.

**(a) Frame feedback / motion blur and upscale (every frame).** [OBS] `FUN_001b75e0`.
- Draws the 512x448 render target onto the 640x448 display as a sprite: TEX0 = render buffer, `REGION_CLAMP`, bilinear.
- `ALPHA (Cs-Cd)*FIX+Cd`, with FIX = byte@+0x304c06 (default 0x20) and ABE switchable. This is the persistent afterimage/blur.
- Sub-pixel jitter offsets come from bytes +0x304c07/08.
- [UNK] When the render size is over 512x448, TEX0 is PSMT8 with CLD=1 and CBP=0x3f00.

**(b) Glow/bloom (0x1ba090/0x1ba260).** [OBS]
- 128-wide buffers at FBP 0x1f0 and 0x180.
- Down-sample in 32-pixel sprite strips, then blur with `ALPHA (Cs-Cd)*0x40+Cd`.
- Add back to 0x110 with `(Cs-0)*0x40+Cd`. 

**(c) Many other half-resolution chains.** [OBS] Copy the render target to 256x256 at 0xc0000 → blur ping-pong 0xce000/0xdc000 → composite. Functions: 0x1ab960, 0x1af3b0, 0x1b4330, 0x1b4f30, 0x2685f0, 0x2699d0, 0x34e9e0, 0x370100, 0x374e50, 0x376990.
- Subtractive darkening, `ALPHA (0-Cs)*FIX+Cd`, appears in 0x1b4330 and 0x1b4f30. [INF] These are panic and vignette darkening.

**(d) Depth read as texture.** [OBS] TEX0 `tbp=0x1400 psm=Z24` (the Z buffer) is sampled into FRAME `psm=Z24`.
- Sites: 0x1ac0d0, 0x2e3500, 0x301e70.
- 0x301e70 then blends with FIX 0x40, 0x30, 0x20 and 0x10.
- [INF] This is depth of field / depth fog.

**(e) Destination-alpha stencil.** [OBS] `TEST date=1 datm=1`, `FBA`, and `FRAME fbmsk=0xffffff` (alpha-only writes).
- Sites: 0x1ac0d0, 0x1b2160, 0x1f2b80, 0x316de0, 0x355aa0, 0x1ab3f0, 0x1b18e0, 0x1b1e50.
- [INF] Builds masks in framebuffer alpha for selective effects or shadows.

**(f) Format aliasing and wrap-around colour math.** [OBS]
- FRAME/TEX0 `PSMCT16S` over a CT32 area with `fbmsk=0x3fff` (0x21d8f0).
- `COLCLAMP=0` (wrap) in 0x21e410.
- [INF] Channel/bit shuffling, which is the hardest class to port.

**(g) Other features.** [OBS]
- Generated CLUTs uploaded through `HWREG` (0x1b8d30, 256 entries). [INF] Palette colour fades.
- Ten 256x256 and one 512x512 **PSMT8** fixed-address VRAM textures (0x1b8250).
- `TEXA aem=1` with subtractive alpha (0x2bc040, 0x31d9c0, 0x31de10, 0x35cee0).
- Two-pass alpha (`ATST LESS 0x80, AFAIL=ZB_ONLY`) in the per-draw state.
- [INF] Film-grain / dust layer: a 16-element random array (0x1b83d0, 0x1b9d50).

**Not seen in A+D scans:** `FOGCOL`, `DTHE` and `SCANMSK`. One unresolved `0x22` store exists in 0x174920.

### 2.5 PCSX2 hacks for this game

[OBS] `GameIndex.yaml` lists the same fixes for SLUS-21075, SLES-52877 and SLPM-65913:
- `halfPixelOffset: 5`
- `bilinearUpscale: 1`
- `nativeScaling: 2` ("Fixes post-processing")
- `textureInsideRT: 1` ("Fixes post effects")

This confirms that (a)–(f) are the pain points.

### 2.6 Hard to reproduce natively

[INF] Hardest first:

1. CT16S aliasing and `COLCLAMP` wrap (f).
2. Z24-as-texture (d).
3. DATE / FBA / FBMSK alpha-stencil (e).
4. Texture-inside-render-target offsets into the 0xc0000–0xf8000 scratch area.
5. Half-pixel-exact down-sample chains (b, c).
6. Frame feedback blend (a).

Each is reproducible with a dedicated shader pass, about 30 bespoke passes in total. A generic GS emulation layer gets them "for free" at the cost of the usual GS-HLE complexity.

---

## 3. IOP side

### 3.1 Module list

[OBS] All of these are loaded from `cdrom0:\MODULES\` by the loader 0x1bc0f0, which retries `sceSifLoadModule` until it succeeds.

| Module | Loaded by | Notes |
|---|---|---|
| `IOPRP300.IMG` | 0x1bc220 | `sceSifRebootIop` with "rom0:UDNL ". SDK 3.0; libcdvd identifies itself as "PsIIlibcdvd 3000". |
| SIO2MAN, SIO2D, DBCMAN, LIBSD | 0x1bf080 | DBCMAN is the libdbc controller stack, not padman. |
| DS2O_S1 | 0x1be6a0 | [INF] libds2o, DualShock 2 |
| MODHSYN, MODMIDI, MODMSIN | 0x2102e0 | Sony modular sound system (CSL hardware synth for HD/BD, MIDI sequencer for SQ, MIDI stream) |
| SNDDRV.IRX | 0x2102e0 | Capcom sound driver; see 3.2 |
| MCMAN, MCSERV | 0x226570 | Memory card |
| CRI_ADXI.IRX | 0x1aace4 | ADX streaming |

**Embedded module.** [OBS] One small IRX is embedded in the ELF at VA 0x44a9a0 (file 0x34aa20).
- Name `ck01` v1.1, 0x90 bytes of code.
- It calls cdvdman import #0x32 with argument -9 and returns 5 or 1.
- Sent to the IOP with SIF DMA and started by 0x37e1f0, called from 0x2d1e40.
- [INF] A disc/drive check probe.

**Missing files.** [UNK] The `MODULES` folder is not in the local copy: `iso/` holds only SLUS, SYSTEM.CNF and DATA.CVM. Module versions and contents cannot be checked without the full disc image.

### 3.2 Custom sound driver

[OBS] SNDDRV.IRX is a custom driver:
- Started with 10 hex/decimal arguments.
- Runs RPC servers 0x77777777 and 0x77777778, bound at 0x220270.
- 38 call sites of the command function 0x21fb70 issue about 25 distinct command codes, from 0x04 to 0x38, in the upper 16 bits of `sceSifCallRpc`. Payloads are 0x20/0x88/0xb4-byte structs.
- An EE thread (0x220150) pumps it.

[INF] This means the sound engine must be reimplemented in two parts:
- Standard HD/BD/SQ playback (formats are known).
- The SNDDRV command semantics (voice/SE/BGM control). These must be learned by disassembling SNDDRV.IRX from the full disc.

ADX streams (CRI_ADXI) and Sofdec (EE IPU + ADX) are standard CRI and are replaceable by libraries.

---

## 4. Timing

### 4.1 Interrupt handlers and frame lock

- [OBS] `AddIntcHandler(2 = VBLANK start, 0x1beda0)` sets a flag at gp-0x76ec and increments a counter at gp-0x76e4.
- [OBS] `AddIntcHandler(3 = VBLANK end, 0x1bed80)` sets a flag at gp-0x76e8. Both are installed in 0x1bf080.
- [OBS] CRI ADX installs its own VBLANK-end handler (0x1aad88).

The main loop body is `FUN_001beef0`:
1. Kick the DMA chain.
2. Busy-wait (nop spin) for VBLANK start.
3. If `count - last < 2`, wait for another VBLANK.
4. Reset T1, `sceGsSyncPath`, swap buffers.
5. Busy-wait for VBLANK end, then reset T0.
6. Run the subsystem updates (0x1b85b0, sound 0x210230, ADX 0x1aacc0, 0x16bfb0, game 0x2d42a0, 0x1be4b0, 0x226510).

[OBS] The vblank counter is read only inside this loop; no delta time is passed on.

[INF] The game is locked to 30 fps (2 NTSC fields) with fixed-step logic. Slow frames slow the game down.

[INF] Running at 60 fps would double game speed: animation, physics, AI timers and the frame-feedback blur strength. It needs either interpolation (render at 60, update logic at 30) or a patch of every per-frame step. Many `30.0`/`60.0` float constants exist in code (`lui 0x41f0`: 116, `lui 0x4270`: 54); they were not audited.

### 4.2 Timers and other hardware polling

- [OBS] T0 and T1 run with `MODE=0x82` (BUSCLK/256 ≈ 576 kHz) and are zeroed once per frame. No loads of T0/T1 COUNT were found by the lui-pair scan. [INF] A debug profiler.
- [OBS] T2 plus INTC 11 is used by an SCE-style alarm/timer library (0x276b70–0x277440). [UNK] Which client uses it.
- [OBS] `mfc0 Count` ($9) never appears in code. The only other hardware polling is libgraph `sceGsSyncPath` (D1/D2/VIF1/GIF/VPU-STAT) and CRI IPU/SPR DMA handlers (`AddDmacHandler` at 0x2a6c00 and 0x2a6e40).
- [INF] No cycle counting. Replace VBLANK with a host timer or present-signal and the loop works.

---

## 5. Open items (with rough cost)

1. **Get `\MODULES\*.IRX` and `IOPRP300.IMG` from the full disc.** Then disassemble SNDDRV.IRX for the command table (≈200–400k tokens) and record module versions (cheap).
2. **Per-function walkthrough of the ~40 post-effect functions.** Name each effect and map it to its in-game trigger (panic, fade, DOF, glow). About 300k tokens.
3. **Audit 30/60 Hz-dependent constants in game logic** for a 60 fps mode. About 200k tokens.
4. **Formal VU1 → C translation of the 19 programs.** About 150k tokens, or reuse an existing VU interpreter.

---

## Bottom line for section B

[INF] The vector-unit side is easy:
- 19 regular VU1 transform programs, translatable to shaders.
- VU0 is used only as a math library.
- No VU0 microcode and no sync tricks.

[INF] The difficulty is concentrated in two places:
- **GS post-processing:** about 40 functions using render-to-texture, Z-as-texture, DATE alpha stencil, CT16S aliasing and colour wrap.
- **Custom SNDDRV IOP driver:** its binary is not in the local copy.

[INF] Timing is a simple fixed 30 fps VBLANK lock.
