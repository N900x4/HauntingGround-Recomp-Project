# Upscaling every texture for the native 4K port: research and workflow design (2026-10-15)

Written by Fable 5.1 as a research task: no code was changed, nothing was built or run, no GPU was used. Inputs:
`docs/textures.md`, `docs/roadmap-native-port.md` (phase 7.2, user answers), `docs/gpu-renderer.md`,
`docs/next-session.md`, `tools/formats/tex_hg.py`, and the web sources listed at the end. Numbers marked *est.* are
estimates to be measured in step 0 of the plan.

## Executive summary

- **The whole texture set is small and fully extractable from the disc without playing.** `tex_survey.py` already
  counted 2,061 TEX-format texture instances, **1,396 unique (image + CLUT)**, 91.8 M texels, all PSMT8/PSMT4 with a
  256-entry CLUT, all power-of-two (mostly 256x256 atlases), linear, no mipmaps. Plus 3 raw full-screen BINs, 48
  gallery images in an unreversed codec, and up to ~70 palette (CSA) variants of 11 UI/FX textures. Target: about
  **1,450-1,520 images**. Disc extraction gives 100 % coverage; dumping at the upload hooks during replays is only
  needed to *validate* the id mapping and to build a **usage map** (which texture is drawn where, with which filter,
  wrap, blend and on-screen magnification), which drives classification.
- **Compute is not the problem.** At 4x (1.47 G output texels) an ESRGAN/SPAN-class model finishes the whole set in
  **1-3 minutes** on the RTX 5090 (*est.*); DAT2/HAT transformers in **10-40 minutes**; a one-step diffusion model in
  under an hour; multi-step diffusion (SUPIR-like) in 4-12 hours and it is non-commercial anyway. Running three
  candidate models per class plus an 8x pass on a close-up subset is **1-2 GPU-hours** in total. BC7 encoding and
  metrics add minutes. The real cost is classification, review and the renderer layer.
- **Quality and style risk is the real problem, and it is per class.** Environment atlases take texture-trained
  models well; faces and skin need conservative models and human review; **fonts, glyph atlases and UI should not go
  through a neural upscaler at all** (use xBRZ-style integer scaling or keep native, later vector redraw); soft
  effect/mask sprites should be resampled, not "enhanced"; runtime CLUTs, framebuffer-as-texture passes and
  anything game logic reads are **excluded by construction** (copy 0 is never touched).
- **Recommended default pipeline:** decode from disc with `tex_hg.py` (id = content hash of indices + CLUT) ->
  auto-classify from file origin, pixel statistics and the usage map, then a Claude vision pass over contact sheets ->
  per-class model: ENV `4x PBRify_UpscalerV4` (DAT2, CC0) with wrap padding, fallback `4xTextures_GTAV_rgt-s_dither`
  (CC-BY-4.0); CHAR `4x Nomos8kSC` (ESRGAN, CC-BY-4.0) or PBRify, two candidates, human pick for faces; UI painted art
  PBRify; fonts/glyphs xBRZ 4x; FX Lanczos resample with alpha kept soft; full-screen images 8x -> alpha upscaled
  separately with premultiplied-safe dilation, binary masks re-thresholded, seam check, colour-drift check ->
  downscale-compare gate (PSNR/SSIM/LPIPS at native size, alpha IoU) -> BC7 DDS with mips down to native size + PNG
  for mods -> loaded by the phase 7.2 replacement layer, with G1-G3 hashes required identical (proof that game logic
  is untouched) and a picture gate for the copies.
- **Everything runs on the user's PC from the user's ISO.** Decoded or upscaled textures are copyrighted game data:
  they live outside the repo, are never committed and never distributed. What the repo ships is scripts, the
  manifest schema, model choices (with licence and hash) and per-texture class overrides, so any owner of the disc
  can reproduce the result. Prefer CC0/CC-BY/Apache/MIT models and tools so the recipe stays redistributable.
- **Prerequisite:** the texture-replacement layer in the copies shader (roadmap phase 7.2, 2-4 agent-weeks). The
  offline pipeline (3-6 agent-days to a first full pass) can be built and reviewed before that layer exists, using
  contact sheets and synthetic composites, but the in-game verification needs it.
- **Decisions needed from the user** (section 8): faithful vs enhanced art direction; which classes to upscale;
  4x everywhere or 8x for close-ups; non-commercial models allowed for personal use or permissive-only; storage
  location and budget; whether to approach the author of the 2023 hand-made "Haunting Ground HD Project" pack.

---

## 1. What the project already knows (basis for the design)

From `docs/textures.md` and `tools/formats/tex_hg.py`:

| Fact | Consequence for the pipeline |
|---|---|
| All disc textures are TEX blobs: PSMT8 (0x13) or PSMT4 (0x14), 256-entry PSMCT32 CLUT in CSM1 order, linear data, alpha 0x80 = opaque | One decoder covers everything; outputs are 8-bit indices + palette, so each image can be decoded to RGBA deterministically. |
| 2,061 instances in 167 PACs (stage atlases), 229 TEX (characters, items, map, UI), THUMBNAIL.TXS (110 gallery thumbs); 1,396 unique by image+CLUT md5; 91.8 M texels | The full set fits in a few GB at 4x. De-duplication by content hash halves the work. |
| Sizes: 256x256 (1,753), 128x128 (210), 512x256 (60), 512x512 (38); BIN screens 640x448 | Batch by size on the GPU; 4x of 256x256 is a 1024x1024 output, 8x is 2048x2048. |
| 11 UI/FX textures use 2-12 CSA sub-palettes (recolours of the same indices) | One RGBA replacement per (image, CLUT) pair; deterministic models keep the variants structurally identical. |
| 1,237 of 2,061 CLUTs have alpha below 0x80 (cut-outs, foliage) | Alpha must be upscaled as a channel of its own and re-thresholded where it was binary. |
| No mipmaps (MXL = 0), TEX1 = bilinear or nearest; TEX0/CLAMP are built at run time by the VRAM manager | The usage map must come from run time; the replacement needs its own mip policy. |
| Stage pages are placed as PSMT8H/4HL/4HH in the upper bits of the frame/Z area; a 64-handle LRU cache re-uploads type-3 textures | Identify by handle/content at the upload hooks `FUN_001bfec0` and `FUN_001bb470`, never by address. |
| Runtime CLUT effects (`FUN_001b8d30` via HWREG, TEX0 PSMT8H reading the frame buffer) and `REGION_CLAMP 640x448` framebuffer passes | Not file textures: excluded automatically; they keep the native path. |
| 48 `ART0x.LEN` gallery images: codec not reversed; 135 SFD movies | Out of scope for this pipeline (LEN needs reversing first; movies are a video-upscale task). |
| GPU renderer: exact copy 0 + N x N sub-pixel copies for the picture; at x5 the picture is 3200x2240 | Replacements are sampled by the copies only; each native pixel is sampled N^2 times, so the picture is naturally supersampled. |

