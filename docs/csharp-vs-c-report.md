# C# as the output language for the Haunting Ground static recompiler: measurements against C

Date: 2026-10-12. Target: SLUS_210.75, 10,710 functions, 689,886 instructions translated.
Machine: 2 vCPUs, 8 GB RAM, shared and heavily loaded (load average 4–6). Wall-clock times are inflated and noisy; benchmark numbers are the minimum of 5 interleaved rounds.
Tags: [OBS] measured here; [INF] inferred; [UNK] not established.
Toolchains: .NET SDK 6.0.400 / runtime 6.0.8 (system); .NET SDK 8.0.425 user-local in `~/.cache/hg-dotnet/sdk8`; gcc 16; system clang.

## 1. Results

| metric | C (rec.py / bulk.py) | C# (rec_cs.py / bulk_cs.py) |
|---|---|---|
| Generation, whole `.text` | 17.9 s [OBS] | 20.3–23.2 s (15.5 s CPU) [OBS] |
| Source size | 59.1 MB, 11 files [OBS] | 34.6 MB, 11 files (only referenced labels) [OBS] |
| Whole-program build | gcc -O0 19.7 s · -O1 54.2 s · -O2 166.8 s · clang -O1 53.1 s · -O2 53.9 s (2 jobs) [OBS] | Roslyn Release .NET 6: 42–46 s (one process); .NET 8: 66 s under heavier load [OBS] |
| Peak RAM during build | 0.85–0.94 GB with 2 gcc jobs; 0.7 GB clang [OBS] | 3.87 GB in one process (.NET 6), 3.5 GB (.NET 8); ~120 MB per MB of source; 1 chunk 636 MB [OBS] |
| Output size | unlinked code -O0 18.0 MB, -O1/-O2 10.0 MB, clang -O2 9.8 MB [OBS] | IL 16.2 MB; ReadyToRun 32.2 MB; NativeAOT exe 16.0 MB [OBS] |
| Extra AOT step | none | ReadyToRun +13.4 s (.NET 6); NativeAOT +9.9 s + 0.3 s link (.NET 8) [OBS] |
| JIT of all 10,722 methods | 0 | .NET 6 5.75 s tiered / 7.3 s no tiering; .NET 8 4.1 s / 9.6 s; ReadyToRun 0.09 s; NativeAOT 0 [OBS] |
| Methods left unoptimised (MinOpts) | n/a | 7, IL 61–135 KB (largest f_002029b0, 5,552 instructions) [OBS] |
| Hot loop, 7 PoC functions (ns/pass) | gcc -O2 121.3; clang -O2 101.7 [OBS] | .NET 6 tiered 201.6 · PGO 199.4 · no tiering 189.4 · R2R 192.8 · .NET 8 tiered+PGO 172.8 · no tiering 192.0 · R2R 159.1 · NativeAOT 175.0 [OBS] |
| Time vs gcc -O2 (sum / geomean) | 1.00 / 1.00 (clang 0.84 / 0.86) | 1.31–1.66 / 1.30–1.62 [OBS] |
| Harness checks | 268/268 [OBS] | 268/268 on .NET 6 JIT, .NET 8 JIT, both R2R, .NET 8 NativeAOT [OBS] |

Per-function numbers: `bench/bench_results.md`.

## 2. What was built (`feasibility/recomp_tool/csharp/`)
- `rec_cs.py`: C# backend reusing `poc/rec.py`'s scanner and the `r5900` decoder. One `unsafe static class` per 0x40000 chunk; one `static void f_XXXXXXXX(Ctx* ctx)` per function; explicit-layout `Ctx`/`GPR` structs; 32 MB unmanaged RAM via `NativeMemory`; `goto` labels; jump tables as `switch`+`goto`; indirect calls via `Runtime.Call()` (sorted `delegate*` table, binary search, host fallback); direct calls across chunks.
- `runtime/Runtime.cs`: port of `ee_runtime.h`. `poc/Harness.cs`: port of `harness.c` (268 checks). `poc/Bench.cs`, `bench/bench.c`, `bench/run_bench.py`: matched hot loops.
- `bulk_cs.py` (same function list as `bulk.py`); `bulk/JitProbe.cs` (forces JIT, reports IL sizes and tiers); `emit/IlEmit.cs` (MIPS → IL via `Reflection.Emit`); `cside/gen_c.sh`, `cside/build_c.sh`; `tools/measure.py` (wall/CPU/peak RSS; kills build on low memory).

