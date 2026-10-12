# Boot-gate experiment: does the bulk-recompiled SLUS_210.75 actually execute?

Date 2026-10-12. Tags: [OBS] observed, [INF] inferred, [UNK] unknown. Paths relative to `feasibility/recomp_tool/bootgate/`.
(Saved by the main session from the worker's report; lightly condensed, numbers unchanged.)

## Answer
Yes [OBS]. The recompiled C runs natively from entry 0x100008 through crt0 + libkernel init → 143 static constructors → `main` (0x20d860) → SIF/RPC init + IOP module loading → CRI ADX/ROFS init + mount of `CDV:DATA.CVM` → file loads → the game's frame loop, which builds and kicks real GIF/VIF1 DMA chains at ~26–30 fps.

**Furthest point [OBS]:** the boot notice/logo sequence object (table of C++ pointer-to-member steps at 0x44ae60, run by 0x380510).
- No input: waits forever in step 0 (0x380410); screen drawn by 0x37f510; waits for pad bit 14 in the word at 0x47e37c (flag byte 0x47e360 = 0 makes the press required).
- Simulated presses (`HG_PRESS`): passes steps 0–2, loads `SUBSCR/MSG_BASE.BIN/.TEX`, `SUBSCR/MSG_SUB.BIN`, `SYSTEM/PROG.TEX` (9,546 sectors), then stays in step 3 (0x37fe50); presses have no effect.

| run | distinct functions entered | guest calls |
|---|---|---|
| 70 s, simulated presses | 1,104 of 11,499 (9.6 %) | 904 M |
| 60–90 s, no input | 1,049 | ~65–69 M |

Return-address check after every call: 0 mismatches, 0 bad jump-table targets over ~1 billion calls [OBS]. Up to ~620 M guest calls/s in tight loops, single host thread.

## What was built
| file | lines | role |
|---|---|---|
| `rec2.py` | 285 | Copy of `poc/rec.py` + fixes; `REC_ENTER` counter/hook, return-address check, `REC_POLL` safe points on back-edges, reachability-based bodies |
| `bulk2.py` | 38 | All starts (mystarts.txt + 646 in extra_starts.txt + 143 sinit ctors from 0x469460) → 16 C files + sorted dispatch table |
| `ee_runtime.h` | 146 | Context (+VU0 state, `ret`); 0–32 MB fast path, slow path otherwise |
| `runtime.c` | 212 | 32 MB RAM + 16 KB scratchpad; ELF load; KSEG/mirrors; trapped EE/GS/VU windows; dispatcher; SIGPROF profiler; 60 Hz VBlank; time-limit stop with guest backtrace; `HG_TRACE`/`HG_PEEK`/`HG_PRESS`; 8 MB log cap |
| `hle.c` | 489 | EE kernel HLE (syscalls, priority threads as ucontext fibers, semaphores, sleep/wakeup/suspend, INTC/DMAC delivery, `SetSyscall` handlers, fake syscall table); IOP SIF command HLE (INIT_CMD, CHANGE_SADDR, SET_SREG, RPC bind/call/END, GETOTHERDATA) replying through the game's SIF0 handler; LOADFILE; CDVD SearchFile/Read/DiskReady/S_CMD from `iso/DATA.CVM` at virtual LSN 0x1000 |
| `interp.c` | 154 | Interpreter fallbacks: MMI half/byte ops, COP0 (Status/EPC/ErrorEPC/Count, `eret`), VU0 macro mode (~40 vector ops) |
| `hle.h`, `Makefile` | 36 | |
| `scan_codeptrs.py`, `scan_gaps.py` | 64 | Extra function-start discovery |
| `isomap.py` | 36 | Maps logged CD reads to DATA.CVM file names |

Sizes: `gen/` 62 MB C, `obj/` 30 MB, `hgboot` 19 MB. Logs `run1..run30.log`; first-entry order in `funcs_entered*.txt`.

## Reproduce
```sh
cd feasibility/recomp_tool/bootgate
python3 scan_codeptrs.py && python3 scan_gaps.py      # writes extra_starts.txt (already present)
python3 bulk2.py gen 16                               # 15–40 s; 11,499 functions; 0 rec_unimpl sites
make -j2                                              # 1:19–3:03 wall at -O1 (113–120 s CPU, shared CPU)
./hgboot -q -t 60 > run.log
HG_PRESS=600,900,1200,1500 ./hgboot -q -t 70 > run_press.log
python3 isomap.py run.log
```
Optional: `HG_TRACE=addr,...`, `HG_PEEK=addr,...`, `HG_DUMPSID=sid`.

## Stops in order [OBS]
| # | stop | cause | fix |
|---|---|---|---|
| 1 | Busy loop in syscall 0x83 | libkernel installs own syscall handlers (0x83→0x276380, 0x5a→0x276348) and searches kernel RAM for the syscall table | Run user handlers like the kernel; fake table at phys 0x10000; 0x5b reads it |
| 2 | `rec_unimpl` pcpyh at 0x115d4c | translator gap | MMI/COP0/COP2 interpreter fallback |
| 3 | Loop at 0x276260 | `di` loop polls Status.EIE; `ei`/`di` were no-ops | Model Status.EIE |
| 4 | `eret` at 0x276b34 | `mtc0 ErrorEPC=ra; eret` as privileged return | `eret` → EPC/ErrorEPC |
| 5 | Call into middle of 0x20e7d0 | ctor pointer built with lui/addiu missing from starts | `scan_codeptrs.py` (+42) |
| 6 | `sceSifInitRpc` waits for sreg 0 | needs IOP answers | SIF command HLE |
| 7 | LoadModule retry loop | LOADFILE version check expects "3000" | Return "3000" |
| 8 | Call into middle of 0x1df068 | missing starts | `scan_gaps.py` (+604) + reachability bodies |
| 9 | `while (cvFsLoadDirInfo(".")) ;` at 0x16c6c0 | no VBlank → CRI threads never ran; CD returned no data | 60 Hz VBlank + preemption at safe points; CD stub (~40 lines) |
| 10 | WaitVSync 0x1bed00 | branch-only poll loop, no safe point | `REC_POLL()` on backward branches |
| 11 | Step 0x380410 waits for pad | no pad input (libpad2/libdbc over SIO2; DBCMAN sid 0x80001300) | Not fixed; `HG_PRESS` |
| 12 | Step 3 (0x37fe50) never finishes | [UNK]; candidates [INF]: memory-card result (only `sceMcInit` answered; libmc 0x1114a8 polled every frame) or ADX/Sofdec logo playback (IOP ADX server sid 0x90000200) | Stopped at user's request |

## Translator bugs and gaps found
1. Function extent "until next start" truncated/split functions → now reachability-based; shared tails duplicated; fall-through into another start = tail call [OBS].
2. `ei`/`di` no-ops [OBS]. 3. `eret` unimplemented [OBS].
4. Missed starts: 42 code-built pointers + 604 pointer-only functions → 11,499 functions (was 10,710) [OBS].
5. No interrupt safe points in call-free polling loops [OBS].
6. Missing semantics (padduw, mtsah/mtsab, maddu, pcpyh, ~15 more MMI, COP2 macro, COP0) → interpreter fallbacks [OBS].
7. Arithmetic/FPU correctness not checked by these runs [UNK]; FPU round-to-zero not modelled.

## Syscalls (70 s run, presses) [OBS]
ChangeThreadPriority 127,774 · GetThreadId 127,770 · ReferThreadStatus 131,977 · SuspendThread 68,086 · SleepThread 8,401 · iGetThreadId/iReferThreadStatus/iWakeupThread 8,398 each · FlushCache 7,268 · sceSifSetDma 1,079 · isceSifSetDChain 1,029 · iSignalSema 986 · PollSema 890 · CreateSema/DeleteSema/WaitSema/SignalSema 700/690/692/595 · sceSifDmaStat 45 · SetSyscall 21 · GetEntryAddress 13 · CreateThread/StartThread 6/6 · AddIntcHandler 4 · AddDmacHandler 2 · Enable/Disable Intc/Dmac 3/2/1 · Deci2Call 6 · sceSifSetReg/GetReg 7/6 · 0x5a 4 · 0x83 2 · EndOfHeap 8 · SetupThread/SetupHeap 1/1 · Get/SetOsdConfigParam 2/2 · SetGsCrt 1 (interlaced NTSC) · GsPutIMR 1 · sceSifStopDma 1.
Threads: main, libkernel top thread (prio 0), 4 CRI ADX threads (8/16/18/25), game thread 0x21f210 (10). 12,604 switches in 70 s; VBlank 60 Hz.

## IOP modules, RPC servers, files [OBS]
Modules via LOADFILE from `cdrom0:\MODULES\` after IOP reset with IOPRP300.IMG: SIO2MAN, SIO2D, DBCMAN, LIBSD, CRI_ADXI, MODHSYN, MODMIDI, MODMSIN, SNDDRV, DS2O_S1, MCMAN, MCSERV.

| sid | server | calls |
|---|---|---|
| 0x80000003 | IOP heap | 20 |
| 0x80000592 | CDVD init | 2 |
| 0x80000593 | CDVD S_CMD | 297 |
| 0x80000595 | CDVD N_CMD (read) | 592 |
| 0x80000597 | CDVD SearchFile | 1 |
| 0x80000006 | LOADFILE | 14 |
| 0x80001300 | DBCMAN | 1 |
| 0x90000200 | CRI ADX | 37 |
| 0x77777777 | SNDDRV | 8 |
| 0x77777778 | SNDDRV status | 41 |
| 0x80000400 | MCSERV | 1 |

Only LOADFILE and CDVD replies are meaningful; others get a generic "result = 1" stub (wrong for ADX, SNDDRV, DBC, MC) [INF].
Files read: C_0000.HD/SDT/BD, SYSTEM/ERRMES.TEX, GAME_FIX.TEX/.GFM, SYSTEM/LOGO_CRI.BIN; with presses SUBSCR/MSG_BASE.BIN/.TEX, SUBSCR/MSG_SUB.BIN, SYSTEM/PROG.TEX. 296 read calls, 10,452 sectors.
Order touched: kernel patches/alarms → SIF/RPC → CDVD → IOP reset + modules → ROFS mount → SNDDRV → memory card (init only) → GS setup → file loads → frame loop.

## Hardware registers [OBS]
82 distinct addresses, no unmapped accesses: timers T0/T1/T3; IPU 0x10002000/2010; GIF 0x10003000/3020; VIF1 0x10003c00–3c20; VIF FIFOs 0x10005000/5008; IPU FIFO 0x10007010/7018; DMA ch 0–4, 6, 9; DMAC 0x1000e000–e050; SBUS 0x1000f520/f590; GS PMODE, SMODE2, DISPFB2, DISPLAY2, BGCOLOR, CSR. DMA kicks in 70 s: VIF1 1,810, GIF 5,449 — completed instantly, never executed (nothing rendered).

## What remains [INF]
**(a) Title screen:** pad HLE (1–2 days); memory-card HLE against a host dir (2–4 days); CRI ADX IOP server sid 0x90000200 or EE-level ADXT/ADXF/MWSFD replacement (1–2 weeks, largest); Sofdec/IPU movies or report "finished" (~1 week); GS 2D backend (1–2 weeks); SNDDRV stub = silence, real audio needs LIBSD/SPU2 HLE (2+ weeks).
**(b) First controllable room:** (a) + VIF1 unpack and 19 VU1 programs (~2 kLOC); full GS 3D (textures, CLUT, alpha, Z); audio via ADX/SNDDRV; streaming loads (CD path works); FPU round-to-zero. Roughly 6–10 engineer-weeks on top of this runtime. The CPU side is largely solved.
