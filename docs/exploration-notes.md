# Playing further: exploration tools and findings (2026-10-13)

## Tools

| Switch | What it does |
|---|---|
| `HG_EXPLORE=vb` | From that VBlank, an explorer drives Fiona. It holds a direction until her position stops changing, then turns (pseudo-random), taps Cross every 2 s and logs her position every second. |
| `HG_RAMDUMP=dir:vb,vb,...` | Writes EE RAM at those VBlanks. |
| `HG_PIXWATCH=x,y,vb` | From VBlank `vb` on, logs every primitive writing that pixel (scalar renderer, so set `HG_GSSIMD=0 HG_TRISIMD=0`). Also logs triangles over the pixel that were kicked with ADC (not drawn). |
| `HG_BIGTRI=1` / `=s` | Logs, or skips, triangles larger than 1500 pixels. |
| `HG_CLIPLOG=1` | Logs VU1 CLIP operands. |
| `HG_VUFLAGDELAY=4` | VU1 MAC/status/clip flags become visible 4 instructions after the FMAC instruction, as the hardware's FMAC pipeline delivers them. The default 0 keeps them immediate, because stalls are not modelled. |

- **Fiona's position:** the player object is `*(0x44f818)` (0x1510c80 in these runs), and its position vector is at
  +0x10 (x, y, z, 1). Found by diffing RAM dumps while walking and standing.
- **Which areas load:** the DATA.CVM files read during a run, mapped from the log's CVM offsets
  (`cdfiles.py` in the session scratchpad).

## Findings

- **Areas reached** (with `HG_MOVIES=skip` and the usual pad script):
  - Fiona leaves the first area (`ST_02A`) for `ST_013` (about VBlank 3700); `ST_00D` was reached by hand at about 6000.
  - Her monologue lines appear on the way ("I just want to put the terrible things I saw back there behind me.",
    "I am not going back.").
  - 20,000 VBlanks of exploration ran without a crash or deadlock.
- **The brown diagonal in the first room is real geometry:** many shaded triangles, very likely the fallen beam from
  the opening. Its exact look needs the reference.
- **Suspect: dark wedge-shaped holes in the ground near the camera** (`ST_013`). The floor triangles there are never
  drawn: the game's VU1 program kicks them with ADC (no drawing) after clip-flag tests. Its CLIP compares each vertex
  with its own w, and the culled triangles are the nearest ones, with z close to −w (the near plane). So the culling
  follows the game's own code as emulated. Whether the console shows the same holes, or the camera normally never
  gets that close, needs the PCSX2 reference.
  - The game's VU1 programs read only clip flags (`FCAND`, `FCSET`).
  - Delaying flags by 4 instructions does not change the frames.
- **Reverb:** no area reached changes the reverb depth. Only the stage-init calls (depth 0) were seen.
- **Memory card:** the boot check creates nothing. Saving needs an in-game save point, which the explorer has not
  reached. Story progress (doors, items, events) is beyond a wall-following explorer.

## Where this stopped (2026-10-13, at the user's request)

- **Save point (from the user):** the first save point is the **clock in the room after Fiona changes from the towel
  into her normal outfit**. Reaching it is the way to test memory-card save/reload and to look for a room that
  turns reverb on.
- **Explorer change:** when blocked, it now presses Cross before turning (doors open with Cross).
  `HG_EXPLORE_SEED` picks the random route.
- **Last runs:** two seeds, stopped at about VBlank 6000 when the user asked to stop. Both reached `ST_013` and
  `ST_00D` (seed 2 went back to `ST_013`). Neither reached the outfit change.
- **Next:** script a route to the outfit-change room and the clock, by watching the area loads and Fiona's
  position. Once real-time play works, that can also be done by hand. Then:
  - save at the clock;
  - check the files written under `HG_MEMCARD_DIR`;
  - reload from LOAD GAME;
  - watch reverb depth there (`HG_TRACE=20ef50`, `HG_SPUREGLOG=78c-78e`).