## 3. Findings
- **Correctness** [OBS]: 268/268 first build; quaternion error 4.37e-08, same as C. Direct-to-IL passes 257/257 checks for 2 functions, 7.8 ms to translate, ~177 ns/call. [INF] Saving emitted IL needs Mono.Cecil or .NET 9 `PersistedAssemblyBuilder`.
- **Build cost** [OBS]: Roslyn ≈ gcc -O1/clang time, 3.5× faster than gcc -O2, but ~4× the peak RAM in a single process; the full build only fit by swapping ~1.5 GB out. [INF] Fix: one assembly per chunk (~640 MB, 6.4 s each), cross-chunk calls through `Call()`.
- **JIT** [OBS]: 7 functions exceed the 60,000-byte IL limit → unoptimised under the runtime JIT. [INF] ReadyToRun/NativeAOT don't apply that cutoff (JIT source, not measured). [UNK] tier-1 behaviour on .NET 8.
- **Performance** [OBS]: C# 1.3–1.7× slower than gcc -O2; .NET 8 R2R best. JIT disassembly shows every `ctx->rN` stored and reloaded. Indirect calls ~7–10 ns vs ~3.5 ns in C. [INF] Caching MIPS registers in locals would narrow the gap.
- **Toolchain** [OBS]: NativeAOT works on Linux with .NET 8 + system clang, no lld or apt packages, but compiler packages come from NuGet (needs network). crossgen2 also from NuGet. .NET 6: NativeAOT experimental, no `UInt128`.

## 4. Harder / easier in C#
Harder: 128-bit GPRs need overlapping-field structs (no `UInt128` before .NET 7); bodies wrapped in `unchecked {}`; `int.MinValue / -1` and divide-by-zero throw (guarded); `uint + int` promotes to `long`; **no MXCSR/`fesetround` API — EE round-toward-zero and flush-to-zero need a native shim or software rounding** (biggest semantic gap) [INF]; no `goto` into blocks (delay-slot entry rewritten); unsafe pointers = no aliasing assumptions (correct but slow).
Easier: in-process Roslyn compile on the user's machine with no external toolchain (but ~4 GB RAM unless split); one build for Windows/Linux/macOS, x64/arm64; IL emit can skip source; managed exceptions and safety for the host/HLE side.

## 5. Recommendation
[INF] Use C (or C++) for the generated game code: 1.3–1.7× faster, native control of EE rounding/denormals, parallel build under 1 GB, no JIT cliffs. C# is viable only with .NET 8+ R2R/NativeAOT, one assembly per chunk, registers in locals, the 7 huge functions split, and a native rounding shim. Reasonable hybrid: generated C + host/HLE/tooling in C#.

## 6. Reproduction
```sh
cd feasibility/recomp_tool/csharp
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1; D8=~/.cache/hg-dotnet/sdk8/dotnet; ELF="$PWD/../../../iso/SLUS_210.75"
python3 rec_cs.py poc/PocFuncs.cs Poc 0x1f17f0 0x25c770 0x1fbf70 0x31e9a0 0x169420 0x2b64f0 0x1f0160
(cd poc && dotnet build -c Release && dotnet bin/Release/net6.0/PocHarness.dll "$ELF")
(cd poc && $D8 build -c Release -p:TF=net8.0 && DOTNET_ROOT=${D8%/dotnet} $D8 bin/Release/net8.0/PocHarness.dll "$ELF")
(cd poc && $D8 publish -c Release -p:TF=net8.0 -r linux-x64 -p:PublishAot=true -o pub/net8_aot)
(cd bench && gcc -O2 -I../../../poc ../../../poc/poc_funcs.c bench.c -lm -o bench_c_gcc_O2 && clang -O2 -I../../../poc ../../../poc/poc_funcs.c bench.c -lm -o bench_c_clang_O2 && python3 run_bench.py 5)
python3 tools/measure.py python3 bulk_cs.py bulk/gen
(cd bulk && MEASURE_MIN_AVAIL_MB=250 python3 ../tools/measure.py dotnet build -c Release -p:GenDir=gen -nodeReuse:false -clp:PerformanceSummary)
(cd bulk && dotnet bin/net6.0_gen/HgRecomp_gen.dll events)
cside/gen_c.sh; for c in "gcc -O0" "gcc -O1" "gcc -O2" "clang -O1" "clang -O2"; do python3 tools/measure.py cside/build_c.sh $c; done
(cd emit && dotnet build -c Release && dotnet bin/Release/net6.0/Emit.dll "$ELF")
```

## 7. Caveats
- ~2.5× CPU oversubscription: trust ratios over absolute times.
- The 7-function benchmark is small and call-heavy, not a whole-game workload [INF].
- A full Roslyn build needs ~4 GB free.
- Round-toward-zero not modelled on either side.
- Bulk C# output is compile-validated only; correctness beyond the 7 functions [UNK].
- No Rust results (request withdrawn; a briefly installed toolchain was removed).

(Saved by the main session from the worker's report; condensed in places, numbers unchanged.)
