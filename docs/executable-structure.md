# Section A: executable structure and static-recompilation hazards (SLUS_210.75)

Tags: [OBS] observed directly (address, count or snippet given) · [INF] inferred from observations · [UNK] unknown/open.
Scripts are in `feasibility/poc/` and were run from that directory. Their inputs and outputs are in `feasibility/poc/data/`.
Ghidra data comes from a **copy** of the project in scratch, exported with `ExportFuncs.java` (headless, read-only). Everything else comes from a dedicated R5900 decoder (`r5900.py`) that covers the full EE ISA, including MMI and COP2. Over the 689,888 words of `.text` it found 0 undecodable words.

## Key numbers

| item | value |
|---|---|
| `.text` (code) | **0x100000–0x3a1b80**, 2,759,552 B (689,888 instruction words, of which 60,462 are `nop`) [OBS] |
| data (file-backed) | 0x3a1b80–0x47b200, 890,496 B, then bss up to 0x1992000 [OBS] |
| code islands inside data | static-ctor ("sinit") code at 0x464480–0x469460 (20 KB, 143 ctors). Two libkernel kernel-mode patches at 0x3eaa70 (0x330 B) and 0x3eb0e0 (0x7a8 B). An embedded R3000 IRX at 0x44a9a0 [OBS] |
| functions (Ghidra) | 7,340 (7,328 in `.text`) [OBS] |
| functions (own, strict) | **10,710** in `.text` = Ghidra 7,328, plus 13 jal-only targets, plus **3,369 reachable only through data pointers (vtables)**. On top of that come ≥75 targets reached only by tail-`j` and 143 sinit functions, about **10,930 in total** [OBS/INF] |
| `jr ra` count | 10,067 [OBS] |
| game vs libraries (strict count / bytes) | game **7,039 fns / 2.15 MB (77.8 %)**; CRI 2,776 / 399 KB (14.5 %); Sony 539 / 108 KB (3.9 %); Metrowerks runtime 356 / 105 KB (3.8 %) [INF] |
| indirect calls | 9,183 `jalr`, of which 8,918 match the vtable pattern `lw t9,0(obj); lw t9,slot(t9); jalr t9`. Another 165 `jr t9` are virtual tail calls [OBS] |
| switch jump tables | 228 tables (225 bounded automatically, 3 with a hoisted bound), 3,970 entries, 2,521 distinct targets, all in 0x44c940–0x464420 (data), and every target lies in `.text` [OBS] |
| translator opcode coverage | **99.80 %** of all instruction words (game code 99.997 %) [OBS] |

## 1. Code vs data extent

[OBS] Layout of the single LOAD segment:
* `0x100000–0x3a1b80`: `.text`. Its last bytes, 0x3a19b0–0x3a1b78, hold **29 C++ this-adjusting thunks** of the form `j target; addiu a0,a0,-0x40`, which point to multiple inheritance. At 0x3a1b80 a GIF/DMA-packet-like table begins (`10000000 00000000 03000000 02000200 60000045 …`).
* `0x3acd30…`: data with `PsII…` version strings, CRI build strings, and the other `.data`/`.rodata`.
* `0x3eaa70` (0x330 B) and `0x3eb0e0` (0x7a8 B): kernel-mode code. libkernel copies it to `0x80075000` and `0x80074000`. Evidence: `0x275b60: lui a1,0x3f; lui a0,0x8007; li a2,0x330; addiu a1,-0x5590; jal 0x275ac8 (syscall 0x5a copy); ori a0,0x5000`, then `FlushCache(0)` and `FlushCache(2)`. This code is linked for 0x74000–0x77000 (it contains `jal 0x75158` and `jal 0x76460`) and has to be HLE'd, not translated.
* `0x44a9a0`: a small embedded IOP module (ELF magic; `jal 0x54` and `sw ra` show R3000 code), referenced from 0x37e200. It belongs to the hardware worker's area and must not be translated.
* `0x44c940–0x464420`: all 228 switch tables, plus vtables and other data.
* **`0x464480–0x469460`: static initialisers.** This is real EE code outside `.text`. Ghidra found only 3 of its functions. The ctor table at **0x469460–0x46969c** holds 143 entries and is walked by `crt_init_globals` 0x100290 (`0x1002a0/0x1002a4` load 0x469460 and 0x46969c). The functions register destructors through `jal 0x100ab0`.
* [INF] `.text` contains no data islands: 0 undecodable words, and every jump table sits in `.data`.

