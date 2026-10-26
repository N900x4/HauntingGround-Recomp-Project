# Launch checklist: release 0.1 (Windows + Linux, HD textures)

0.1 is the first public release (the user, 2026-10-26: "Its 0.1 not 0.2"). The 0.1 build of 2026-10-15 (Windows only,
no textures) was never published. Nothing here is pushed, uploaded or posted without the user's go-ahead.

Who: **C** = a Claude session can do it alone; **U** = needs the user (a decision, a password, a Windows boot, an
account).

## 1. The downloads

| # | Step | Who | State |
|---|---|---|---|
| 1.1 | Linux archive `HauntingGround-Recomp-0.1-linux-x64.tar.gz` (`tools/release/mkrelease_linux.py`, from the release commit) | C | built from 94391f3: 1,633.7 MB, SHA-256 `e50f87096e927847046009d8e63ddde4536b1c53f2935b54b887f560a5592c91` |
| 1.2 | Linux: unpack into an empty folder, first start with the ISO in batch mode, frames identical to hashA, ISO untouched | C | done 2026-10-26 on the 0.1 archive: first start 64 s, ISO untouched, 706/706 frames identical to hashA |
| 1.3 | The Windows drive: boot Windows ~15 min, **or** mount it read-only on Linux (`udisksctl mount -b /dev/nvme0n1p3 -o ro`, needs the password) | U | waiting |
| 1.4 | Windows zip `HauntingGround-Recomp-0.1-win64.zip` (`mkrelease.py --textures D:/ps2-work/texwork/release_textures`) | C (on Windows or with the drive) | waiting for 1.3 |
| 1.5 | Windows: first start from an empty folder (real Windows, or Wine as a first check), frames identical to hashA | C, then U on real Windows | waiting for 1.4 |
| 1.6 | Both files under 2 GB (GitHub's limit per file); sizes and SHA-256 into `github-release-0.1.md` | C | Linux done, Windows waiting |

## 2. A last playtest of the final downloads

| # | Step | Who | State |
|---|---|---|---|
| 2.1 | Linux download: the first start as a player would do it (file picker, progress window), 10-15 minutes of play through Debilitas; the HD textures row on/off | U | waiting |
| 2.2 | The same on Windows (can be shortened if 1.5 ran on real Windows) | U | waiting |
| 2.3 | The F9 marks of playtests 6, 6b and 7: what they were (or "nothing") | U | waiting |
| 2.4 | Hand checklists, optional for 0.1: Options menu (`docs/options-menu.md`), DualSense (`docs/controller.md`) | U | open; the README says controllers were tested by automation only |

## 3. Paperwork

| # | Step | Who | State |
|---|---|---|---|
| 3.1 | Licence: GPL-3.0 (`LICENSE` is GPL-3.0) | U to confirm | leaning GPL-3.0 |
| 3.2 | Compiler sources (GPL): the notices point to MSYS2's and Bootlin's source archives and offer a copy on request. Safer: attach those source archives to the GitHub release too (a download of a few hundred MB, the user's permission) | U decides | open |
| 3.3 | The HD textures in the downloads: the risk was explained once; the user's decision stands. The texts say plainly what they are | done | |
| 3.4 | The user's ISO against redump (`SLUS-21075`): the checksums the setup accepts should be redump's | U (or C with permission to look it up) | open |

## 4. Publishing (only on the user's word)

| # | Step | Who |
|---|---|---|
| 4.1 | Create the GitHub repository (the user's account; public), set the description and topics | U |
| 4.2 | Replace `<REPO URL>` in `docs/release/github-release-0.1.md` and `reddit-posts.md` | C |
| 4.3 | Push `main` (the local history only holds code and docs; check once more that nothing game-derived is tracked: `git ls-files` against `recomp/gen`, `*.iso`, `*.elf`, `*.irx`, `*.dds`, `*.png` from texwork) | C checks, U pushes or says go |
| 4.4 | Tag `v0.1` on the release commit; the release page from `github-release-0.1.md` with both downloads (and the source archives of 3.2) | U or C on the user's word |
| 4.5 | Download both files back from GitHub once and compare the SHA-256 | C |
| 4.6 | Reddit posts (`reddit-posts.md`): check each subreddit's rules first | U |

## 5. After the launch

- Watch the issues for the first reports; ask for `logs/play.log`, `logs/launcher.log` and the newest recording.
- A Nexus page for the texture pack (planned for 1.0).
- Roadmap: asynchronous interpolation (the real fix for heavy scenes), the HD font, pop-in.
