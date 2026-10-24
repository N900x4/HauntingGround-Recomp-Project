# Textures — Haunting Ground (SLUS-21075) feasibility, section C

Tags: [OBS] observed directly (bytes/code), [INF] inferred, [UNK] unknown.
Tools: `poc/tex_hg.py` (parser and decoder), `poc/tex_dump.py` (PNG dump), `poc/tex_sheet.py` (contact sheets, `--csa` palette variants),
`poc/tex_survey.py` (disc-wide census -> `tex_samples/tex_index.tsv`), `poc/tex_sfd_probe.py` (Sofdec resolutions).
Samples: `tex_samples/` (environment, character, UI and palette-variant sheets, plus single PNGs).
Ghidra work was done on a scratch copy of `ghidra/`. The project itself was not touched.

## 1. Where textures live

| Container | Files | Textures (instances) | Unique (img+CLUT md5) | Texels (unique) | Notes |
|---|---|---|---|---|---|
| `.PAC` stage packs | 167 of 168 | 1625 | 989 | 64.8 M | all 256x256 PSMT8; one embedded TEX blob per PAC |
| `.TEX` (chars, items, map, UI, SUBSCR) | 229 | 326 | 297 | 25.2 M | 163 PSMT8, 163 PSMT4 |
| `SUBSCR/THUMBNAIL.TXS` | 1 | 110 | 110 | 1.8 M | gallery thumbnails, 128x128 PSMT4 TEX blobs |
| **Total TEX-format** | | **2061** | **1396** | **91.8 M** | from `tex_survey.py` |
| raw `.BIN` screens | 3 | 3 | | ~0.9 M | headerless, see below |
| `SUBSCR/ART0x.LEN` | 6 | 48 | | ~12-15 M est. | compressed gallery art, not decoded |
| `.SFD` video | 135 | — | | — | FMV, separate problem |

- [OBS] **PAC**: the header is a u32 section-offset table (ST_000.PAC: 17 entries at 0x00..0x40). Section 9 (`+0x24`, 0x142400 in ST_000.PAC) is a plain TEX blob (count=0x0E). Every PAC except `ST_060/ST_065.PAC` (2272 B, empty) has exactly one blob, holding 2..16 textures (mode 10-11). Found by scanning for valid TEX headers. Other PAC sections: section 3 (0xbd20) holds geometry with float UV pairs in [0,1] (e.g. 0xbd90..). Section 0x225d00 holds message text.
- [OBS] **PCK** (models): no TEX blobs inside (scan found 0 in 115 files). Character textures are in sibling `O_xxx/xxx_NNN.TEX`, and mesh entry field `e[7]` indexes into that TEX (FIN_000: indices 0..3).
- [OBS] **BIN**: `SYSTEM/TITLE_BACK.BIN` (143424 B) = 640x448 PSMT4 + 16-entry CLUT, with no header (decoded: `tex_samples/TITLE_BACK_BIN_*`). `LOGO_CRI.BIN` (287744 B) = 640x448 PSMT8 + 256 CLUT. [INF] `LOGO_DOLBY.BIN` (389824 B) = 720x540 PSMT8 + CLUT (size fits, not decoded). EV/DE `MARK.BIN`/`PARAMS.BIN` are not textures.
- [OBS] **ART0x.LEN**: 8-entry offset table (0x20,0x17c86,…), compressed bitstream. They are loaded by FUN_00387f00 ("SUBSCR\ART%02X.LEN", 0x463fe0). [UNK] codec not reversed (unlockable gallery art, 48 images).
- [OBS] **SFD** (Sofdec, MPEG-1, frame-rate code 5 = 30 fps): 135 files, 938 MB. Resolutions from sequence headers: **118 x 256x224** (EV*/DE* event/death clips), **4 x 512x224** (EV0002/4/5/8), **11 x 512x448** (CAPCOM, OPENING, STAFF_ROLL, LOOP_DEMO, PLAY_DEMO_0..6), 1 x 256x64, and 1 unclear (EV0021: first 0x1B3 hit is a false match). Everything is low-res, so FMV needs AI upscale or re-render separately.
- [INF] Total disc texture content is about **1400 unique textures / ~92 M texels** in TEX format, plus about 50 art images and 3 full-screen BINs. A 4x-per-axis upscale gives ~1.5 G texels (~1.5 GB as BC7, ~5.9 GB RGBA8).