### Libraries (versions and address clusters)
The split was inferred from string anchors, followed by call-graph propagation (`classify.py`), and then checked by hand at the boundaries (`callmap.py`). Boundaries are accurate to within a few functions [INF]. Table: `poc/data/comp_table.md`.

| range | component | version evidence [OBS] | strict fns |
|---|---|---|---|
| 0x100000–0x1004f0 | crt0 (entry 0x100008) | | 9 |
| 0x1004f0–0x10be10 | Metrowerks C++ runtime / MSL C++ (EH, iostream, string, vector) | RTTI strings `!std::exception!!std::bad_alloc!!` 0x44c8b0; `.comment` "MW MIPS C Compiler (2.4.1.01)" | 204 |
| 0x10be10–0x10ef38 | Sony libgraph / libdma / libvu0 / libipu | `PsIIlibgraph3000` 0x3acd30, `PsIIlibdma  3000`, `PsIIlibvu0  3000`, `PsIIlibipu  3000` | 70 |
| 0x10ef38–0x110938 | Sony libcdvd | `PsIIlibcdvd 3000` 0x3ace80, `SceCdCallbackThread` | 23 |
| 0x110938–0x111698 | Sony libmc | `PsIIlibmc   3020` 0x3aea38 | 12 |
| 0x111698–0x11c770 | MSL C (stdio/stdlib/string; exit 0x1136e8, printf family 0x11a3b8) | `0123456789ABCDEF`, `Inf`, `(null)` | 120 |
| 0x11c770–0x1c2d38 | **game #1** | | 1,297 |
| 0x1c2d38–0x1ee740 | CRI ADX middleware: ADXT/ADXF/ADXRT/SJ/CVFS/DVCI/HTCI/LSC/MFCI/PS2RNA/SVM/ROFS/PL2ENC | `ADXPS2 Ver.2.57`, `ADXT/PS2EE Ver.9.34`, `ADXF/PS2EE Ver.7.19`, `ADXRT Ver.3020`, `SJ/PS2EE Ver.6.28`, `CVFS Ver.2.37`, `ROFS Ver.1.76` (builds of 12 Nov 2004 and 3 Sep 2004) | 1,281 |
| 0x1ee740–0x1efb40 | Sony libdbc + libpad2 | `PsIIlibdbc  3020`, `PsIIlibpad2 3020` | 22 |
| 0x1efb40–0x226808 | **game #2** (main = 0x20d860) | | 841 |
| 0x226808–0x25c440 | CRI Sofdec: MWSFD/SFD/SFH/SFX/SUD/M2V/MPV/MPS/CFT | `MWSFD/PS2EE Ver.3.33`, `CRI SFD Ver.1.946`, `CRI MPV Ver.1.933` (17 Sep 2004) | 1,495 |
| 0x25c440–0x26bcc0 | **game #3** | | 393 |
| 0x26bcc0–0x277e90 | Sony libkernel + libc stdio-over-SIF + sifrpc + kernel patches | `PsIIlibkernl3000` 0x3ea988, `SceStdio*Sema`, `SceSifrpcGod`, `# TLB over flow` | 312 |
| 0x277e90–0x29ff10 | **game #4** | | 255 |
| 0x29ff10–0x2a7630 | Sony libmpeg (IPU) | `_sceMpegSliceA0(): error happens`, `[MPEG ERROR]%s` (no PsII tag found [UNK] version) | 100 |
| 0x2a7630–0x319f48 | **game #5** | | 2,113 |
| 0x319f48–0x31caa0 | float libm (acosf/asinf/atan2f/logf/log10f/sqrtf), called from all game ranges and calling MSL C | | 23 |
| 0x31caa0–0x3a1b80 | **game #6** | | 2,140 |