Screen arithmetic: on a 4K display the 4:3 picture is 2880x2160, i.e. 4.5x the native 640 columns and 4.8x the 448
rows. A texel drawn at 1:1 on the native screen (UI, full-screen images) therefore covers about 4.6 display pixels:
**4x is the natural factor**, 8x is only useful where the game magnifies a texture (close-up walls, faces in
cutscenes, full-screen images stretched to the display).

## 2. Extraction

### 2.1 Primary source: the disc, through the game's own formats (complete, offline, deterministic)

`tools/formats/tex_hg.py` already parses and decodes TEX, scans PACs for embedded blobs, and handles CSM1 palettes
and 0x80 alpha. The documented survey (`tex_survey.py`, in the old `poc/` tree) walked the whole CVM. The extraction
tool `tools/tex/texdump.py` (new, built from these) should:

1. Open the user's `DATA.CVM` through the existing directory parser (`tools/verify/cvm.py`) or the extracted file
   tree, read every `.TEX`, `.PAC` (blob scan), `.TXS`, and the three raw `.BIN` screens (fixed dimensions from
   `textures.md`).
2. For every entry and every CSA in use, write:
   - `source/<id>.png`: RGBA8 decoded (alpha scaled 0x80 -> 255, as `tex_hg.decode` does);
   - `source/<id>.idx.png`: the raw 8-bit indices as a greyscale PNG plus `source/<id>.clut.bin` (the 1,024-byte CLUT
     in CSM1 order) so the pipeline can re-derive anything later without the disc;
   - a manifest row (section 6.2).
3. **Texture id** = `<xxh3-128 of the index bytes (w*h, PSMT4 expanded to 8-bit)>-<xxh3-64 of the CLUT 1,024 bytes>`
   with the CSA folded in by hashing only the 16 or 256 entries actually used. Two hashes, not one, so that CSA variants
   (same image hash, different CLUT hash) are grouped automatically, exactly as PCSX2 does with `TEX0Hash-CLUTHash`
   names. Keep `(file path, blob offset, entry index, csa)` in the manifest as the human-readable origin; several
   origins map to one id (the 2,061 -> 1,396 de-duplication).
4. Which CSAs to emit: all 16 for PSMT4 textures and all that the runtime ever uses for PSMT8 ones. The disc alone
   does not say which CSAs are used; the usage map (2.2) does. Emit CSA 0 for everything first; add the variants the
   usage map reports.

This gives 100 % of the file textures in minutes, without playing a single room, which is the decisive advantage over
the emulator-style "dump what you see" approach (the community notes that PCSX2 dumping is slow because only textures
on screen can be dumped, and that some games change hashes on re-entering an area).

### 2.2 Secondary source: the upload hooks during deterministic replays (validation and usage map)

Add a logging mode to the runtime (`HG_TEXLOG=path`, no behaviour change, G1-G3 identical): at `FUN_001bfec0`
(load-time registration) and `FUN_001bb470` (LRU re-upload) record `(VBlank, handle, file buffer origin if known,
psm, w, h, content hashes computed from the entry bytes exactly as `texdump` does)`; and in the GS at every textured
draw record `(VBlank, TBP/CBP/CSA -> resolved id, TEX1 filter, CLAMP mode and region, ALPHA/TEST state, prim type,
screen-space area and max texel-to-pixel ratio)`. Then replay the existing recordings (`recordings/20261014-200745`,
the hash routes, and later one recording per chapter from phase 9) with the log on. Output `usage/<id>.json`:

- rooms (stage ids) and VBlank ranges where the texture appears; first-seen screenshot reference;
- filter: nearest vs bilinear (nearest = pixel-art/UI signal); wrap: REPEAT/CLAMP/REGION_* (REPEAT = must stay
  tileable; REGION_REPEAT would need a sub-rect-aware wrap);
- blending: additive or `(Cs-Cd)*FIX+Cd` passes (FX signal); alpha test modes (binary alpha expected);
- magnification percentiles (p50/p95/max of display pixels per texel at x5): decides 4x vs 8x;
- uploads that matched **no** disc id: these are the only textures the disc pass missed (expected: none besides
  runtime CLUTs and render targets); the log stores their bytes so they can be inspected.

