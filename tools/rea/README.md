# Reverse engineering with rea and Ghidra (Emotion Engine)

[rea](https://github.com/morluto/rea) (MIT) gives Claude Code Ghidra's decompiler, references and renaming as MCP tools and
a CLI. Out of the box it cannot analyse the PS2's CPU; this is the setup that works (Linux, 2026-10-25). Everything is
in the user's home folder; nothing system-wide changes.

| Piece | Where | Why |
|---|---|---|
| Node.js 24.22.1 | `~/.local/opt/node-v24.22.1-linux-x64` | rea 6.7 needs Node 22.x, 24.x or >= 26.12.1 |
| rea-agents 6.7.0 | `~/.local/opt/rea` (`npm install --global --prefix`) | |
| Ghidra 12.1.4 PUBLIC (official zip, sha256 checked) | `~/.local/opt/ghidra-ee` | rea's verified build; user-owned so the extension can go inside it |
| ghidra-emotionengine-reloaded v2.1.38 (chaoticgd) | `~/.local/opt/ghidra-ee/Ghidra/Extensions` | adds the R5900 language `r5900:LE:32:default` (MMI, 128-bit loads/stores, COP2) |
| `~/.local/bin/rea` | wrapper: Node 24, `GHIDRA_INSTALL_DIR=~/.local/opt/ghidra-ee` | |
| `~/.local/bin/rea-ps2` | `rea` + `REA_GHIDRA_LANGUAGE_ID=r5900:LE:32:default`, 30-minute startup, 8 GB heap | |
| MCP servers `rea`, `rea-ps2` | `claude mcp add --scope user ...` | `rea-ps2` also sets `REA_GHIDRA_SEED_FILE` |

Two things that do not work, and why:
- **The extension in the user's Ghidra folder** (`~/.config/ghidra/.../Extensions`), or a symlink overlay of a
  root-owned Ghidra: rea runs Ghidra with its own `user.home` and only scans `GHIDRA_INSTALL_DIR/Ghidra/Extensions`, and
  Ghidra resolves its real install path ("Unsupported language: r5900:LE:32:default").
- **The game's ELF as it is**: rea refuses MIPS ELFs that name a machine variant ("Machine-specific MIPS variants ... need
  a separately supported processor profile"; the PS2's e_flags are `0x20924000`). `ps2elf_for_rea.py OUTDIR` writes a
  copy declaring plain MIPS32 (`0x50000001`) and a seed file of every function start; the language override then
  selects the R5900. The copy is game data: keep it in scratch.

Use:
```sh
python3 tools/rea/ps2elf_for_rea.py ../scratch/rea
REA_GHIDRA_SEED_FILE=$PWD/../scratch/rea/hg_seeds.tsv rea-ps2 decompile $PWD/../scratch/rea/SLUS_210.75.elf --procedure=0x1bfec0
```
Each CLI call imports and analyses the program again (70-110 s). For many queries use one MCP session (the `rea-ps2`
server in Claude Code), where Ghidra stays open. The seed file matters: Ghidra alone misses about 3,370 functions that are
reached only through vtables.