[OBS] No libsd/libsdr EE library is present: there is no `PsIIlibsdr` string. Sound goes through LIBSD.IRX plus a custom `SNDDRV.IRX`/`MODHSYN`/`MODMIDI` on the IOP (strings at 0x457267) and through CRI PS2RNA. [INF] CRI and parts of the Sony libraries were built with a GCC-family compiler, not CodeWarrior: 1,412 of the 1,999 branch-likely instructions are in CRI, against only 109 in 2.15 MB of game code.

## 2. Recompilation hazards

### Indirect control flow [OBS] (`jumptables.py`, `indirect.py`)
* `jr` other than `ra`: **394** in total.
  * 225 are switch tables with an automatically recovered bound (`sltiu; lui/addiu table; sll; addu; lw; jr`), sizes 5–219 (median 11).
  * 3 more are switch tables whose bound check was hoisted: 0x209404, 0x2cc05c, 0x39c508.
  * 165 are `jr t9` virtual tail calls (`lw t9,0(a0); lw t9,0xc(t9); jr t9`).
  * 1 is the `longjmp`-style register restore `jr a2` at 0x10237c (func 0x102320), used only by the MW C++ exception runtime (callers 0x100ed0 and 0x10115c).
  * 193 of the 228 tables are in game code.
* `jalr`: **9,183** (game 8,536). Classification:
  * 8,918 match the vtable-call pattern (double load, then `jalr t9`).
  * 202 call a function pointer loaded from a struct or table.
  * 41 are other cases, 21 call a computed register, and 1 calls a `lui` constant.
* C++: there are **584 vtable-like runs (≥3 consecutive code pointers) in data, with 15,355 slot entries**, and 4,306 code-pointer runs of any length. 163 distinct vtable slot offsets are used.
  * The game's own classes have no RTTI name strings; only the MSL `std::` RTTI strings exist.
  * There are 29 this-adjusting thunks.
  * There is no mangled-name symbol table (stripped).
  * [INF] Exceptions are present only in MSL, through `bad_alloc`, `ios_base::failure` and similar. I found no game call into the throw path. [UNK] I have not proven this.
* **Ghidra misses about 3,370 functions** that are reachable only through vtables. Many are 2–3 instruction getters, for example 0x2e72e0 `lui v0,0x42; jr ra; addiu v0,-0x6f10`. It also misses ≥75 functions that are reached only by tail `j` (for example 0x1e12d8) and 140 of the 143 sinit functions. A recompiler must seed its entry points from data pointers. Otherwise about 30 % of functions are missing.

### Self-modifying, overlay or runtime-loaded code [OBS]
* There are 0 stores with a constant address inside `.text` (out of 30,623 resolved `lui`-pair references).
* `FlushCache(2)`, the I-cache invalidate, appears only 3 times, all in the libkernel kernel-patch installers (0x275b80, 0x276698, 0x276a78). Game code calls only `FlushCache(0)`, the D-cache writeback before DMA, at 9 sites.
* Code copied to scratchpad: none. Only 3 functions reference 0x70000000, all in libmpeg/kernel, and all use it for data.
* Code on disc: **every** `.PAC` (168), `.BIN` (248), `.PCK` (115) and `.TEX` (229) was extracted and scanned, 776 files and about 415 MB. They contain 0 aligned `jr ra` (0x03e00008) and 0 `addiu sp,-N`+`sd/sw ra` prologue pairs (`scan_disc_code.py`). `.PAC` entropy is 7.2 bits/byte, so they may be partly compressed. [INF] Code loaded from those files would still require `FlushCache(2)`, which the game never calls. There are no IRX/ELF files on the disc.
* **Conclusion [INF]: no overlays and no SMC. The whole EE program is in the ELF.** Only the kernel patches (about 2.7 KB) and the IRX are copied or run elsewhere, and both are HLE targets.

### Kernel, threads, interrupts [OBS] (`syscalls.py`; table in `data/syscalls.md`)
* There are 168 `syscall` sites, all in libkernel wrappers (`li v1,N; syscall`). The game code itself contains 0.
* `CreateThread` has 9 call sites:
  * libcdvd 0x10ef38;
  * 6 in the CRI thread setup 0x1cb810–0x1cbb18 (ADX/SJ/DTX server threads);
  * **game 0x220150** (1);
  * libkernel 0x26cf78 (`SceKerneltopThread`).
