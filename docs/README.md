# Docs

Findings from the October 2026 feasibility studies. Claims are tagged [OBS] (observed), [INF] (inferred) and [UNK] (unknown). File paths inside these notes refer to the original research layout (`feasibility/...`), not to this repository.

| File | What it covers |
|---|---|
| [boot-test-report.md](boot-test-report.md) | The recompiled game running natively: how far it gets, every stop and fix, syscalls, IOP RPCs, and the files it reads |
| [csharp-vs-c-report.md](csharp-vs-c-report.md) | C vs C# as the output language, measured: speed, build time, memory, JIT |
| [executable-structure.md](executable-structure.md) | Code size, functions, compiler (CodeWarrior 2.4), libraries, recompilation hazards |
| [hardware.md](hardware.md) | VU1 programs, GS usage and post-effects, IOP modules, timing (30 fps logic) |
| [audio.md](audio.md) | Sound bank formats, Hewie's sound IDs and samples, the replacement plan |
| [textures.md](textures.md) | Texture formats, the upload path, the replacement plan |
| [handoff.md](handoff.md) | **Start here when resuming:** current state, rules, next steps, verification scripts |

## Verdict (summary)

| Outcome | Estimated chance |
|---|---|
| Tool turns your disc into a native executable that boots | ~90 % (85–95) |
| That executable reaches the title screen | ~80 % (70–90) |
| Playable start to finish, runtime reusing PCSX2 parts | ~40 % (25–55) |
| Playable start to finish, runtime written from scratch | ~20 % (10–35) |

The recompiler is the proven part. The open work is the hardware runtime: graphics, sound, controller and video.