## 2. Formats

TEX blob header [OBS, all 2061 entries validate with the strict size check]:
```
0x00 u32 count, 12 x 0
0x10+16*i: u8 psm, u8 cpsm, u16 0 | u16 w | u16 h | u16 img_qwc | u16 clut_qwc | u32 data_off (relative to this entry)
data: linear indices (w*h or w*h/2) then clut_qwc*16 bytes CLUT
```
- [OBS] PSM is only **PSMT8 (0x13) or PSMT4 (0x14)**, and CLUT is always PSMCT32 (cpsm byte 0). There are no direct-colour textures on disc. FUN_001c1e00 also accepts CPSM=2 (CT16) and PSMT8H/4HL/4HH, but those are runtime aliases.
- [OBS] CLUT is always 0x40 qwc = 1024 B = 256 RGBA entries in **CSM1 order** (blocks 8..15 and 16..23 swapped), uploaded as a 16x16 CT32 rect (FUN_001c02a0, FUN_001bb470 TRXREG 0x1000000010). PSMT4 textures also carry a 256-entry CLUT block. Palette **CSA 0 is normal, but 11 UI/FX textures use 2..12 sub-palettes** (GAME_FIX.TEX #1: 12, SYN_SLOT #4: 11, MSG_BASE: 10, TITLE #1: 4…). See `tex_samples/ui_GAME_FIX_font_csa_variants.png`: the same sprite atlas recoloured gold/blue/green/red by palette.
- [OBS] Alpha: all 2061 CLUTs have max alpha <= 0x80, so 0x80 means opaque. Scale with a*255/128. 1237/2061 contain alpha < 0x80 (cut-outs and foliage).
- [OBS] Data is **linear, not GS-swizzled**. The uploader writes BITBLTBUF.DPSM = the file psm and sends the bytes unchanged. The linear decode is visually correct for every sample.
- [OBS] **TEX0/TEX1/CLAMP are not stored in files.** TEX0 is built at runtime by the VRAM manager (FUN_001c12e0 / 001c1160 / 001c0fe0): TBP from the allocated page, TBW=(w+63)/64, TW/TH=log2, TCC=1, CBP from the CLUT slot, CSM=0, CSA as a caller argument, CLD=1.
- [OBS] Dimensions are all power-of-two: 256x256 (1753), 128x128 (210), 512x256 (60), 512x512 (38). The only non-POT images are the raw 640x448 BIN screens.
- [OBS] No mipmaps: one level per entry, and the TEX1 constants in code are 0x60 (bilinear, MXL=0) or 0 (nearest) (29/19 occurrences). [INF] The world geometry's TEX1 may sit in VU/GIF templates that were not traced.
- [OBS] **Atlases everywhere**: each PAC 256x256 page packs many sub-textures (walls, doors, foliage, sky) with magenta/flat fill (see `env_ST_000_PAC_sheet.png`). Character pages pack face/body/hands, and UI/FX pages pack sprites.
- **UVs** [OBS from the working render in prior sessions]: PCK character UVs are int16/32768, normalised to the page. Stage geometry UVs (PAC section 3) are float pairs in [0,1] [INF]. Both are resolution-independent, so a same-aspect higher-res page works with unchanged UVs. Caveats:
  - Bilinear and mip bleeding across atlas sub-rect borders.
  - [UNK] Whether stage tiling relies on REGION_REPEAT CLAMP. The CLAMP constants seen in code are 5 (clamp) and REGION_CLAMP 639x447, both used for framebuffer effects.

## 3. Decoder
`poc/tex_hg.py` + `poc/tex_dump.py` decode TEX, PAC (blob scan), TXS and raw BIN. Verified samples:
- environment: `ST_000_PAC_142400_*` and `env_ST_000_PAC_sheet.png`
- character: `char_FIN_000_TEX_sheet.png`, `char_HEW_000_TEX_sheet.png`
- UI: `ui_TITLE_TEX_sheet.png`, `TITLE_BACK_BIN_*`, `ITEM_000`, `H_1F` (map), `ui_THUMBNAIL_TXS_sheet.png`

Colours and alpha are correct (alpha scaled from 0x80).

## 4. Load and upload path (addresses in SLUS_210.75)
- [OBS] `stage_load_pac` 0x120720 reads the whole PAC into one of two 0x2A0000 buffers (ctx+0x99C0 + i*0x2A0000). Textures stay in EE RAM.
- [OBS] **Texture-set manager** `DAT_0044e998` (vtable 0x46d7d0): `+8` = FUN_0026bb00(slot, blob). The slot type table is at 0x3ea970 (3 B/slot): slots 0,1 = type 2, slots 2..6 = type 3.
  - Type 1 is FUN_0026b690 (prefers region 0). Type 2 is FUN_0026b570 (prefers region 1). Both register every entry through the VRAM manager.
  - Type 3 goes to an **LRU cache** (`DAT_0044e4e8`, vtable 0x46b1d0, FUN_001f41a0): 64 textures share 10 VRAM slots, and the get-handle call returns `slot|0x80000000` on a miss.
  - Callers: title FUN_0012f720 (slot 6 = TITLE.TEX), stage/scene FUN_001a3ee0 (slot from ctx+0x1528).
- [OBS] **VRAM manager** `DAT_0044e9a0` (vtable 0x46b050, 64 handles of 0x12 B at +0x98):
  - `+0x5c` FUN_001bfec0(entry, region) is the choke point for load-time registration. When region=1 it rewrites **PSMT8 to PSMT8H and PSMT4 to PSMT4HL/HH**, allocates (`+0x14` FUN_001c1e00), then uploads with `+0x48` FUN_001c02a0. That function uses libgraph sceGsSetDefLoadImage 0x10ccb8, ExecLoadImage 0x10cea0 and SyncPath 0x10c9a0, sending the whole image then the 16x16 CLUT.
  - VRAM layout: region 0 = words 0xC0000..0xFC000 (3-3.94 MB, 120 pages). Region 1 = page*0x800 from 0, i.e. **the upper byte/nibbles of the 0-3 MB frame/Z buffer area**. CLUTs are at 0xFC000 + page*0x800 + idx*0x100.
  - `+0x4c/+0x50` load a TEX by filename (first entry), and `+0x54/+0x58` register entry i of a blob.
- [OBS] **Per-frame uploads**:
  - FUN_001bb470 (renderer `DAT_0044e4f0` vtable 0x46ac50 +0x44) builds an inline PATH2 packet: CLUT 16x16 CT32, image w x h, then TEXFLUSH. It runs whenever a type-3 cache lookup misses (FUN_0012d7f0, title draw).
  - FUN_001bb010 (+0x4c) re-uploads TITLE_BACK.BIN (640x448 PSMT4) every frame.
  - FUN_001bb230 (+0x48) is a generic PSMT8 upload.
- [OBS] **Runtime CLUTs**: FUN_001b8d30 (+0x94) writes a 256-entry CLUT via HWREG from a callback. FUN_001f9d90 pairs it with TEX0 0x621b13000 (PSMT8H): palette-lookup effects that read the frame/Z buffer as a texture. These belong to the render-effects worker and are not file textures.

**Hook point for a native build:** wrap FUN_001bfec0 (load-time) and FUN_001bb470 (cache re-upload). Both receive the TEX entry pointer, which lies inside a known file buffer, so key on (file name, blob offset, entry index) or on a content hash of entry+indices+CLUT. Bind a host texture to the VRAM handle (0..63). The renderer then resolves TEX0.TBP0/CBP/CSA back to that handle and samples the host texture with the same normalised UVs.

Complications:
1. The 8H/4HL/4HH aliasing into frame/Z-buffer bits means the TBP is shared with render targets. Map by handle, not by address.
2. LRU slot reuse: the same TBP holds different textures over time.
3. CSA palette variants: replace the indexed image plus palette, or one RGBA texture per CSA.
4. Runtime-generated CLUT effects and framebuffer-as-texture passes (REGION_CLAMP 640x448) must keep native-resolution semantics.
5. Atlas bleeding at higher res, and mip generation must respect sub-rects.
6. FMV and LEN art are outside the TEX path.

## 5. PCSX2 texture-replacement baseline
[OBS] PCSX2 is not installed here, so this is untested. Reasoning:
- (+) File textures are uploaded whole, unchanged and linear, as a full w x h transfer plus a separate CLUT. PCSX2 hashes the texture data with the CLUT, so each texture is stable and dumpable. Load-time uploads happen once per stage/scene.
- (+) The type-3 LRU re-uploads send identical bytes, so the hash still matches. They cost re-hashing only.
- (−) CSA sub-palettes give one hash per palette variant (up to 12 per UI/FX texture).
- (−) Stage textures are preferably placed as **PSMT8H in frame/Z-buffer upper bytes**. PCSX2's HW texture cache treats these as overlapping render targets, which can make replacement fail or flicker. [INF] This is the biggest risk.
- (−) Runtime-CLUT effects and the per-frame 640x448 title background are fine but not useful to replace.
- (−) Atlas pages mean one replacement per 256x256 page, which is workable.

Cheapest test: in PCSX2 ≥1.7 enable Graphics → Texture Replacement → "Dump Textures", load the first stage (ST_000) and the title screen. Check that the 14 ST_000 pages and the FIN_000 pages each appear once with stable hashes, and that the dumps are not mixed with framebuffer bytes. Then drop a recoloured page into `textures/SLUS-21075/replacements` and see whether it shows in the 8H-placed stage geometry.

## 6. Extraction with ids and the runtime check (2026-10-25)

Tools: `tools/textures/texdump.py OUT` (every texture on the disc with an id, 7 s), `HG_TEXLOG=FILE` (gs.c: what the GS
receives and what the game draws with), `tools/textures/texmatch.py MANIFEST TEXLOG`. Code reading with rea + Ghidra
(R5900 language, `tools/rea/README.md`).

- **id** = `<crc32 of the index bytes>_<crc32 of the 1 KB CLUT>`, both exactly as uploaded (linear, unswizzled), so the
  runtime computes the same numbers from the image transfers. Disc: **2,063 instances, 1,398 ids** (the census above
  plus TITLE_BACK.BIN and LOGO_CRI.BIN). Output in `D:\ps2-work\texwork` (`/mnt/NewVolume/ps2-work/texwork`): `source/<id>.png`
  and `manifest.tsv` (file, blob offset, entry, size, psm). Game data: never committed.
- **Placement, decompiled** (`vram_register_tex_entry` 0x1bfec0 -> `vram_alloc_tex` 0x1c1e00 -> `vram_upload_tex_clut`
  0x1c02a0; TEX0 from `vram_build_tex0` 0x1c12e0). Handles: 64 x 0x12 bytes at manager+0x98 (+2 w, +4 h, +6 psm, +8 page,
  +0xc cpsm or 0xff, +0xe clut page, +0x10 clut index). TBP = page x 0x800 words for PSMT8H/4HL/4HH (the frame buffer's
  upper bits), else 0xC0000 + page x 0x800; CBP = 0xFC000 + clut page x 0x800 + (index << 11) / (16 if cpsm = CT16
  else 8). Region 1 turns PSMT8 into 8H and PSMT4 into 4HL. The only caller of libgraph's LoadImage pair
  (0x10ccb8/0x10cea0) is `vram_upload_tex_clut`; the cache re-upload (0x1bb470), TITLE_BACK (0x1bb010) and 0x1bb230 build
  their own PATH2 packets.
- **Checked on the user's session `recordings/20261024-230145`** (to VB 26,000: title, ST_02A, ST_013, ST_021, the
  EV0002 cutscenes): 182,066 image uploads and 179,620 CLUT uploads (content changes per address), **all disc textures
  and CLUTs**; the 21,107 others are CT32 16x448 / 16x224 strips at bp 8704 / 12288 (movie frames) and 12 one-off
  256x256 CT32 uploads at start. Paletted texture setups drawn: 96 map to disc ids (59 ids); 12 read the movie buffer or
  the frame buffer as indices (palette effects, not replaceable by design). Every on-screen disc texture is identified
  by its TEX0 and the content at TBP/CBP, which is what the replacement layer needs.