* `CreateSema` has 41 sites. Most are in Sony libc stdio, libcdvd and libmc, CRI has 0x24ed90, and the game has 1 (0x220150).
* Calls by callers:

  | call | game | libraries |
  |---|---|---|
  | `WaitSema` | 0 | 64 |
  | `SignalSema` | 0 | 92 |
  | `iSignalSema` | 0 | 8 |
  | `WakeupThread` | 0 | 9 |
  | `ChangeThreadPriority` | 0 | 21 |
  | `RotateThreadReadyQueue` | 0 | 1 |

* Interrupt handlers:
  * `AddIntcHandler`: game 0x1aacd0 (VSync, inside the CRI init), game 0x1bf080 (2 handlers), and the libkernel alarm 0x276b70.
  * `AddDmacHandler`: libkernel/SIF 0x26f000, and libmpeg IPU 0x2a6b68 and 0x2a6d28.
  * Small game leaf functions end in `sync; jr ra; ei`, for example 0x21f280, 0x21f2a0 and 0x21f2c0.
* Exception vectors: `SetVTLBRefillHandler` is called 4 times inside libkernel (`_InitTLB`, "TLB over flow" strings). The kernel patches also scan kernel RAM 0x80000000–0x80080000 (0x276468).
* Privileged instructions:

  | instruction | total | game |
  |---|---|---|
  | `sync` | 81 | 6 |
  | `ei` | 18 | 6 |
  | `di` | 3 | 0 |
  | `cache` | 17 | 0 |
  | `mfc0` / `mtc0` | 33 / 11 | 0 |
  | `eret` | 4 | 0 |
  | `break 7` | 153 | 36 |

  All `break 7` are divide-by-zero traps.
* [INF] Because CRI and the Sony libraries run cooperating EE threads with priority-based scheduling, the recompiled program needs a thread model (host threads or fibers with priority semantics). The alternative is to replace CRI ADX/Sofdec and the Sony libraries completely with HLE.

### Delay slots and control-flow oddities [OBS] (`delayslots.py`)
* Control-transfer instruction in a delay slot: **0**.
* Branch targeting a delay slot: **1**. libkernel 0x26f74c `bgtz t2,0x26f704` enters the delay slot of `beqz t2` at 0x26f700. The translator handles it.
* Delay slot that overwrites a branch operand register: 1,629 (normal compiler scheduling). Branch conditions and `jr`/`jalr` targets must be evaluated before the slot runs.
* Branch-likely: 1,999 (game 109, CRI 1,412, Sony 265, MW 213).
* `j` tail calls to function starts: 1,709. `j` into code no table knew about: 75, all real tail-called functions.
* Branches into the middle of another function: 6. These are boundary-estimate artefacts, so there is no evidence of real cross-function jumps.

## 3. EE-specific instructions (histogram)

From `stats.py` and `histo.py`; full per-mnemonic list in `data/isa_histogram.md` and `data/hist.json` [OBS].

| category | game | CRI | Sony | MW | total |
|---|---|---|---|---|---|
| MMI 128-bit parallel | 220 | 835 | 334 | 116 | 1,534 |
| lq/sq | 19,407 | 76 | 307 | 865 | 20,682 |
| 64-bit integer (daddu, ld/sd, dsll32…) | 72,996 | 29,113 | 6,388 | 4,373 | 112,895 |
| EE int extras (3-operand mult, madd, SA) | 155 | 169 | 120 | 25 | 472 |
| pipeline 1 (mult1/div1/mfhi1…) | 44 | 31 | 15 | 4 | 96 |
| FPU total | **54,151** | 557 | 194 | 986 | 55,922 |
| of which EE-only FPU (adda/madd/msub/mula/madda/msuba/min/max/rsqrt) | **1,750** | 0 | 0 | 0 | 1,751 |
| COP2 / VU0 macro | 18 | 0 | 307 | 0 | 325 |
| system (syscall/sync/ei/di/cache/cop0/break) | 48 | 128 | 327 | 10 | 513 |
| branch-likely | 109 | 1,412 | 265 | 213 | 1,999 |
| all words | 536,930 | 99,728 | 26,974 | 25,940 | 689,888 |