Coverage of rooms: the game has about 125-131 named rooms (`rooms-checklist.md`); reached so far: menus, stage 1,
Castle Suite, `ST_010`, `ST_020`. The usage map improves as recordings accumulate (phase 9), but it is **not** needed
for completeness of the texture set, only for the classification signals and the CSA list; the defaults (section 3)
cover textures with no usage data yet. A scripted explorer run (`HG_EXPLORE`-style seeds, 100,000 VBlanks, mentioned
in the roadmap's soak plan) adds coverage without the user.

### 2.3 CLUT handling: one RGBA image per (image, palette) pair

Options considered:

| Option | Verdict |
|---|---|
| Upscale the indexed image and keep the palette (indexed output) | No upscaler produces indices; re-quantizing a 4x result to 256 colours throws away the gain and reintroduces banding. Rejected. |
| One RGBA image per (image, CLUT) pair, upscaled independently | Simple; the renderer samples RGBA. With a deterministic CNN/transformer model (no sampling noise) the palette variants come out structurally identical, differing only in colour. **Recommended.** |
| Upscale CSA 0, recolour the others by palette mapping | Fragile where palette entries merge or split. Keep as a fallback for review-flagged variants only. |

Alpha: decode with alpha scaled 0x80 -> 255 (as now), keep the information "was the original alpha binary?" (set of
distinct alpha values in the used CLUT entries) in the manifest; the renderer scales 255 -> 0x80 back.

## 3. Classification

Each class has a different treatment, and some must not be upscaled. Three signal sources, combined in the
manifest with a provenance (`auto`, `vision`, `human`):

**(a) Origin and pixel statistics (automatic, `texclass.py`):**

| Signal | Meaning |
|---|---|
| `ST_xxx.PAC` blob | ENV (stage atlas) by default |
| `O_xxx/xxx_NNN.TEX` (FIN, HEW, stalkers) | CHAR |
| `TITLE`, `GAME_FIX`, `MSG_BASE`, `SYN_SLOT`, `ITEM_*`, map `H_*`, `SUBSCR/*` | UI; GAME_FIX/MSG = font/glyph candidates |
| `SYSTEM/*.BIN` | FULLSCREEN |
| PSMT4, few used colours, hard edges, many connected small components | glyph/icon atlas |
| low colour count + smooth radial/linear gradient + mostly alpha | FX/mask sprite |
| 256x1, 16x16, 32x32 with monotone ramps, or every row identical | LUT/data (never upscale) |
| magenta/flat fill fraction and rectangle layout | atlas with sub-rects (wrap padding per page, no bleed across sub-rects in mips) |
| skin-tone histogram peak + CHAR origin | FACE/SKIN sub-class (conservative model, mandatory review) |

**(b) Usage map (section 2.2):** nearest filtering -> pixel art/UI; additive blend -> FX; REPEAT wrap -> tileable;
magnification p95 > 6 -> 8x candidate; appears in the options/menu screens -> UI.

**(c) Claude vision over contact sheets:** `texsheet.py` makes sheets of 20 textures (4x nearest-neighbour thumbnails
with the id, size, origin and auto class printed under each cell); about 70 sheets for the unique set. A Claude Code
agent reads each sheet with the Read tool and answers, per cell, with a class from the fixed list and a confidence,
plus flags: `text`, `face`, `lut`, `tileable`, `soft-alpha`, `dither`. Disagreements with (a)/(b) go to a short
human review sheet. Cost: tens of images, a few thousand tokens each, well under an hour.

Classes and treatment (the defaults of section 4.4):

| Class | Examples | Treatment |
|---|---|---|
| ENV | PAC stage pages (989 unique): stone, wood, plaster, foliage, sky | texture-trained model, 4x, wrap padding, dither-aware pre-pass if dithered |
| CHAR | body/cloth/hair pages (~300) | conservative realistic model, 4x, two candidates |
| FACE/SKIN | face regions inside CHAR pages | same model as CHAR but reviewed per texture; option to keep native or use a softer model |
| UI-ART | item pictures, map, thumbnails, title art | texture model, 4x; 8x where shown near full screen |
| UI-GLYPH | fonts, message boxes, icons with text, HUD | **no neural model**: xBRZ (GPL-3) integer scaling or native; vector redraw later |
| FX | particles, light shafts, blood, fog, lens, panic overlays | Lanczos/bicubic resample, alpha kept soft; no "enhancement" |
| FULLSCREEN | TITLE_BACK, LOGO_CRI, LOGO_DOLBY (3), later LEN art (48) | 8x; logos possibly kept crisp by xBRZ or redrawn |
| LUT/DATA | gradient ramps, palettes as textures, anything with data semantics | **never replaced** |
| RT/RUNTIME | render targets, runtime CLUT effects | not file textures; excluded by the hook |

## 4. Upscaling models and tools (local, RTX 5090)

### 4.1 Runner: spandrel in a Python script, not a GUI

- **spandrel** (MIT; the model-loading library that chaiNNer extracted) loads `.pth`/`.safetensors` of ESRGAN,
  Real-ESRGAN, SwinIR, HAT, DAT, SPAN, PLKSR/RealPLKSR, OmniSR, etc. with automatic architecture detection; the
  restrictively licensed architectures live in `spandrel_extra_arches` (not needed here). It does **not** do
  image-to-tensor conversion, tiling or alpha: that is the caller's job, which is what we want for a controlled
  pipeline (batching same-size images, wrap padding, alpha as a separate channel, deterministic settings).
- **chaiNNer** (GPL-3, stable 0.25.1 of October 2025, "under active development", PyTorch via spandrel plus NCNN,
  ONNX and TensorRT) is the right tool for the user's *manual* experiments and for visual model comparisons; its
  chains can also be run from the command line, but an orchestrated, resumable, manifest-driven pipeline is simpler
  as our own script.
- **TensorRT** (ComfyUI-Upscaler-Tensorrt reports 3-4x over PyTorch for Real-ESRGAN engines) is an optional speed-up;
  at our volume it is unnecessary.
- Python 3.14 is installed; PyTorch wheels for 3.14 should be checked (step 0); otherwise a 3.12 venv for the
  pipeline. No installs until the GPU measurement running on this PC is finished.

### 4.2 Model families compared

| Family / model | Arch, licence | Quality for our classes | Speed on a 5090 at 4x per 256x256 input (*est.*) | Alpha / tiling | Style risk |
|---|---|---|---|---|---|
| **4x PBRify_UpscalerV4** (Kim2091, 2025-05) | DAT2, **CC0** | Made for "textures from 2000s-era games"; detail without heavy sharpening; the author's RPLKSR vs DAT2 comparison shows DAT2 ahead | 0.3-1.5 s (DAT2: 217 GFLOPs per 512x512 output, transformers run at low utilisation) -> 1,500 images in 10-40 min | caller handles | low-medium |
| **4xTextures_GTAV_rgt-s** and **_dither** (2024) | RGT, **CC-BY-4.0** | game textures; the dither variant handles the ordered dithering common in 8-bit palettised art | similar to DAT (RGT is a transformer) | caller | low |
| **4x Nomos8kSC** (Helaman, 2023) | ESRGAN 64nf23nb, **CC-BY-4.0** | realistic photo SR with JPEG/blur degradation; good conservative default for characters | 0.02-0.1 s -> 1,500 images in 1-3 min | caller (Real-ESRGAN's own runner upscales alpha through the model or bilinear) | low-medium |
| **4x HDCube2/3** (Venomalia) | ESRGAN, CC-BY-NC-SA-4.0 | trained on GameCube/Wii dumps (stone, wood, metal, cloth, plants, characters, FX); the community's favourite for PCSX2 packs | as ESRGAN | caller | low |
| **4x ClearRealityV1 / Soft** (Kim2091) | SPAN, CC-BY-NC-SA-4.0 | realistic faces/hair/nature; "Soft" variant recommended for games and renders; artefacts on bokeh | SPAN is the fastest class (NTIRE efficient-SR winner): ~0.01-0.05 s | caller | medium (invents skin detail) |
| **4x UltraSharp**, GameAI 2.0, RealisticRescaler | ESRGAN, mostly NC | older generation; sharper, more artefacts | ESRGAN speed | caller | medium-high |
| **Real-ESRGAN x4plus / anime** | RRDB, BSD-3 code, weights free | generic; over-smooths textures, strong on compression | 0.02-0.1 s (3090: 0.04 s for 128->512) | runner splits alpha | medium |
| **HAT / Real_HAT_GAN_SRx4(_sharper)** | HAT (XPixel), Apache-2.0 | top fidelity transformer; GAN variants for perceptual quality | HAT 20.8 M params, 102 GMAdds per 64x64 -> 1-3 s per 256x256 | caller | medium |
| **DAT / DAT-2 / DAT-light**, **SwinIR** | Apache-2.0 | strong generic SR backbones; the game-texture fine-tunes above are what matter | DAT 0.3-1.5 s; SwinIR similar | caller | depends on fine-tune |
| **SPAN / RealPLKSR / PLKSR** | Apache-2.0 / MIT | efficient backbones; fine-tunes exist on OpenModelDB | fastest | caller | depends |
| **OSEDiff** (one-step SD2-based, NeurIPS 2024) | Apache-2.0 | realistic detail synthesis in one step; "SeeSR-level quality, 10x faster" | 0.5-2 s per 1024x1024 output -> 15-50 min for the set | no alpha (RGB latent), tile 512 | **high** (hallucinates materials and faces) |
| **SeeSR** (multi-step, CVPR 2024) | Apache-2.0 | better than OSEDiff, slower | 5-15 s | no alpha | high |
| **HYPIR / HYPIR++** (XPixel, SIGGRAPH 2025 / ICML 2026) | non-commercial declaration | state of the art restoration (SD2.1 LoRA), 1.7 s per 1024x1024 claimed for the hosted model | ~1-3 s | patch 512 / stride 256 | high |
| **SUPIR** | licence conflict (MIT text vs non-commercial clause; SupPixel keeps commercial rights) | best-looking photo restoration; SDXL, slow, needs prompts | 10-30 s per 1024x1024 tile -> 4-12 h | no alpha | very high |
| **InvSR**, **PiSA-SR** | S-Lab 1.0 (non-commercial) / unverified | one-step to few-step diffusion SR | 1-3 s | no alpha | high |
| **Flux ControlNet upscaler (jasperai)**, SD tiled upscale (Ultimate SD Upscale) | FLUX.1-dev non-commercial; SD1.5/SDXL per model | creative re-rendering; not reproducible without seeds; prompt-dependent | 10-60 s per image | no alpha | very high: effectively re-painting the art |
| **NVIDIA RTX Remix AI Texture Tools** | part of the Remix Toolkit (free, NVIDIA terms); REST API, ComfyUI bridge | 4x upscale + PBR (albedo/normal/roughness) from one input; Quality mode tiles 256x256; recommends <= 512x512 input and >= 12 GB VRAM | tens of seconds per texture through the GUI/REST; batch not documented | n/a | medium; PBR output is irrelevant for a GS-style renderer |
| **NVIDIA Texture Tools 3 / Exporter** | proprietary (free download), CUDA | not an upscaler; BC7/ASTC encoder with GPU acceleration | fast | - | - |
| **NVIDIA RTX Video Super Resolution** | driver feature for video playback | not applicable to textures (relevant to the SFD movies only as a playback-time option) | - | - | - |
| **Topaz Gigapixel** | paid, closed | strong general models incl. "Redefine" (generative); used by some pack makers | fast | handles alpha? (check) | medium-high (generative modes) |
| **xBRZ** (Zenju) | GPL-3.0 | pixel-art scaler, 2x-6x integer; crisp edges, no invented detail | CPU, milliseconds | preserves alpha exactly | none (the look of pixel art is kept) |
| Lanczos/bicubic/area (Pillow, OpenCV) | permissive | the right tool for soft FX sprites and alpha | instant | exact | none |

Notes on the estimates: DeeDSR's table measured Real-ESRGAN at 0.04 s for a 128->512 upscale on an RTX 3090;
ComfyUI-Upscaler-Tensorrt reports 5 fps for 512->2048 Real-ESRGAN on an L40s. A 5090 is about 2-3x a 3090 in FP16
dense throughput; DAT README FLOPs are 10-20x ESRGAN's per pixel and transformers run at lower utilisation. All of this
should be measured in step 0 (ten textures per model, `torch.cuda.synchronize()` around the loop). Even if the
estimates are off by 5x, the whole set still finishes within a working day on the slowest class.

### 4.3 Comparison on the criteria asked

- **Quality:** for game textures the texture-trained DAT2/RGT fine-tunes (PBRify V4, GTAV rgt-s) beat generic
  photo models in community comparisons and on the authors' own examples; diffusion models produce the most
  "impressive" output but the least faithful (a Half-Life project found anything beyond 2x "introduced severe
  hallucination artifacts"; a Skyrim pack chose a 2x GAN; a Reddit tester found SDXL img2img "either distorted or
  changed too much").
- **Consistency:** CNN/transformer models are deterministic given fixed weights and settings, which is essential for
  CSA variants, atlas neighbours and reproducibility by end users. Diffusion models need fixed seeds and still vary
  with tile boundaries and batch composition.
- **Alpha:** no model takes RGBA natively; the pipeline splits alpha, dilates RGB into transparent areas before
  upscaling (otherwise the fill colour, often magenta or black, bleeds into edges), upscales alpha as a greyscale
  image with the same model (Real-ESRGAN's `alpha_upsampler='realesrgan'` approach) or bicubic for soft masks, and
  re-thresholds binary masks.
- **Tiling and seams:** CNNs have a receptive field of tens of pixels; padding each page by 32-64 px with
  wrap-around before inference and cropping after keeps wrap-tiled textures seamless (the same trick as Real-ESRGAN's
  `pre_pad`/`tile_pad`, but with wrap instead of reflect padding). For 256x256 inputs no internal tiling is needed at
  all on 32 GB VRAM; even 512x512 inputs fit untiled for DAT2.
- **Licence (for the recipe in a GPL-3 repo, and for a hypothetical pack):** CC0 (PBRify), CC-BY-4.0 (GTAV,
  Nomos8kSC), Apache-2.0 (HAT, DAT, SPAN, SwinIR, OSEDiff, SeeSR), MIT (PLKSR, spandrel), BSD-3 (Real-ESRGAN) are
  fine. CC-BY-NC-SA (HDCube, ClearReality, UltraSharp) is fine for personal use (OpenModelDB: all licences allow
  private use and the licence does not cover the output images) but a non-commercial clause is a poor base for a
  reproducible recipe in a GPL project; use them only as review candidates. HYPIR, SUPIR, InvSR, Flux-dev: avoid.
- **Style risk:** ordered from lowest to highest: xBRZ/Lanczos < texture-trained CNN/transformer < realistic GAN
  fine-tunes < one-step diffusion < multi-step diffusion/Flux. Haunting Ground's look (soft, painterly, dim castle
  interiors, grain and bloom from the effect passes) argues for the conservative end: the N x N supersampling already
  removes the pixel crawl, and the goal of texture replacement is to remove blur and blockiness, not to re-author
  the art. The user's choice (question 1 in section 8).

### 4.4 Default pipeline per class

| Class | Primary | Second candidate (auto-tried if the gate fails or on review request) | Factor |
|---|---|---|---|
| ENV | PBRify_UpscalerV4 (DAT2, CC0), wrap-pad 32, dither pre-pass when flagged | 4xTextures_GTAV_rgt-s_dither (CC-BY-4.0); HDCube (NC) for personal comparison only | 4x; 8x where magnification p95 > 6 |
| CHAR | 4x Nomos8kSC (ESRGAN, CC-BY-4.0) | PBRify V4; Real_HAT_GAN_SRx4 (Apache) | 4x |
| FACE/SKIN | same as CHAR, mandatory side-by-side review; "keep native-smooth" option = Lanczos 4x | ClearReality-Soft (NC, personal) | 4x; 8x for cutscene faces if the usage map says so |
| UI-ART | PBRify V4 | Nomos8kSC | 4x; 8x for title/menu art shown full-screen |
| UI-GLYPH | xBRZ 4x (GPL-3) | keep native (renderer samples the native page) | 4x |
| FX | Lanczos 4x RGB, bicubic alpha, no sharpening | PBRify V4 with soft-alpha rule, only on review request | 4x |
| FULLSCREEN | PBRify V4 then 2x Lanczos, or an 8x two-pass (4x model twice) with review | OSEDiff (Apache) as an "enhanced" option for the user to compare | 8x |
| LUT/DATA, RT/RUNTIME | none | none | native |

## 5. Post-processing, formats, budget

- **Alpha:** (1) dilate RGB into alpha = 0 regions (iterative nearest-colour fill, 16-32 px) before upscaling;
  (2) upscale A separately; (3) if the original alpha was binary (values in {0, 0x80}), threshold the upscaled
  alpha at 0.5 and optionally smooth the boundary by a 1-px feather, so the game's alpha tests (`ATST`
  >= 0x80 / `LESS 0x80` two-pass, `hardware.md`) behave as before; if it was soft, keep it continuous; (4) store
  straight (non-premultiplied) RGBA; the renderer scales 255 -> 0x80.
- **Seams:** wrap padding at inference removes page-edge seams. Verify with a seam metric: gradient energy across
  the wrapped edge divided by interior gradient energy; flag > 1.5. Atlas sub-rect borders: the native bleed is
  half a native texel; at 4x the bleed shrinks to half a high-res texel, so nothing gets worse, provided mips never
  go below the native size (next point).
- **Mipmaps:** the game has none. Generate a chain **down to the native resolution and stop** (4x -> 3 levels), so
  minification never blends more than the native sampling did; clamp LOD in the shader to that last level. Box
  filter in linear light is fine for textures; alpha mips must keep coverage (scale alpha so that the fraction above
  the threshold is preserved; a standard "alpha-to-coverage mip" fix).
- **Palette-quantised originals:** keep the result in true colour. Dithering in the source (common in PS2 8-bit art)
  should either be handled by a dither-trained model or removed by a light pre-blur guided by the palette size; do
  not re-quantise the output.
- **Colour fidelity:** compare the area-downscaled result against the original: mean CIEDE2000 < 1.5, no channel
  mean drift > 1/255 of range, histogram intersection > 0.95. GAN models sometimes lift blacks or tint; this catches
  it, and a per-texture 3x3 colour correction fitted on the downscaled pair can fix small drifts automatically.
- **Format for the game:** BC7 DDS with the mip chain (1 byte per texel, 8 bits of alpha quality), encoded with an
  MIT tool: DirectXTex `texconv -f BC7_UNORM -gpu` (DirectCompute), Compressonator (MIT), or `bc7enc_rdo`
  (MIT, CPU, optional RDO for smaller files). NVTT 3 (proprietary, CUDA) is not needed. PNG stays as the mod-facing
  source format (`mods/<name>/textures/<id>.png`, converted and cached to DDS at first load, as PCSX2 and Dolphin
  packs do).
- **Budget:** 91.8 M native texels. 4x: 1.47 G texels = 1.47 GB BC7 (+0.5 GB mips) or 5.9 GB RGBA8. 8x: 5.9 GB
  BC7 (+2 GB mips). Only one stage and the characters are resident at a time: a stage PAC holds at most 16 pages of
  256x256, i.e. <= 1 M native texels -> <= 67 MB BC7 at 4x; the whole 4x set would even fit resident on the 32 GB
  card. Disk: PNG 4x set roughly 1.5-3 GB; DDS cache about 2 GB. Working tree with three candidates per texture plus
  intermediates: 10-20 GB. An 8x full set would be 4x that; recommend 8x only for the close-up subset (a few hundred
  images at most).
- **Neural texture compression (NVIDIA RTXNTC SDK, beta 0.10, NVIDIA RTX SDK licence, needs cooperative-vector
  shaders):** not needed at this budget; noted only as a future option.

## 6. Orchestration with Claude Code

### 6.1 Folder layout (outside the repo; `HG_TEXWORK`, default `D:\ps2-work\texwork`)

```
texwork/
  manifest.sqlite            one row per texture id (schema below), plus candidates and reviews tables
  source/<id>.png            decoded RGBA, CSA applied
  source/<id>.idx.png        raw 8-bit indices;  <id>.clut.bin  the CLUT (CSM1 order)
  usage/<id>.json            from replays: rooms, filter, wrap, blend, magnification, CSAs seen
  sheets/class/NNN.png       contact sheets for classification (vision pass)
  sheets/review/<class>/NNN.png   native (nearest 4x) | candidate A | candidate B, with ids
  up/<model>@<params>/<id>.png    raw model output (RGBA, after alpha split/merge)
  post/<id>.png              after alpha/seam/colour post-processing of the chosen candidate
  final/png/<id>.png         mod-facing output (PNG)
  final/dds/<id>.dds         BC7 + mips, what the runtime loads (or its cache)
  reports/gate_<stage>.md    machine-readable + human summary per run
  logs/
```

Scripts in the repo, `tools/tex/` (all Python, no game data inside): `texdump.py`, `texusage.py`, `texclass.py`,
`texsheet.py`, `texup.py` (spandrel runner), `texpost.py`, `texgate.py`, `texpack.py`, `texreview.py` (HTML/PNG
review sheets), and the orchestrator `texpipe.py`. Model weights are downloaded by `texpipe.py models fetch` into
`texwork/models/` with URL, licence and SHA-256 recorded in `tools/tex/models.toml` (committed: it is metadata, not
weights).

### 6.2 Manifest schema (the single source of truth)

`textures(id, img_hash, clut_hash, psm, w, h, csa, alpha_kind {opaque,binary,soft}, origins[] (file, offset, entry),
class, class_source {auto,vision,human}, flags[] {text,face,lut,tileable,soft_alpha,dither,atlas}, factor {1,4,8},
model, params_hash, status {new,classified,upscaled,posted,gated,approved,rejected,native}, chosen_candidate,
metrics{psnr_ds, ssim_ds, lpips_ds, dE00, alpha_iou, seam_ratio, nr_score}, reviewer, note, updated_at)`

`candidates(id, model, params_hash, path, metrics..., created_at)`; `runs(run_id, stage, git_commit, started,
finished, counts)`. Idempotency: a candidate is keyed by `(id, model, params_hash)`; re-running a stage skips rows
whose key exists and whose inputs are unchanged. Everything is resumable by status; a crash loses at most one batch.

### 6.3 Stages and GPU batching

1. `texpipe dump` (CPU, minutes): disc -> `source/` + manifest.
2. `texpipe usage` (replays, no window, muted; one run per recording; hours of wall-clock but unattended; G1-G3
   must still pass with `HG_TEXLOG` on).
3. `texpipe classify --auto` then `--vision` (agent reads sheets) then `--review` (human sheet for low-confidence
   cells). Output: class per id.
4. `texpipe upscale --class ENV --model pbrify_v4` (GPU): groups images by size, FP16, batch 8-16 for 256x256 inputs,
   wrap padding per class rule, alpha split, writes candidates. Resumable. Logs images/s so the estimates get
   replaced by measurements.
5. `texpipe post` and `texpipe gate`: metrics computed with **torchmetrics** (Apache-2.0: PSNR, SSIM, MS-SSIM,
   LPIPS wrapper) and the `lpips` package (BSD-2); DISTS from its MIT reference implementation if wanted. Note that
   `pyiqa` is PolyForm **Noncommercial** (plus S-Lab components): usable privately for extra no-reference scores
   (MUSIQ, TOPIQ, NIQE), but not a dependency of the repo's scripts.
6. `texpipe review --class FACE` produces side-by-side sheets; a Claude agent does a first pass (flags obvious
   failures: warped eyes, invented text, haloed alpha, seams), the user does the final pick for the classes they
   care about; approvals and rejections go into the manifest. Rejected -> retry with the next candidate model or
   fall back to Lanczos/native.
7. `texpipe pack`: BC7 + mips for approved ids; `texpipe export --mod <name>` writes the PNG pack layout the
   data-overlay layer (phase 7.1/7.2) reads.
8. `texpipe ingame`: with the replacement layer present, run the picture gate (`HG_FRAMES_SHOWN` ranges with
   replacements on vs off) and the hash gates; produce a per-room screenshot sheet for review.

### 6.4 Quality gates (automatic, per texture)

| Gate | Measure | Default threshold (tune on the golden set) |
|---|---|---|
| Downscale consistency | area-downscale the candidate to native, compare with `source/<id>.png` (RGB, premultiplied by alpha) | PSNR >= 30 dB, SSIM >= 0.90, LPIPS <= 0.15; ENV may use looser PSNR (GAN detail) but tight LPIPS |
| Colour drift | CIEDE2000 mean on the downscaled pair; per-channel mean difference | dE00 <= 1.5; mean diff <= 1/255 |
| Alpha fidelity | binary: IoU of downscaled mask vs original mask; soft: MAE | IoU >= 0.97; MAE <= 2/255 |
| Seam | wrapped-edge gradient energy ratio | <= 1.5 for `tileable`, <= 3 otherwise |
| Sanity | size = factor x native, no NaN/blank, file decodes, DDS mip count | exact |
| Ranking only | no-reference score (MUSIQ/TOPIQ if pyiqa is used privately) and LPIPS between candidates | never a pass/fail by itself |

A candidate that fails goes to `rejected` with the failing metric; the orchestrator tries the class's second model,
then the fallback (Lanczos or native), and marks anything that ends on a fallback for the human sheet.

### 6.5 How the Claude Code agents work

- A coordinator session owns `texpipe.py` runs (background Bash, one GPU job at a time, never while the user's
  measurements or the game are running), reads `reports/gate_*.md`, and keeps a short status in
  `docs/textures-status.md` (counts per class/status, not pixels).
- Classification and review are vision tasks: subagents read sheets (Read tool on PNG) in parallel, 10-20 sheets
  each, and write JSON decisions keyed by id; the coordinator merges them into the manifest with `class_source =
  vision`. Only disagreements and low confidence reach the user.
- Retries are data-driven (the class's model list), so an agent never improvises a model; adding a model means a
  row in `models.toml` with its licence.
- Reproducibility for end users: `texpipe all --iso <path>` runs dump -> classify (using the committed class
  overrides and the vision decisions exported as `tools/tex/classes.tsv`, ids only) -> upscale -> post -> gate ->
  pack, with no agent in the loop; the agents' and the user's judgements are captured as data in the repo.

### 6.6 Time estimate (RTX 5090, 9800X3D)

| Step | Machine time | Agent/human time |
|---|---|---|
| Dump + manifest | 2-5 min | 0.5 day to write the tool (reusing `tex_hg.py`) |
| Usage logging (runtime switch + parser) | hours of unattended replays | 1-2 days to implement and verify G1-G3 unchanged |
| Classification (auto + vision + review) | minutes | 1 day (70 sheets; user reviews ~100-200 flagged cells) |
| Golden set (40 textures, all classes, 6-8 models) + user decision | 10 min GPU | 0.5 day + the user's look |
| Full 4x pass, 2-3 candidates per class | ESRGAN-class 1-3 min; DAT2-class 10-40 min; total **< 2 GPU-hours** | 0.5 day of orchestration |
| 8x subset (a few hundred images, two-pass) | 10-30 min | - |
| Post + gates | 5-15 min (LPIPS on 1,500 pairs at 256x256 is seconds) | - |
| Review sheets (FACE, UI, flagged) | minutes | 1-2 days (the user: an hour or two for faces and UI) |
| BC7 + mips | 1-10 min GPU encoder, 10-30 min CPU RDO | - |
| In-game picture gate and room sheets | needs phase 7.2 | 1 day |
| **Total offline pipeline to a reviewed 4x set** | **under half a day of GPU** | **3-6 agent-days**, plus 2-4 hours of the user's review |

## 7. Legal and ethical

- The textures are Capcom's copyrighted work. Decoded, upscaled and packed textures are derivative works: they stay
  in `texwork/` (outside the repo, `.gitignore` guards in case), are never committed (the existing rule in
  `next-session.md`) and never distributed. The repo ships only the recipe: scripts, manifest schema, class
  decisions keyed by content hash, model list with licences and hashes, thresholds. A "mod pack" in the usual sense
  (PNG files) cannot be published; what can be published is the pipeline, which every disc owner runs locally
  (`texpipe all --iso`), the same principle as the launcher's build-from-your-disc design in roadmap phase 10. Hash
  lists derived from the textures (ids) are not the textures and are fine to ship, like the frame-hash references.
- Model licences: prefer CC0/CC-BY/Apache/MIT (section 4.3). CC-BY requires attribution in `models.toml` and the
  docs. Non-commercial models are allowed for the user's private use but should not be the recipe's default. Weights
  are downloaded at run time, never committed (several are 60+ MB anyway).
- Tooling in the GPL-3 repo: xBRZ (GPL-3) is compatible; DirectXTex/Compressonator/bc7enc (MIT), spandrel (MIT),
  torch (BSD) are fine; `pyiqa` (PolyForm NC) only as an optional private extra; NVTT 3 (proprietary) not used.
  Offline tools are not linked into the game, so their licences only matter for redistribution of the scripts, not
  for the runtime.
- Training-data provenance: some community models are trained on other games' dumps (GTAV, GameCube). Their
  licences say CC-BY/NC; their training data was not the authors' to license, which is an unsettled area. Outputs
  used privately on one's own disc are the lowest-risk use. Prefer PBRify (custom dataset, CC0) as the default.
- Art direction: an upscaler changes the author's work; keep every class switchable, keep the native path one
  setting away, label the result as a fan enhancement, never as official. The 2023 hand-made "Haunting Ground HD
  Project" (over 1,000 textures remade from photographs, 400-500 hours, released for PCSX2 as a video-linked pack) is
  the main existing alternative; it is the author's work and must not be ingested without permission. If the user
  wants it, the right path is to ask the author and to match it by content (PCSX2 dumps -> our ids), as an
  optional overlay in the data-overlay layer.
- Trademarks in full-screen images (CRI, Dolby logos): upscaling for private display is unproblematic; do not
  redraw or alter them.

## 8. Recommendation: step-by-step plan, prerequisites, gates, questions

### 8.1 Prerequisites

1. **Phase 7.2 texture-replacement layer** (2-4 agent-weeks, after 6.2/6.3 symbol map and hook points), with these
   pipeline-facing requirements added to its design: id computed exactly as `texdump.py` does (shared spec in
   `docs/textures.md`); loads `final/dds/<id>.dds` (and PNG from `mods/<name>/textures/` with a DDS cache);
   bindless array or atlas; same wrap/clamp/REGION semantics in normalised UV; bilinear vs nearest per TEX1; alpha
   255 -> 0x80; LOD clamp to the native-size mip; copies only; native fallback when an id is missing; `HG_TEXREPLACE`
   switch; a debug tint (replaced vs native) and "dump unmatched upload" mode; hot reload for review.
2. **`HG_TEXLOG` usage logging** (part of this work, 1-2 days) is independent of 7.2 and can be done first; it is
   also what proves the id mapping.
3. A Python environment with torch + spandrel + torchmetrics + lpips + Pillow + numpy (+ xBRZ binding; the
   `xbrz.py` wrappers exist, or build the C++ once). Not before the current GPU measurements are over.
4. Disk: 20-40 GB on `D:` for `texwork/`.

### 8.2 Steps and gates

| Step | Work | Gate |
|---|---|---|
| 0. Measure (0.5 day) | ten textures per class through 6-8 candidate models; images/s, VRAM; torch on Python 3.14 or a 3.12 venv | numbers replace the *est.* values in this doc |
| 1. `texdump.py` + manifest (0.5 day) | all TEX/PAC/TXS/BIN -> `source/`; ids; de-dup | counts equal `textures.md` (2,061 instances, 1,396 unique); every PNG decodes; md5s of CSA-0 images match the old survey where available |
| 2. `HG_TEXLOG` + `texusage.py` (1-2 days) | hooks at `FUN_001bfec0`/`FUN_001bb470` and the GS draw path | G1, G1g, G2, G3 identical with the log on; every logged upload maps to a disc id or is a known runtime class; CSA list complete for reached rooms |
| 3. Classification (1 day) | auto features, vision pass, human sheet | every id has a class; LUT/DATA and UI-GLYPH lists reviewed by the user |
| 4. Golden set + decision (0.5 day + user) | 40 textures, all classes, side-by-side at 4K crop size | the user picks "faithful" vs "enhanced" per class (question 1) |
| 5. Full pass (0.5 day) | `texpipe upscale/post/gate` per class | >= 95 % of ids pass the gates on the first or second candidate; the rest listed |
| 6. Review (1-2 days + user hours) | FACE, UI-ART, flagged ENV | approvals in the manifest; nothing `rejected` without a fallback |
| 7. Pack (0.5 day) | BC7 + mips, PNG export | DDS loads; sizes within budget |
| 8. In-game (1 day, needs 7.2) | picture gate on/off, room sheets | G1-G3 identical with replacements loaded (copy 0 untouched); G10 picture gate passes with replacements off; visual review with them on |
| 9. Reproducibility (0.5 day) | `texpipe all --iso` on a clean `texwork/`, no agent | identical ids and identical final hashes (CNN/transformer models are deterministic in FP16 only up to driver/kernel choice: record torch/cuda versions; accept small diffs by comparing metrics, not bytes) |

### 8.3 Questions for the user

1. **Art direction:** faithful (texture-trained CNN/transformer, no invented detail, the recommended default) or
   enhanced (diffusion models allowed for environments and full-screen art, more detail, more drift from the original
   look, slower, mostly non-commercial licences)? Can be decided per class after the golden set.
2. **Which classes:** all of ENV, CHAR, UI-ART, FULLSCREEN; FACE/SKIN yes/no (most visible risk); FX left as
   resampled; UI-GLYPH via xBRZ or kept native until a vector redraw? LEN gallery art only if its codec gets reversed
   (worth a half day?).
3. **Factor:** 4x everywhere (recommended) plus 8x for the close-up subset found by the usage map and for the
   full-screen images; or 8x everywhere (4x the disk and VRAM, little visible gain at 2880x2160)?
4. **Licences:** permissive-only recipe (CC0/CC-BY/Apache/MIT), with non-commercial models (HDCube, ClearReality)
   only as private comparison candidates; agreed?
5. **Storage:** `D:\ps2-work\texwork` with 20-40 GB; keep all candidates or only the chosen ones after approval?
6. **Review effort you are willing to spend:** faces and UI only (1-2 hours) or a pass over every stage's sheet
   (half a day)?
7. **The 2023 HD Project pack:** ignore, or ask its author whether it may be matched into our id scheme as an
   optional overlay?
8. **Order:** build the offline pipeline now (it does not touch the runtime except the `HG_TEXLOG` switch) and keep
   it waiting for phase 7.2, or wait until 7.2 is scheduled? Default: `texdump` + classification + golden set now
   (cheap, informs the 7.2 design), the full pass when 7.2 is ready.
9. **Movies:** the 118 event clips at 256x224 and the 512x448 opening/credits are the other low-resolution asset;
   a separate video-SR task (different models, 938 MB of MPEG-1): in scope later, or not at all?

## Sources

Project documents: `docs/textures.md`, `docs/roadmap-native-port.md` (section 3.7, phase 7.2; section 5 user
answers), `docs/gpu-renderer.md`, `docs/hardware.md`, `docs/next-session.md`, `docs/rooms-checklist.md`,
`tools/formats/tex_hg.py`, `tools/ingest.py`.

Models and licences:
- [4x PBRify_UpscalerV4 (DAT2, CC0, 2025-05)](https://openmodeldb.info/models/4x-PBRify-UpscalerV4)
- [4xTextures_GTAV_rgt-s (RGT, CC-BY-4.0)](https://openmodeldb.info/models/4x-Textures-GTAV-rgt-s) and
  [the dither variant](https://openmodeldb.info/models/4x-Textures-GTAV-rgt-s-dither)
- [4x Nomos8kSC (ESRGAN, CC-BY-4.0)](https://openmodeldb.info/models/4x-Nomos8kSC)
- [4x ClearRealityV1 (SPAN, CC-BY-NC-SA-4.0)](https://openmodeldb.info/models/4x-ClearRealityV1)
- [4x HDCube2 (ESRGAN, CC-BY-NC-SA-4.0, GameCube/Wii textures)](https://openmodeldb.info/models/4x-HDCube2)
- [4x RealisticRescaler](https://openmodeldb.info/models/4x-RealisticRescaler),
  [4x GameAI 2.0](https://openmodeldb.info/models/4x-GameAI-2-0), [4x UltraSharp](https://openmodeldb.info/models/4x-UltraSharp)
- [OpenModelDB licence guide](https://openmodeldb.info/docs/licenses)
- [HAT (Apache-2.0; HAT 20.8 M params, 102.4 GMAdds; Real_HAT_GAN models)](https://github.com/XPixelGroup/HAT)
- [DAT (Apache-2.0; DAT-2 11.2 M params, 217 GFLOPs per 512x512 output)](https://github.com/zhengchen1999/DAT)
- [SPAN (Apache-2.0; NTIRE 2024 efficient SR winner)](https://github.com/hongyuanyu/SPAN)
- [PLKSR / RealPLKSR (MIT)](https://github.com/dslisleedh/PLKSR)
- [OSEDiff (Apache-2.0, one-step diffusion SR)](https://github.com/cswry/osediff), [SeeSR (Apache-2.0)](https://github.com/cswry/seesr)
- [HYPIR (non-commercial declaration, SD2.1 LoRA)](https://github.com/XPixelGroup/HYPIR)
- [SUPIR licence conflict issue](https://github.com/Fanghua-Yu/SUPIR/issues/51)
- [InvSR (NTU S-Lab 1.0)](https://github.com/codeyu/InvSR), [PiSA-SR](https://arxiv.org/abs/2412.03017v2)
- [Flux.1-dev ControlNet Upscaler (non-commercial)](https://huggingface.co/spaces/jasperai/Flux.1-dev-Controlnet-Upscaler/blame/main/README.md)
- [Real-ESRGAN runner: alpha split and tiling (`realesrgan/utils.py`)](https://raw.githubusercontent.com/xinntao/Real-ESRGAN/master/realesrgan/utils.py)

Tools:
- [spandrel (MIT; extra_arches for restrictive licences)](https://github.com/chaiNNer-org/spandrel)
- [chaiNNer (GPL-3; PyTorch/NCNN/ONNX/TensorRT; 0.25.1 stable)](https://github.com/chaiNNer-org/chaiNNer)
- [ComfyUI-Upscaler-Tensorrt (L40s timings for Real-ESRGAN engines)](https://github.com/yuvraj108c/ComfyUI-Upscaler-Tensorrt)
- [DeeDSR paper (Real-ESRGAN 0.04 s for 128->512 on an RTX 3090)](https://arxiv.org/pdf/2404.00661)
- [NVIDIA RTX Remix AI Texture Tools (4x upscale + PBR, Speed/Quality modes)](https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.2.4/docs/howto/learning-aitexturetools.html)
- [NVIDIA RTX Remix product page (ComfyUI integration)](https://www.nvidia.com/en-eu/geforce/rtx-remix)
- [bc7enc_rdo (MIT)](https://github.com/richgel999/bc7enc_rdo_devel/), [NVIDIA Texture Tools Exporter announcement](https://forums.developer.nvidia.com/t/announcing-the-nvidia-texture-tools-exporter-2020-1/225488)
- [RTX Neural Texture Compression SDK (beta)](https://github.com/NVIDIA-RTX/RTXNTC), [Phoronix on its licence](https://www.phoronix.com/news/NVIDIA-RTXNTC-0.5-Beta/)
- [pyiqa / IQA-PyTorch (PolyForm Noncommercial)](https://github.com/chaofengc/IQA-PyTorch), [pyiqa on PyPI](https://pypi.org/project/pyiqa/)

Emulator texture replacement practice:
- [PCSX2 `GSTextureReplacements.cpp` (TEX0Hash-CLUTHash naming, dumps/replacements layout, mip suffix)](https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp)
- [PPSSPP texture replacement reference (hashing with CLUT)](https://www.ppsspp.org/docs/reference/texture-replacement)
- [How emulator HD texture upscales are made](https://www.readonlymemo.com/how-emulator-hd-texture-upscales-are-made)
- [PCSX2 HD texture pack hub (GBAtemp)](https://gbatemp.net/threads/638803/post-10425093), [TexMaster](https://texmaster.odoo.com/)
- [r/hauntingground: "Haunting Ground HD Project" hand-made remake, October 2023](https://lr.in.psf.lt/r/hauntingground)
- [Half-Life 2x upscale notes (hallucination beyond 2x)](https://twhl.info/journal/view/9417), [Skyrim pack using BSRGANx2](https://www.nexusmods.com/skyrimspecialedition/mods/105178)