Notes [OBS unless tagged]:
* **lq/sq**: 20,237 of the 20,682 are `sp`-relative. CodeWarrior saves callee-saved GPRs as full 128-bit values in prologues and epilogues (`sq s0,0(sp)`, as at 0x464a80). The other 445 are 16-byte struct and vector copies.
* **MMI** in game code: `pcpyld` 184, mostly building 128-bit values for `sq`; `paddub` 30; `pmfhl` 5; `pcpyud` 1. The heavy MMI users are libraries: CRI Sofdec IDCT/colour conversion (`paddsh` 192, `pmaxh`/`pminh` 144 each, `psrah` 132, `ppacb` 72, all in about 1 function) and libmpeg (`qfsrv` 42, `mtsab` 32, `paddh` 80, `psrlh` 50).
* **64-bit**: `daddu` (53,143) is mostly CodeWarrior's `move`. `ld`/`sd` save `ra` and the s-registers. `dsll32`/`dsra32` (4,039 / 1,768) build GIF/DMA tags in software: game code writes `0x1000000X` 64-bit tags with `dsll32`+`sd`, as at 0x2b7314. `ldl/ldr/sdl/sdr` (370 each) are all in library memcpy routines.
* **Three-operand `mult`**: 427 (game 154). `mult1` (3-operand form) appears 75 times, which means CodeWarrior uses pipeline-1 multiplies.
* **COP2 in game**: only 2 hand-written VU0 macro leaf functions, for example 0x2e2da0 (`lqc2 ×3; vmulabc/vmaddabc/vmaddbc; sqc2`, a matrix×vector product). All other COP2 use is in libvu0 (0x10daa8–0x10e5xx). There are 0 `vcallms`/`vcallmsr` [OBS]. The micro-mode VU0 and VU1 microcode is out of scope here (see hardware.md).
* **FPU-heavy game code [OBS]**: 10.1 % of game instructions are COP1. That includes 2,421 `mul.s`, 1,975 `sub.s`, 1,700 `add.s` and 835 `div.s`, plus about 3,550 compares (`c.lt`/`c.le`/`c.eq`) feeding `bc1t`/`bc1f`, **653 `madd.s`, 622 `adda.s`, 215 `mula.s` and 181 `msub.s`** (CodeWarrior uses the ACC register for dot products, as in 0x1f17f0), 152 `sqrt.s`, and 752 `cvt.w.s`. There are 0 `rsqrt.s` and 0 `min.s`/`max.s`. There are no double-precision ops, because the EE FPU has none.
* **FPU semantics a translator must model [INF/known EE behaviour]:**
  * no Inf, NaN or denormals: results saturate to ±FLT_MAX and denormals flush to ±0;
  * rounding toward zero;
  * `div.s` by 0 gives ±FLT_MAX with a flag set;
  * `sqrt.s`/`rsqrt.s` use |x|;
  * `cvt.w.s` truncates and saturates;
  * `madd.s`/`msub.s` round the intermediate product ACC+fs·ft differently from IEEE fused or separate operations (PCSX2 needs per-game "FPU mul hack" settings).

  `fpu_clamp()` in `ee_runtime.h` implements the clamping. Round-to-zero is not modelled in the PoC: it needs `fesetround(FE_TOWARDZERO)` or SSE MXCSR RZ, plus DAZ/FTZ, which is cheap on x86.

## 4. Proof of concept: `rec.py` static translator

Files:
* `poc/rec.py`: the translator.
* `poc/ee_runtime.h`: context, memory accessors and EE arithmetic helpers.
* `poc/poc_funcs.c`: generated output.
* `poc/harness.c`: test harness.

Design:
* `Ctx` holds 32 × 128-bit GPRs, HI/LO plus HI1/LO1, SA, 32 FPRs, the ACC and the condition flag.
* Every instruction gets a label `L_addr` and one C statement or block.
* 32-bit ops sign-extend into the low 64 bits and leave the upper 64 bits unchanged, as on the EE.
* Branches: the condition is computed into a temp, the delay slot is emitted, then `goto`. Branch-likely executes the slot only on the taken path.
* `jal` becomes a direct C call when the callee is in the same unit, otherwise `rec_call()`.
* `jalr` and virtual `jr t9` capture the target before the slot and then call `rec_call()`.
* `jr` with a recovered switch table becomes a C `switch` that jumps to labels.
* An entry into a delay slot becomes `if (0) { L_x: slot; }`.
* Unsupported opcodes become `rec_unimpl()`, which in a real project would be an interpreter fallback.

Translated functions (all game code except where noted):

| addr | why picked | test (`harness.c`) | result |
|---|---|---|---|
| 0x1f17f0 | FPU ACC math (`adda.s`/`madd.s`): out = a + b·s | (1,2,3)+(4,5,6)·0.5 = (3,4.5,6) | pass |
| 0x25c770 | FPU-heavy: quaternion to 4×4 matrix (15 `mul.s`, 3 `adda.s`, 3 `madd.s`) | q=(1,2,3,4)/√30 against a double-precision formula | pass, max error 4.4e-8 |
| 0x1fbf70 | **jump table** (0x456420, 11 entries) | all 256 inputs against the table read by hand (0xF0→0, 0xF1→7 … 0xFA→16, otherwise (v+1)&0xFF) | pass (256/256) |
| 0x31e9a0 | **MMI `pcpyld`** + `sd`/`sh`/`sb` struct init | byte-exact layout, plus the rule that the upper 64 bits of the 128-bit GPR are preserved by a later `addiu` | pass |
| 0x169420 | **`jalr` C++ virtual call** (vtable slot 0x2c) | fake object/vtable, host stub returns 5, expected 5<<11; `this`, `sp` and `ra` are checked | pass |
| 0x2b64f0 | delay-slot load in a taken branch | flag 0/1 | pass |
| 0x1f0160 | **branch-likely (`beql`)** + loops; bitmask to 40 records | against a separate C reference written from the asm | pass |

Results [OBS]:
* `gcc -O2 -Wall -Wextra` and `clang -O2` both build it, and **268/268 checks pass**.
* Generated code sample: `L_169430:; { uint32_t t = ctx->r[25].u32[0]; ctx->r[31].u64[0] = 0x169438; rec_call(ctx, t); }`
* **Scaling test [OBS]:** `bulk.py` translated **the whole `.text`**, 10,710 functions and 689,888 instructions, into 11 C files totalling about 62 MB, in about 14 s. All 11 files compile with `gcc -O1` in about 50 s on 2 cores and produce 13 MB of objects. This shows only that the code builds; it is not a correctness claim.

Opcode coverage of `rec.py` [OBS] (`coverage.py`): **688,515 / 689,888 = 99.80 %** of instruction words, and 140 of the 191 distinct mnemonics.

| part | covered |
|---|---|
| game | 99.997 % |
| MW runtime | 99.93 % |
| CRI | 99.36 % |
| Sony | 97.53 % |

What remains (1,373 words):
* MMI halfword/byte ops (`paddsh`, `pmaxh`, `pminh`, `psrah`, `paddh`, `ppacb`, `psrlh`, `qfsrv`, `pextlb`, `padduw`, `pextub`, `mtsab`…), about 1,100 words, all in CRI Sofdec, libmpeg and MSL.
* COP2 macro instructions (about 300): libvu0, plus the 2 game functions above.
* COP0 (`mfc0`/`mtc0`/`eret`, 48): libkernel.

In functions, `rec_unimpl` sites fall in 26 COP2 functions, 45 MMI/COP0 functions and 106 privileged or syscall functions. Nearly all of them are library code that would be replaced by HLE.

## 5. Assessment

[INF] Compared with typical PS2 titles, **this binary is a better-than-average static-recompilation candidate on ISA grounds**, but it has one large structural peculiarity, C++ dispatch:
* **Easy:** the code region is single and contiguous with no overlays, SMC or disc code. Game code has no syscalls and almost no MMI or COP2. 99.997 % of game instructions are plain MIPS-III, FPU or `lq`/`sq`. Delay-slot oddities are essentially absent. Switch tables are compiler-standard and fully recoverable. The compiler (CodeWarrior 2.4) is consistent.
* **Main obstacles:**
  1. **Pervasive C++ virtual dispatch.** There are 8,918 virtual `jalr`, 165 virtual tail calls, 584 vtables and about 3,370 vtable-only functions that Ghidra does not find. Every indirect call needs a runtime address→function lookup, and function discovery must be seeded from data pointers, including the 29 this-adjust thunks and the 143 sinit functions at 0x464480. This is mechanical work, but it decides whether the build is complete.
  2. **EE FPU semantics** in about 54 k FPU instructions. Clamping, round-to-zero and ACC (`madd`/`msub`) must be bit-accurate for physics and animation, or results drift from the PS2. This is cheap on x86 with RZ+FTZ+DAZ plus clamping on overflow paths, but every FPU op is affected.
  3. **Middleware and threads.** CRI ADX/Sofdec (2,776 functions, 6 threads, semaphores, VSync and IPU interrupt handlers) and Sony libkernel, libcdvd, libmc, libpad2, libdbc, SIF and libmpeg should be HLE-replaced rather than recompiled. Examples: ADX decode on the host, SFD to an FFmpeg/MPEG-1/2 decoder, file I/O against an extracted DATA.CVM. Recompiling them would pull in IOP RPC, the IPU, DMA and the thread scheduler.
  4. GS/VIF/VU rendering paths are **not** assessed here; see hardware.md. The 64-bit `dsll32`/`sd` DMA-tag building in game code means the game produces its own DMA chains, so a GS/VIF backend is unavoidable.
* **Functions needing manual attention [INF]:**
  * about **10 game functions**: 2 VU0-macro functions, 3 switch tables with a hoisted bound, 2 interrupt handlers and the thread/semaphore setup at 0x220150, plus a few `ei`/`sync` helpers;
  * about **200–220 library functions** that touch hardware, syscalls, COP0, COP2 or MMI (out of 222 counted). They are better handled by replacing the whole library: about 3,300 library functions are HLE or replace candidates;
  * **plus** an unbounded but typically small number of game functions whose behaviour depends on PS2 timing or hardware data (DMA packet builders, VSync waits). Estimated at 50–150 [UNK], to be confirmed with hardware.md.

  In short: about 7,000 game functions should translate automatically, about 1 % need manual work, and the middleware is replaced.

## Open items
* [UNK] Exact library boundaries (±few functions) and the libmpeg version. Fix: match against the Sony SDK 3.0 `.a` files if available (about 0.5 day).
* [UNK] Whether `.PAC` contents are compressed. Code was ruled out by signature scan and by the absence of `FlushCache(2)`. Decompressing would make the check absolute (see stage_load_pac 0x120720).
* [UNK] 436 Ghidra-only function starts that my heuristics did not reproduce, mostly functions reached by `b`. They are harmless, since the union is used.
* [UNK] Correctness of the bulk translation beyond the 7 tested functions. The next step is a differential test against PCSX2 traces, at about 1–2 weeks for a harness.
* [UNK] Whether the game ever throws C++ exceptions. If it does, the MW EH unwinder (0x100xxx, longjmp at 0x102320) needs a host implementation.

## Files (section A)
`poc/`: r5900.py, consts.py, funcscan.py, jumptables.py, indirect.py, classify.py, libmap.py, libruns.py, callmap.py, stats.py, histo.py, syscalls.py, delayslots.py, scan_disc_code.py, rec.py, bulk.py, coverage.py, ee_runtime.h, harness.c, poc_funcs.c, ExportFuncs.java; `poc/data/`: ghfuncs.tsv, mystarts.txt, jumptables.json, hist.json, comp_table.md, isa_histogram.md, syscalls.md.
Reproduce: `cd feasibility/poc && SCRATCH=data python3 rec.py poc_funcs.c 0x1f17f0 0x25c770 0x1fbf70 0x31e9a0 0x169420 0x2b64f0 0x1f0160 && gcc -O2 poc_funcs.c harness.c -lm -o /tmp/h && /tmp/h ../../iso/SLUS_210.75`
