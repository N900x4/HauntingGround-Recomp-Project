# DLSS for the Haunting Ground recompilation: feasibility study (written 2026-10-15)

Research only: nothing was built or run. Sources are listed at the end; in the text, **[official]** marks a statement
taken from NVIDIA's own documents, headers or pages, **[third party]** marks press or community material, and
**[inference]** marks my own reading of the code or the documents. Where a fact could not be confirmed it says so.

## 1. Executive summary

- **DLSS is an output-only stage for us.** Every DLSS component consumes images (colour, depth, motion vectors) and
  produces a picture. Our exact GS memory (copy 0), the game's state, the frame hashes and all exactness gates are
  untouched by construction. DLSS would replace or complement the *picture* path (`display_pic_` -> compose ->
  window), nothing else. That is the strongest argument for it: it cannot break faithfulness of the game itself.
- **DLSS Super Resolution (SR) is feasible and the only component worth doing.** It needs colour, depth, motion
  vectors and a per-frame jitter at render resolution. We can produce all four from what exists: N x N copies with a
  shared per-frame jitter (instead of 25 fixed offsets), the PS2 Z buffer of the displayed frame, and the frame
  interpolation's triangle matching turned into a per-pixel motion image. Vulkan is supported; a compute-only
  renderer is fine (DLSS only needs `VkImage`s). Prerequisite: phase 1.2 (present from video memory).
  Effort after that: 6-10 agent-days. Benefit: raster work for a 4K picture drops from 25 copies to 4 or 9 per frame,
  which is what 120 fps and weaker RTX cards need; on the RTX 5090 at 4K/60 our own x5 supersampling is expected to
  reach full speed anyway, so SR is headroom and portability, not a necessity.
- **Frame Generation / Multi Frame Generation: no.** Our interpolation re-renders real geometry at known motion,
  leaves the UI alone, adds exactly one VBlank, is deterministic and runs on any Vulkan GPU. DLSS-FG is image-space
  warping that needs Reflex, a Streamline-owned swapchain and hardware GPU scheduling, adds latency, and from a
  30 fps base is well below the frame rates NVIDIA's partners recommend [third party]. Revisit only for 240 Hz.
- **Reflex: no (through Streamline).** The latency here is the emulated console's frame pipeline plus our queue;
  Reflex cannot shorten it. The open Vulkan extension `VK_NV_low_latency2` could be tried later for present pacing
  without any SDK or licence.
- **Ray Reconstruction: no.** It denoises ray-traced signals and needs normals, albedo, roughness and hit distances.
- **DLSS 5 (neural rendering, "DLSS-NR"): not now, maybe never as a default.** Confirmed inputs are the finished
  colour frame and motion vectors [official]; the public Streamline 2.14.1 headers already contain
  `kFeatureDLSS_NR = 1004` and "uplift" buffer types, but there is no public header, guide or plugin for it yet.
  It is RTX 50 only, driver 616.64+, and costs about 8 ms per 4K frame on a 5090 today [third party]. It re-lights
  and re-materials the frame with photoreal priors, which is the opposite of "accuracy to the original developers'
  vision"; at most an optional "neural lighting" toggle, off by default, added on top of the SR integration if the
  SDK becomes public and the user wants it after a side-by-side look.
- **Licensing is the real gate.** Streamline's SDK is MIT, but the DLSS feature binaries (`nvngx_dlss*.dll`) are
  under the NVIDIA RTX SDKs licence, which forbids any use "that would cause it to become subject to an open source
  software license", requires NVIDIA marks on splash screens/about box/credits, notification before a commercial
  release, and NVIDIA-GPU-only use [official]. With a GPL-3.0 main program the workable shape is: all DLSS code in a
  separate, permissively licensed plugin DLL with a small C ABI, a GPLv3 section 7 additional permission in the main
  program, and NVIDIA binaries never committed and preferably downloaded by the user at first run. The branding
  clause needs a user decision (an NVIDIA logo in the game's own screens is out of the question for this project; a
  launcher "about" box is the realistic place).
- **Toolchain:** Streamline is a DLL with `extern "C"` exports and plain-data structs, so a shim built with MSYS2
  g++ (or even C with hand-written declarations) is realistic; NVIDIA does not support MinGW, so this must be proven
  in a half-day spike. The NGX static library route (`nvsdk_ngx_s.lib`) is MSVC-only and should be avoided.

## 2. What each component needs (facts)

### 2.1 Common Streamline facts

Streamline (SL) is NVIDIA's integration layer; all DLSS features, Reflex and DLSS 5 go through it [official].

- Version on GitHub: **2.14.1** (8 Sep 2026 per the releases page; years are not shown) [official]. Release notes:
  2.14.1 added V-Sync/frame-rate-limiter support for Dynamic MFG and **`VK_NV_low_latency2` support**; 2.11.1 added
  Dynamic Multi Frame Generation [official].
- APIs: D3D11, D3D12, **Vulkan**. Requirement: Windows 10 20H1 or newer, a GPU "capable of running DirectX 11 and
  Vulkan 1.2 or higher", NVIDIA driver 512.15 or newer [official]. Linux is not covered by Streamline [official,
  absence]. (The bare NGX/DLSS SDK does ship Linux `.so` files for DLSS SR and RR [third party: dlss_wgpu docs].)
- Integration modes [official]: (a) load `sl.interposer.dll` instead of `vulkan-1.dll` and take
  `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` from it (SL proxies `vkCreateInstance`/`vkCreateDevice` and adds
  extensions, features and queues itself); or (b) "manual hooking": create instance/device natively, call
  `slGetFeatureRequirements` first (it returns required instance/device extensions, Vulkan 1.2/1.3 feature names and
  the number of extra graphics/compute/optical-flow queues), then `slSetVulkanInfo` with device, instance, physical
  device and the queue family/index for compute, graphics and optical flow. For DLSS-G and Reflex the swapchain
  functions (`vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`,
  `vkQueuePresentKHR`, `vkDeviceWaitIdle`, `vkCreateWin32SurfaceKHR`, `vkDestroySurfaceKHR`) must go through SL;
  for SR alone nothing about presentation is hooked [official, ProgrammingGuideManualHooking].
- `slInit` must be called before any Vulkan call, never from `DllMain`, and is not thread-safe; use
  `PreferenceFlags::eUseFrameBasedResourceTagging` and `slSetTagForFrame` (`slSetTag` is deprecated); the host must
  restore its command buffer state after `slEvaluateFeature`; Vulkan tags must carry layout, format, extent and
  usage because SL cannot query them; a tagged depth image view needs a depth-only aspect [official].
- DLLs: `sl.interposer.dll` and `sl.common.dll` are mandatory; per feature `sl.dlss.dll` + `nvngx_dlss.dll`,
  `sl.dlss_g.dll` + `nvngx_dlssg.dll` (DLSS-G "is provided as prebuilt DLLs only"), `sl.reflex.dll`, `sl.pcl.dll`,
  `sl.dlss_d.dll` + `nvngx_dlssd.dll`. Production builds are signed; `sl::security::verifyEmbeddedSignature` checks
  the NVIDIA certificate; development/self-built DLLs must not ship [official].
- Building Streamline itself needs VS2019+ and Windows SDK 10.0.19041+ [official]. We would not build it; we use the
  signed release binaries.
- ABI (from `sl_core_api.h`, `sl_struct.h`, `sl_core_types.h`) [official]: every exported function is
  `extern "C"` (`SL_API`), no explicit calling convention (x64 default), C++ references in the signatures
  (`const sl::Preferences&` etc.). Structs derive from `BaseStructure { BaseStructure* next; StructType structType
  (16 bytes); size_t structVersion; }` with no virtual functions; `FrameToken` is opaque (pure virtual conversion,
  only ever handled by pointer); `Resource` has constructors and conversion operators to D3D interfaces (so the
  header pulls in D3D types). Feature-specific calls (`slDLSSGetOptimalSettings`, `slDLSSSetOptions`, ...) are
  function pointers fetched with `slGetFeatureFunction`.
- `Preferences` [official]: `featuresToLoad`, `applicationId` (NVIDIA-issued) **or** `engine = eCustom` +
  `engineVersion` + `projectId` GUID, `renderAPI` ("please specify if planning to use Vulkan"),
  `flags` default `eDisableCLStateTracking | eAllowOTA | eLoadDownloadedPlugins` (OTA updates of SL/NGX are on by
  default).
- `Constants` [official]: "all parameters must be provided unless marked optional": `cameraViewToClip`,
  `clipToCameraView`, `clipToPrevClip`, `prevClipToClip` (row-major, **no jitter in the matrices**),
  `jitterOffset` (pixel space), `mvecScale`, camera position/up/right/forward, near/far/FOV/aspect,
  `depthInverted`, `cameraMotionIncluded`, `motionVectors3D`, `reset` ("previous frame has no connection to the
  current one"), `orthographicProjection`, `motionVectorsDilated`, `motionVectorsJittered`,
  `minRelativeLinearDepthObjectSeparation` (its comment assumes hyperbolic depth: `lin = 1/(1-d)` or `1/d`).

### 2.2 DLSS Super Resolution

- Required tags [official, ProgrammingGuideDLSS]: `kBufferTypeDepth`, `kBufferTypeMotionVectors`,
  `kBufferTypeScalingInputColor` (render resolution), `kBufferTypeScalingOutputColor` (output resolution).
  Optional: `kBufferTypeExposure` (otherwise auto exposure; irrelevant for SDR with `colorBuffersHDR = eFalse`),
  `kBufferTypeBiasCurrentColorHint` ("lerp(history, current, bias); set to 1 to completely reject history"),
  `kBufferTypeTransparencyAndCompositionMaskHint`, `kBufferTypeScalingOutputAlpha` (alpha upscaling, costs
  performance), `kBufferTypeAnimatedTextureHint`, `kBufferTypeParticleHint` and friends.
- Motion vectors: screen-space, per pixel, "accurate for all scenes, materials and objects"; `mvecScale =
  {1/renderWidth, 1/renderHeight}` for pixel-space vectors; low-resolution vectors are detected from the tagged
  buffer's size [official]. Jitter must be "in pixel space"; missing/incorrect jitter gives poor anti-aliasing and
  flicker [official, DLSS 4 Streamline blog].
- Modes (`sl_dlss.h`) [official]: `eMaxPerformance`, `eBalanced`, `eMaxQuality`, `eUltraPerformance`,
  `eUltraQuality`, `eDLAA`. The ratios are not in the header; the long-standing public figures are 2.0x
  (Performance), 1.72x (Balanced), 1.5x (Quality), 3.0x (Ultra Performance), 1.0x (DLAA) [third party / common
  knowledge]. `slDLSSGetOptimalSettings` returns `optimalRenderWidth/Height` and `renderWidth/HeightMin/Max`, the
  range for dynamic resolution; one developer reports the SDK accepting 1/2 to 1.0 of output in that range
  [third party, NVIDIA forum]. Presets: K default for DLAA/Quality/Balanced (transformer), M for Performance,
  L for Ultra Performance (sharper, more expensive) [official]. `sharpness` is deprecated ("not supported").
- GPUs: all GeForce RTX (20 series and later) [official, NVIDIA DLSS 4 announcement]. DLSS 4.5 (Jan 2026) added a
  second-generation transformer SR model "using 5x more compute" [official, developer blog]; measured cost of the
  transformer presets versus the older CNN is a few percent of frame rate at 4K on RTX 50 [third party];
  no absolute millisecond figure for a 5090 at 4K was found, so **measure it** (expect roughly 1-2 ms at 4K
  [inference]).
- Compute-only renderer: DLSS does not care how the inputs were made; it needs `VkImage`s with the usual sampled /
  storage usage and runs on a queue with compute capability. Our queue family 0 (graphics+compute on NVIDIA)
  qualifies [inference]. NGX's Vulkan header is `extern "C"` [official]; the extensions it needs are returned by
  `NVSDK_NGX_VULKAN_GetFeature{Instance,Device}ExtensionRequirements` (SL wraps this in
  `slGetFeatureRequirements`) [official]. dlss_wgpu warns that DLSS triggers some Vulkan validation errors that
  are "safe to ignore" [third party].

### 2.3 DLSS Frame Generation / Multi Frame Generation (DLSS-G)

- Required [official, ProgrammingGuideDLSS_G]: depth, **dense** motion vectors (camera and object motion),
  **HUD-less colour** ("without any HUD/UI elements"), the backbuffer (intercepted by SL), camera constants every
  frame; strongly recommended a UI alpha (`kBufferTypeUIAlpha`) or UI colour+alpha buffer. **Reflex through
  Streamline is mandatory** ("Any existing Reflex SDK integration that does not use Streamline cannot be used with
  DLSS-G"). Hardware-accelerated GPU scheduling must be on. SL owns the real swapchain and hands the host a proxy;
  it creates an extra graphics queue for asynchronous presents; frame pacing is SL's ("SL pacer"), and generated
  frames can be dropped.
- Vulkan: supported; optical flow runs "in interop mode" by default on Vulkan; native optical flow needs
  Vulkan 1.1+ (1.3 recommended), driver 527.64+, and an optical-flow queue family set up at device creation;
  VSync with FG is D3D12-only; Dynamic MFG needs driver 595.41+ and D3D12 [official]. DLSS 4 replaced the hardware
  optical flow accelerator with an AI model [official, NVIDIA DLSS 4 page]; whether Vulkan still *requires* the
  optical-flow queue was not confirmed (the manual-hooking guide still describes it).
- Multipliers: 2x (FG), up to 6x (MFG, 5 generated frames) [official]. GPUs: FG on RTX 40 and 50, MFG on RTX 50
  [official, NVIDIA DLSS 4 page]. Cost at 4K: 1.72 ms (2x, RTX 5090), 2.77 ms (2x, 4090), 5.25 ms (4x, 5080);
  VRAM about 725 MB at 4K [official]. Latency: at 60 Hz, multipliers above 4x "can add significant latency"
  [official]. Minimum base frame rate: NVIDIA publishes none; partners and reviewers converge on about 60 fps,
  AMD says not below 30 [third party].

### 2.4 Reflex

- Through Streamline [official, ProgrammingGuideReflex]: `slReflexSleep` each frame, `slReflexSetOptions`, PCL
  markers "must always be sent" (simulation, render submit, present, input sample, ping), modes Off / Low Latency /
  Low Latency + Boost; Reflex Low Latency is NVIDIA-only, PCL stats are cross-vendor; `slSetVulkanInfo` for Vulkan.
- Without Streamline [official, Khronos]: `VK_NV_low_latency2` (Vulkan 1.3.266, 2023) exposes the same mechanism
  (`vkSetLatencySleepModeNV`, `vkLatencySleepNV`, latency markers) as an open extension; `VK_NV_low_latency` is
  the legacy path for the Reflex SDK; "new implementations should use `VK_NV_low_latency2`". Using it needs no
  NVIDIA SDK and no NVIDIA licence (Vulkan headers are Apache-2.0/MIT).

### 2.5 DLSS Ray Reconstruction (DLSS-RR / DLSS-D)

- A denoiser for ray-traced lighting; the Streamline buffer list shows what it wants: normals, albedo, specular
  albedo, roughness, specular/diffuse hit distances and ray directions, noisy radiance [official, header]. We have
  no ray tracing and no G-buffer: not applicable.

### 2.6 DLSS 5 (neural rendering, DLSS-NR)

- Announced at GTC on 16 March 2026 ("the GPT moment for graphics"), shipped in NBA 2K27 on 1-3 September 2026
  [official page dated 1 Sep 2026; third party for the GTC quote].
- Inputs: the page says DLSS 5 "takes a frame's color and motion vectors as input" and is "trained to recognize
  engine data such as color, surface albedo, detailed lighting, and surface normals" [official]. Igor's Lab and
  TechPowerUp read this the same way: albedo/normals are training data, the runtime takes colour + motion vectors
  [third party]. Depth is not mentioned. NVIDIA calls the model a "Pixel Space Diffusion Transformer" [third party
  quoting NVIDIA]. It runs **at output resolution after upscaling** ("the neural pass runs after upscaling, on the
  final output frame") [third party, TechSpot].
- Public SDK status: the Streamline 2.14.1 header defines `kFeatureDLSS_NR = 1004` and three buffer types:
  `kBufferTypeUpliftInputColor = 70` ("Input color for neural-net 'uplift' passes"),
  `kBufferTypeUpliftOutputColor = 71` ("May alias kBufferTypeUpliftInputColor (in-place uplift)"),
  `kBufferTypeUpliftControlMask = 72` ("Optional - 4-channel control mask consumed by uplift passes") [official].
  There is **no `sl_dlss_nr.h`, no programming guide and no plugin DLL in the public repository** (checked the
  include and docs listings) [official, absence]. The developer-facing description: a DLSS 5 SDK "with several AI
  models", **Structure Intensity** (high-frequency detail: AO, reflections, subsurface scattering), **Tone
  Intensity** (low-frequency lighting and colour; "setting it to zero shows the rendered frame's exact colors"),
  automatic **semantic masking** and **engine-level masking** of asset groups; integration "leverages the same
  NVIDIA Streamline framework" plus a UE5 plugin; it is "an optional and independent feature" that runs alongside
  SR, MFG and RR; "output quality scales with input quality: ray-traced or path-traced input produces more accurate
  results than rasterized input" [official]. Early-access or partner-only distribution is implied; the terms are
  not published.
- Hardware: all GeForce RTX 50 GPUs and laptops, GeForce NOW; GeForce Game Ready driver 616.64 [official]. RTX 40
  is not supported; community DLL swaps run it on older cards with rapidly falling performance [third party].
- Cost: Digital Foundry measured NBA 2K27 at 4K ultra on an RTX 5090: 128.4 -> 62.7 fps, about **8.3 ms** per
  frame; TechSpot: 192 -> 76 fps at 4K DLAA, about 8 ms, "largely a fixed cost based on the output resolution";
  enabling SR gave back only 9 % because the pass runs after upscaling [third party]. NVIDIA promises "model updates
  in the fall to improve performance" and claims a 5x gain in six months [official claim].
- Determinism: NVIDIA says "deterministic and temporally stable" in the sense of no random variation between runs
  of the same input; bit-exactness across drivers or GPUs is not claimed [official page; Igor's Lab reading].
- Reception: strongly divided; critics call it an "AI filter" over the artists' work, defenders point at the
  intensity/mask controls [third party, opinion].

## 3. How each input could be produced here

Everything below sits on the picture path and leaves copy 0 and the game untouched. File references are to the
current tree (`recomp/gs.c`, `recomp/gsvk.c`, `recomp/vk/gs_sprite.comp`, `recomp/host_sdl.c`).

### 3.1 Resolution and jitter

Today a picture at scale N is the interleave of N x N copies drawn at fixed sub-pixel offsets
`vk_offf(i) = (2i+1)/(2N) - 1/2` (`gs.c:1177`, `apply_copy` in the shader at line 238). That is true supersampling,
not jitter. DLSS wants one *jittered* render per frame at render resolution and does the accumulation itself.

Mapping [inference]:
- Render resolution = N x native with N = 2 (1280 x 896) or 3 (1920 x 1344). The 4:3 picture on a 4K screen is
  2880 x 2160 (`host_sdl.c:154` stretches to 4:3), so N = 3 is a 1.5x upscale (Quality), N = 2 is 2.25x (between
  Performance and Ultra Performance; allowed if inside `renderWidthMin..Max` from `slDLSSGetOptimalSettings`, to be
  checked). Make the DLSS output image exactly the letterboxed rectangle so the swapchain blit is 1:1.
- Per-frame jitter: add a shared offset `(jx, jy)` to all N x N copies of a frame, from a Halton(2,3) sequence
  scaled to one render pixel, i.e. `[-1/(2N), 1/(2N))` native pixels, quantized to the GS's 1/16 native pixel
  (so the CPU-made copies for lines and serial primitives, `vk_off()`, and the sprite/point path in `apply_copy`
  stay on the fixed-point grid; triangles accept any float offset). At N = 2 the quantization step is 1/8 of a
  render pixel, at N = 3 3/16: coarse but exact, and DLSS only needs the *reported* jitter to match the real one.
  Pass the true offset as `Constants::jitterOffset` (sign convention to be verified with the DLSS indicator in the
  development DLLs). Implementation: a per-frame `jx, jy` in the push constants of the copies dispatch
  (`close_batch`, `gsvk.c:193`) added inside `apply_copy`, and the same values in `vk_offf()` for the CPU side and
  in the render-target offset rule (`p.rt`, shader lines 245-248 and 266: the rule is relative, so it follows).
  The interpolation replay derives copy geometry from the recorded offsets (`gs.c:1769-1782`), so it follows too.
- Cost: N x N copies per frame instead of 25 (plus copy 0, always). At 60 fps with interpolation: 2 x 9 = 18
  native-pixel units versus 2 x 25 = 50 today.

### 3.2 Colour

`gsvk_compose_rec` (`gsvk.c:301`) already composes the display rectangle from the copies into an RGBA picture.
Change the destination from the `dvram` out region to a `VkImage` (R8G8B8A8_UNORM, sampled | storage) at render
resolution; that is the `ScalingInputColor`. Phase 1.2 introduces exactly these slot images, so DLSS depends on it.
SDR: `DLSSOptions::colorBuffersHDR = eFalse`, no exposure buffer. Movies (Sofdec frames displayed by sprites) and
pure 2D frames (menus) should bypass DLSS (no triangles with Z test in the frame -> plain upscale), since there is
no geometry to accumulate and 30 fps video would only ghost.

### 3.3 Depth

The GS has a real Z buffer per render target: `Draw.zbp/zpsm/zmsk/zte/ztst` (`gs.c:289-311`), formats Z32, Z24,
Z16, Z16S. The frame-finishing draws are those with `d->fbp == vk_disp_fbp` (`gs.c:1292`); record their `zbp/zpsm`
and have the compose pass also unswizzle that Z buffer from each copy (same `read_px` addressing as `pixel()`,
shader line 169) into an `R32_SFLOAT` (or `D32`) image, normalized by the format's range.

Caveats [inference]:
- Convention: PS2 games normally test GEQUAL/GREATER with larger Z nearer, so `depthInverted = eTrue`; verify on a
  frame dump (near objects must have the larger value).
- Shape: DLSS expects a perspective projection's hyperbolic depth (`minRelativeLinearDepthObjectSeparation`'s
  comment assumes `1/(1-d)`); what the game's VU1 code writes (something like `k / w` scaled to 24 bits) is unknown
  until the projection set-up is decompiled (roadmap phase 6.3, "camera update and projection set-up"). DLSS uses
  depth mostly for disocclusion and history rejection, so a monotonic depth is probably good enough; the
  separation heuristic may need tuning.
- The camera matrices in `Constants` are "required". We have no camera until the decompilation; the first
  integration must pass plausible placeholders (`cameraMotionIncluded = eTrue`, dense vectors including camera
  motion, approximate near/far/FOV from the depth histogram) and check the result with the DLSS debug overlay.
  This is the largest technical uncertainty of the SR path and the reason the camera decompilation should precede
  a polished DLSS mode.
- 2D passes do not write Z (`zmsk` or `zte = 0`), so depth under UI/effects is the 3D scene's: correct for DLSS.

### 3.4 Motion vectors

This is where this project is unusually well placed. `vi_match` (`gs.c:1617`) already pairs each triangle of the
current frame with the previous frame's triangle (identity = draw state + texture coordinates, ambiguity resolved
by a 16-pixel motion field per render target), and `vi_replay` moves vertices by `f->w - e->w` (previous minus
current, 1/16 pixel). That is exactly DLSS's motion vector (where this pixel was in the previous frame).

Proposal [inference]:
- Give the copies' memory a parallel motion buffer (2 x int16 per native pixel address, one per copy, or one
  `RG16_SFLOAT` image per picture) and a UI/mask buffer (1 byte).
- Carry per-vertex motion (3 x 2 int16) in `GsVkSprite` for triangles; the shader's `draw_at` already has the
  barycentrics `w0, w1, w2` (line 205); after `pixel()` accepted the write (`fb_ok` and the Z test passed) store
  the interpolated motion. Opaque, Z-writing triangles write motion; alpha-blended ones (`abe`) and sprites do
  not overwrite it (industry practice: vectors of opaque geometry). Unmatched triangles get the field's motion
  (`vif_at`) or `motionVectorsInvalidValue`.
- Timing: the matching exists only after a frame completes (one VBlank later) and only with `HG_GSVK_INTERP`. For
  DLSS the log and the match must run whenever DLSS is on (cost today: `vi_t_log`, `vi_t_match`, small). The
  "real" picture of frame C is already shown one VBlank late with interpolation on, so motion for C is known in
  time. For the in-between picture (t = 0.5) the vector is half of (P - C) measured from P; for the real picture it
  is the other half; without interpolation it is the whole. A camera cut (`vi_replay` returns 0 when the average
  motion exceeds 32 pixels) becomes `Constants::reset = eTrue`.
- Scale: vectors are in native pixels times N for the render image; `mvecScale = {1/renderWidth, 1/renderHeight}`.
  The compose pass must take the motion value from the copy the pixel came from, like colour.
- What stays imperfect: UV-scrolled effects (water, fog) have geometry motion but texture motion that DLSS will
  smear; particles drawn as sprites get zero motion (they are in the UI-like class); the game's own feedback
  effects (passes that read a render target of the previous frame, the panic blur) are already "temporal" and
  DLSS's history on top can double the trails. These are the artifacts to review room by room.

### 3.5 UI, text and the effect passes

- UI/text: sprites and triangles with `zte == 0` or `ztst == 1` (always pass) that texture from non-render-target
  pages with CLUT formats (fonts, HUD) after the last Z-tested triangle of the frame. Write 1 into the mask where
  they cover; tag it as `kBufferTypeBiasCurrentColorHint` (history rejected there, so text does not blur) and, if
  FG were ever used, as `kBufferTypeUIAlpha`. Heuristic, so expose `HG_DLSS_UIMASK=0/1` for A/B.
- Effect passes (full-screen sprites texturing render targets; `gsvk_tex_is_rt`, `gs.c:1172`): bloom, blur, colour
  grading and the panic effects happen *inside* the GS frame at native resolution, so DLSS runs after
  post-processing, not before it as NVIDIA recommends ("integrate DLSS as close to the start of post-processing as
  possible"). Consequence: DLSS treats blur and bloom as image content (fine) but cannot un-blur them, and the
  half-resolution effect chains stay the game's look, as with our copies today. Nothing to do; just a quality note.
- Movies, menus, loading screens: bypass (3.2).

### 3.6 Where the DLSS call goes

Phase 1.2's design has the UI thread presenting from `VkImage` slots on queue 1 with a timeline semaphore from the
GS thread's submissions. DLSS fits there: the UI thread, once per picture, calls `slSetTagForFrame` (colour, depth,
motion, mask, output), `slSetConstants`, `slEvaluateFeature(kFeatureDLSS)` on its command buffer, then blits the
output to the swapchain. One frame token per presented picture (in-between and real pictures alike). Streamline is
not thread-safe, so every SL call happens on the UI thread; the GS thread only renders. SL's `VulkanInfo` gets
queue family 0, queue index 1 (the UI queue) as both graphics and compute queue.

### 3.7 DLSS 5 inputs

Confirmed inputs are colour and motion vectors, both available from 3.2 and 3.4 at *output* resolution (DLSS 5 runs
after SR, so the vectors must be upscaled too, which DLSS itself presumably provides; unknown without the guide).
Albedo, normals and materials do not exist in this renderer: the GS receives already-shaded vertex colours and
textures that mix albedo with baked lighting. Approximations (texture colour as albedo, screen-space normals from
the Z buffer) are possible but, per the official description, not needed at runtime; the optional 4-channel
control mask maps naturally to our UI mask plus a "characters vs environment" split if the game's draw order allows
one (not investigated). Everything else about DLSS 5 (options, models, mask semantics) is unpublished.

## 4. Integration mechanics for this code base

- **C code base, C++ SDK.** Two layers: (1) a C ABI of our own (`hg_upscale.h`: init with instance/device/queue,
  per-frame `tag + evaluate`, shutdown, query of optimal render size) implemented by (2) a shim compiled as C++
  that includes the Streamline headers. The shim and nothing else includes `sl*.h`.
- **MinGW/gcc.** Streamline is a DLL with `extern "C"` functions; references are pointers in the x64 ABI; the
  structs are plain data with a 32-byte `BaseStructure` header; enums are `uint32_t`/`uint64_t`/`char` with
  explicit underlying types. So `LoadLibraryW("sl.interposer.dll")` + `GetProcAddress` from MSYS2 g++ should work.
  Unknowns to settle in a half-day spike: whether `sl_core_types.h` compiles under mingw-w64's `d3d12.h`
  (its `Resource` has conversion operators to D3D interfaces), struct layout agreement (`static_assert` on
  `sizeof` against values printed by a tiny MSVC-built checker is not available without MSVC; instead compare
  against offsets implied by the headers and test `slInit`/`slGetFeatureRequirements` round trips), and whether
  SL's own loader is happy with a non-MSVC host (it should be; it only cares about the DLL and the Vulkan handles).
  NVIDIA does not list MinGW as supported. Fallback: build only the shim DLL with the free MSVC Build Tools or
  `clang-cl` (a user decision about installing them). Do not use the NGX static library (`nvsdk_ngx_s.lib`,
  MSVC CRT) from gcc.
- **Vulkan set-up changes** (`gsvk_init`, `gsvk.c:67`): the instance must be created after `slInit`; use
  manual hooking (we keep native `vkCreateInstance`/`vkCreateDevice`): call `slGetFeatureRequirements(kFeatureDLSS)`
  and append its instance/device extensions and 1.2/1.3 features (`sl_helpers_vk.h` builds the feature structs
  from the names); raise `apiVersion` to 1.3 (phase 1.2 already goes to 1.2); then `slSetVulkanInfo`. If
  `vkGetInstanceProcAddr` is taken from `sl.interposer.dll` the SL proxies do this automatically but then also
  intercept the swapchain, which we do not want for SR alone.
- **Driver and OS:** driver 512.15+ for SL, newer for DLSS 4.x features (query at run time with
  `slGetFeatureRequirements`, never hard-code); Windows 10 20H1+; DLSS 5 needs 616.64+.
- **Redistribution:** `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll` next to the executable
  or at absolute paths given in `Preferences`; verify the signature in production; do not ship development DLLs.
  OTA updates are on by default (`eAllowOTA`), which changes the model under our feet; for reproducible pictures
  turn it off (user decision).
- **Checks:** copy 0 gates (G1g, G5) are untouched by construction. Add a `HG_DLSS=0/1` switch, a render-input
  dump (`HG_DLSS_DUMP=dir`: colour, depth, motion, mask as PPM/PFM) so inputs can be inspected, and a picture gate
  with a tolerance against a reference set made with a pinned `nvngx_dlss.dll` version; the DLSS development DLL's
  on-screen indicator for jitter/motion sanity during development only.

## 5. Licensing

### 5.1 The texts

- **Streamline SDK** (`license.txt` in the repository): MIT, "Copyright 2023 NVIDIA Corporation"; `sl_nvperf.*`
  carries the separate Nsight Perf SDK licence [official]. The MIT grant covers the Streamline source, headers and
  the parts built from them. Which licence governs the prebuilt `sl.dlss*.dll` plugins in the release package must
  be read from that package's notices (not checked here); the `nvngx_*` binaries are clearly DLSS SDK material.
- **DLSS feature binaries** (`NVIDIA/DLSS` repository, `LICENSE.txt`): "NVIDIA RTX SDKs LICENSE (v. March 14,
  2024)" [official]. Relevant clauses, quoted:
  - 1(c): distribute SDK software only "as incorporated in object code format into a software application".
  - 2(a): "An application must have material additional functionality, beyond the included portions of the SDK."
  - 2(c): "You agree to distribute the SDK subject to the terms at least as protective as the terms of this
    license".
  - 2(d): notify NVIDIA of non-compliant distribution and enforce your agreements.
  - 4(e): "You may not use the SDK in any manner that would cause it to become subject to an open source software
    license. As examples, licenses that require as a condition of use, modification, and/or distribution that the
    SDK be: (i) disclosed or distributed in source code form; (ii) licensed for the purpose of making derivative
    works; or (iii) redistributable at no charge."
  - No reverse engineering; no claim of NVIDIA endorsement; NVIDIA may push updates and may "temporarily disable
    the DLSS integration" of a released application with unresolved quality issues (Supplement 6.2).
  - Supplement 1: DLSS/NGX "licensed for you to develop applications only" for systems with NVIDIA GPUs.
  - Supplement 4: "You are required to notify NVIDIA prior to commercial release of an application" (submission
    portal: company, SDK, ship date).
  - Supplement 7.1(b): "you must attribute the use of the applicable SDK and include the NVIDIA Marks on splash
    screens, in the about box of the application (if present), and in credits for game applications"; trademark
    use needs prior written approval with samples two weeks ahead.
  - Optional: giving NVIDIA game content grants a perpetual licence to use it for training.
  - Nothing mentions emulators, recompilations, fan ports or third-party game content. The licence looks only at
    "the application" and the SDK.
- **Prior art** [third party]: Quake II RTX (GPL, maintained by NVIDIA) ships without DLSS, reportedly because the
  SDK licence is incompatible with the GPL; Godot's maintainers declined DLSS in core because it means linking a
  proprietary SDK, because of the notify-NVIDIA requirement, and because the library could not ship with the
  editor; Bevy's `dlss_wgpu` (MIT/Apache) keeps the SDK out of the crate, makes the user download it and accept the
  licence, and ships only `nvngx_dlss.dll` plus the licence text from section 9.5 of the DLSS Programming Guide.

### 5.2 GPL-3.0 analysis [inference; not legal advice]

- GPLv3's "Corresponding Source" must include everything needed to build and run the covered work except System
  Libraries. The NVIDIA driver's own `_nvngx.dll` arguably is a System Library; `nvngx_dlss.dll` and the Streamline
  DLLs shipped with the program are not.
- The FSF's position is that a program and a plugin it loads with function calls and shared data structures form
  one combined work, whichever side does the loading. A GPL program that is written to load `nvngx_dlss.dll`
  would, on that reading, need the DLL under GPL-compatible terms, which 4(e) and 2(c) rule out.
- Options, from safest to loosest:
  1. **Separate plugin + additional permission.** All NVIDIA-facing code lives in `hg_dlss.dll` (its own
     directory, MIT or zlib), talking to the main program through our small C ABI. The main program's GPL-3.0
     notice adds a section 7 additional permission: "you may combine this program with the NVIDIA Streamline and
     DLSS runtime libraries and convey the result without the libraries' source". The repository never contains
     NVIDIA binaries. The plugin DLL may be built and published separately.
  2. **User-fetched binaries.** The release never includes NVIDIA files; on first use the launcher offers to
     download the Streamline release from NVIDIA's GitHub, shows the licence, and places the DLLs. We then do not
     "distribute the SDK" at all, which sidesteps 1(c), 2(c) and the strongest form of the GPL conflict. The
     developer who downloads the SDK to build the shim has still accepted the licence, so 4(e) and the branding
     clause remain our obligations in spirit.
  3. **Ship the DLLs** in the release zip with a third-party-notices file that reproduces the DLSS licence and the
     9.5 text. Cleanest for users, but it is distribution under 1(c)/2(c) inside a GPL-licensed download, which is
     what Quake II RTX and Godot avoided. Needs legal advice.
  4. **No SDK at all:** talk to the driver's NGX entry points directly, as vkd3d-proton/dxvk-nvapi do on Linux
     (open-source reimplementations under permissive licences). Undocumented, driver-version dependent, and the
     `nvngx_dlss.dll` model file still has to come from somewhere. Not recommended.
- Branding (7.1(b)) is the clause that fights this project's goal: the game's own splash screens and credits are
  Capcom's and should stay as they are. The realistic reading is a launcher/about box and a README credit with the
  approved NVIDIA mark; whether that satisfies "splash screens" is for NVIDIA's developer relations to confirm, and
  getting approval needs an NVIDIA developer account (the user's).
- "Commercial release" notification likely does not apply to a free fan port; still cheap to send if the user
  wants no ambiguity.
- Nothing in either licence objects to the application being a recompiled third-party game; that question is
  between the project and Capcom, as it already is without DLSS.

## 6. Benefit versus our own pipeline

| Topic | Our pipeline today | With DLSS SR | Judgement |
|---|---|---|---|
| 4K picture at 30 Hz | x5 = 25 copies, 100 % speed on the 5090; x6 78 % (GPU-bound) | x3 = 9 copies + about 1-2 ms DLSS | About 2.5x less raster work; margin for 120 fps and smaller GPUs |
| 4K at 60 fps (interp.) | 2 x 25 copies, 59 % before phase 1.2; 100 % expected after | 2 x 9 copies + 2 DLSS passes | Phase 1.2 alone should reach 100 % on the 5090; DLSS is headroom |
| Geometry edges | Exact 25-sample coverage of the game's own geometry, no history | Temporal accumulation from 9 or 4 samples per frame plus history | DLSS edges are typically smoother in motion, but ghosting on sprites/particles and after cuts is expected |
| Textures | Native texels, the game's bilinear/point filtering, pixel-exact | Same input, but DLSS sharpens and "invents" sub-texel detail | Changes the PS2 look; subjective; keep off by default |
| UI/text | Exact | Needs the mask heuristic; residual blur or flicker possible | Risk |
| Effect passes, feedback effects | Exact (modulo the offset rule) | Treated as colour; double-temporal risk on feedback effects | Review room by room |
| Determinism | Copies differ by 1 level in a few pixels between runs | Not bit-reproducible; model updates via OTA change the picture | Picture gate must be tolerance-based or DLSS off in tests |
| Hardware | Any Vulkan 1.2 GPU with fp64 | GeForce RTX 20+ (SR), 40+ (FG), 50 (MFG, DLSS 5) | DLSS can only ever be optional |

Frame generation specifically: our interpolation is **geometry-based** (triangles re-rendered at true in-between
positions with the real depth test), while DLSS-FG is **image-based** (warps two frames, guesses disocclusions).
Ours knows exact motion per triangle, keeps the UI static for free, adds exactly one VBlank (about 17 ms), is
deterministic (`HG_VI_SELFTEST`) and needs no NVIDIA hardware; FG needs Reflex + SL swapchain + HWS, adds its own
latency and pacing, and interpolating from a 30 fps base is below what anyone recommends [third party]. The one
case where FG/MFG could add something is a 240 Hz display: our 60 or 120 fps output multiplied by DLSS-MFG. Not
worth the integration cost now.

Reflex: the end-to-end latency is dominated by the emulated pipeline (pad read at VBlank N, draw, display two
VBlanks later) plus our interpolation delay (one VBlank) plus the queue (`fq_keep = 2`) plus present. Reflex's
mechanism (delay the CPU's frame start so the GPU queue stays empty) addresses a GPU-bound modern engine, not this
structure. `VK_NV_low_latency2` could replace the UI thread's spin-wait and give the driver pacing hints in phase
1.2 at zero licence cost; the gain is unmeasured and probably small. The roadmap's latency measurement (phase 1.2)
should come first.

DLSS 5 and the art direction: Haunting Ground's look (vertex lighting, baked shadows, deliberate fog, the panic
distortion, a muted palette) *is* the direction. DLSS 5 by design adds "natural skin subsurface scattering, light
transmission through hair and foliage, deeper contact shadows and global illumination" [official], i.e. it
re-lights the frame with photoreal priors learned from modern engines; NVIDIA itself says rasterized input gives
less accurate results than path-traced input. With Tone Intensity at zero the colours are kept and only structure
is added, which is the only setting that could be called "remastered" rather than "re-imagined". It costs about
half of a 60 fps frame budget at 4K on a 5090 today [third party], runs only on RTX 50, and its SDK is not public.
Recommendation: not part of the plan; if the user is curious, revisit when (a) SR is integrated, (b) the plugin
and guide are public, (c) the cost has dropped as NVIDIA promises; then as an optional toggle, off by default,
evaluated side by side against the PCSX2 reference and the original.

## 7. Recommendation and plan

### 7.1 Go / no-go

| Component | Verdict | Why | When |
|---|---|---|---|
| DLSS Super Resolution | **Go, optional, off by default** (conditional on the licence decision) | Feasible with existing data; frees GPU time for 120 fps and other GPUs; no effect on exactness | After phase 1.2 and the licence/branding decision; ideally after the camera decompilation |
| DLAA (DLSS at 1x) | No | Our copies already supersample; nothing to gain | - |
| Frame Generation / MFG | **No** | Our geometry interpolation is better suited; FG needs Reflex, SL swapchain, HWS; 30 fps base | Revisit for 240 Hz only |
| Reflex (Streamline) | **No** | Latency is in the emulated pipeline; Reflex cannot reach it | - |
| `VK_NV_low_latency2` (no SDK) | Maybe, experiment | Open extension, zero licence cost, pacing hints for the swapchain | Phase 1.2, after the latency measurement |
| Ray Reconstruction | **No** | No ray tracing, no G-buffer | - |
| DLSS 5 / DLSS-NR | **Not now** | SDK not public, RTX 50 only, about 8 ms per 4K frame, re-lights the art | Re-evaluate when the plugin is public; optional toggle at most |

### 7.2 Prerequisites

1. Phase 1.2 (present from video memory: `VkImage` slots, timeline semaphores, swapchain on the UI thread).
2. The user's licensing decision (section 8, questions 1-3) and an NVIDIA developer account for the SDK download,
   the mark approval and (if ever) the DLSS 5 request.
3. Preferably the camera/projection decompilation (phase 6.3), so `Constants` can be filled honestly.
4. Recorded sessions covering rooms with particles, water, fog and panic effects (phase 9) for the review.

### 7.3 Phased plan with gates and effort (agent-days; one unattended session is about 6-8 hours)

**D0. Spike: toolchain and licence shape (0.5-1 day).** Download Streamline 2.14.1 into a directory outside the
repository; write a 100-line g++ program (MSYS2 UCRT64) that loads `sl.interposer.dll`, calls `slInit` with
`renderAPI = eVulkan`, `engine = eCustom`, `projectId`, `featuresToLoad = {kFeatureDLSS}`, then
`slGetFeatureRequirements` and prints the extensions, features and queue counts; create a Vulkan 1.3 instance/device
with them and call `slSetVulkanInfo`, `slIsFeatureSupported`, `slDLSSGetOptimalSettings` for 2880 x 2160 in every
mode and print the min/max render sizes. Gate: it runs with g++-built code; the numbers decide N (3.1). If the
headers do not compile under mingw-w64, write the C declarations by hand (ABI in 4) and retry; if `slInit` fails
in a way that points at the compiler, stop and ask about MSVC Build Tools.

**D1. Per-frame jitter in the copies (1-2 days).** `HG_GSVK_JITTER=1`: shared offset per frame in the copies
dispatch and in `vk_offf()`; Halton sequence; with the jitter at zero the picture must equal today's (G10 exact for
copy 0 pictures, tolerance for copies). Also record the jitter per picture for D4.

**D2. Compose into images: colour, depth, motion, mask (3-4 days).** Extend the compose pass to write the display
rectangle's colour, the displayed frame buffer's Z (`zbp/zpsm` of the frame-finishing draws), the motion buffer
and the mask into `VkImage`s at render resolution. The motion buffer needs: per-vertex motion in `GsVkSprite`,
the store in `pixel()` for opaque Z-writing triangles, field/invalid motion for unmatched ones, and the half/whole
rule for in-between/real pictures; the matcher runs whenever `HG_DLSS` is on. Mask: the UI heuristic (3.5).
Gates: `HG_DLSS_DUMP` inspected by eye on stage 1 and the Castle Suite (motion arrows drawn by a small Python tool
on the dump; sanity: static walls have zero motion when the camera is still, the camera pan gives a uniform field),
G1g/G5 unchanged, G6 unchanged, speed within drift (G8).

**D3. The plugin and the shim (1-2 days).** `recomp/plugins/hg_dlss/` (separate licence file): C ABI
`hg_up_init/shutdown/query/evaluate`, shim in C++, loaded with `LoadLibraryW` when `HG_DLSS=1`; failure of any step
falls back to the plain blit with one log line. `gsvk_init` learns the extra extensions/features (only when the
plugin is present). Validation layers clean apart from the known DLSS warnings.

**D4. Evaluate and present (1-2 days).** On the UI thread per picture: tags, constants (`jitterOffset`,
`mvecScale`, `depthInverted`, `reset` on cuts, `cameraMotionIncluded = eTrue`, placeholder matrices),
`slEvaluateFeature`, blit to the letterboxed rectangle; DLSS output image sized to that rectangle; bypass on 2D
frames. Gates: G7 pacing unchanged at 60 and 120 fps; GPU time per picture logged (render, DLSS, blit); G12 for
20 minutes.

**D5. Review and tuning (1-2 days).** Side-by-side captures (`wincap.py`) of x5 copies versus x3 + DLSS Quality
versus x2 + DLSS in stage 1, the Castle Suite and the first rooms with particles; modes and presets as settings;
decide the UI mask default; write `docs/dlss.md` with the numbers. The user looks at the comparisons before DLSS
becomes a menu option.

Total: 8-13 agent-days including the spike, after phase 1.2. The in-game setting ("Upscaler: supersampling /
DLSS Quality / Balanced / Performance") belongs to phase 3's Display page and must say "NVIDIA RTX only" when the
plugin is absent.

**Later, if wanted:** `VK_NV_low_latency2` experiment in the swapchain code (0.5-1 day); DLSS-NR toggle on top of
D4 (2-3 days once a public plugin and guide exist, plus an RTX 50 and the model's cost at that time).

### 7.4 What this would not change

Copy 0, the frame hashes, the lockstep checks, the recordings and their replay, the software GS fallback, the
default picture (supersampling stays the default and the reference). DLSS is a per-user option that trades
exactness of the picture for GPU time or smoothness, on NVIDIA hardware, and nothing in the project's verification
depends on it.

## 8. Questions for the user

1. **Licence shape.** Is a GPLv3 section 7 additional permission (linking with NVIDIA Streamline/DLSS runtime
   libraries) acceptable in the main program, with all DLSS code in a separate permissively licensed plugin DLL?
   Alternative: keep the project pure GPL and drop DLSS. Recommended default: the exception plus plugin.
2. **Distribution of NVIDIA binaries.** User-fetched at first run (nothing of NVIDIA's in our releases or the
   repository), or shipped in the release zip with third-party notices (needs legal comfort)? Default: user-fetched.
3. **Branding.** NVIDIA's licence requires their marks "on splash screens, in the about box, and in credits". The
   game's own screens stay untouched; is an NVIDIA credit in the launcher's about box and README acceptable, and
   may the project ask NVIDIA (through your developer account) whether that satisfies the clause? If no, DLSS is a
   no-go for distributed builds (it could still exist as a developer-only, self-built option).
4. **Is DLSS SR wanted at all?** Phase 1.2 is expected to give 4K/60 at x5 on your 5090 without DLSS. The case for
   DLSS is 120 fps at 4K, weaker RTX cards and lower power. Do these matter to you now, or should DLSS wait until
   after the whole batch (phases 1-4) and the readability work?
5. **Toolchain.** If the MSYS2 g++ shim fails in the spike, may the free MSVC Build Tools or clang-cl be installed
   to build only the plugin DLL? Default: try g++ first, ask again if it fails.
6. **Picture determinism.** DLSS output changes with the model version and OTA updates. Disable OTA
   (`eAllowOTA` off, pinned `nvngx_dlss.dll`) and keep DLSS out of the picture gates? Default: yes.
7. **DLSS 5.** Any interest in an optional "neural lighting" toggle later, given it re-lights the original art and
   is RTX 50 only? If yes, should the project request SDK access from NVIDIA once the plugin is public? Default: no
   for now; revisit after a public release and a side-by-side look.
8. **Frame generation.** Confirm that DLSS-FG/MFG is out of scope and our interpolation stays the 60/120 fps path;
   a 240 Hz display would be the only reason to revisit.
9. **Latency.** May phase 1.2 include a small `VK_NV_low_latency2` experiment (no SDK, no licence) after the
   latency measurement, dropped if it gives nothing measurable? Default: yes, time-boxed to one day.

## 9. Sources

Official NVIDIA documents, headers and pages:

- NVIDIA, "DLSS 5: Neural Rendering Brings Lifelike Lighting to NBA 2K27" (1 Sep 2026):
  https://www.nvidia.com/en-in/geforce/news/dlss-5-3d-guided-neural-rendering/
- NVIDIA Streamline repository (2.14.1): https://github.com/NVIDIA-RTX/Streamline
  - Programming guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md
  - DLSS guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md
  - DLSS-G guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md
  - Reflex guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideReflex.md
  - Manual hooking: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md
  - API changes: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/APIChangesAndImprovements.md
  - Releases: https://github.com/NVIDIA-RTX/Streamline/releases
  - Headers: `sl_core_api.h`, `sl_core_types.h` (`kFeatureDLSS_NR`, Uplift buffers), `sl_struct.h`,
    `sl_consts.h`, `sl_dlss.h`, `sl_helpers_vk.h` under https://github.com/NVIDIA-RTX/Streamline/tree/main/include
  - Licence (MIT): https://github.com/NVIDIA-RTX/Streamline/blob/main/license.txt
- NVIDIA DLSS SDK repository: https://github.com/NVIDIA/DLSS
  - NVIDIA RTX SDKs LICENSE (v. March 14, 2024): https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt
  - `nvsdk_ngx_vk.h` (C API, extension queries): https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_vk.h
- NVIDIA, "How to Integrate NVIDIA DLSS 4 into Your Game with NVIDIA Streamline":
  https://developer.nvidia.com/blog/how-to-integrate-nvidia-dlss-4-into-your-game-with-nvidia-streamline
- NVIDIA, "DLSS 4.5 Delivers Super Resolution Upgrades and New Dynamic Multi Frame Generation" (14 Jan 2026):
  https://developer.nvidia.com/blog/nvidia-dlss-4-5-delivers-super-resolution-upgrades-and-new-dynamic-multi-frame-generation
- NVIDIA, "DLSS 4 Introduces Multi Frame Generation" (CES 2025; AI optical flow, GPU support per feature):
  https://www.nvidia.com/en-us/geforce/news/dlss4-multi-frame-generation-ai-innovations
- NVIDIA Streamline product page / download: https://developer.nvidia.com/rtx/streamline/get-started
- Khronos, `VK_NV_low_latency2`:
  https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_NV_low_latency2.html
- Khronos, `VK_NV_optical_flow`: https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_optical_flow.html

Third-party measurements, reporting and prior art:

- TechPowerUp, "NVIDIA DLSS 5 Technical Preview": https://www.techpowerup.com/review/nvidia-dlss-5-technical-preview/2.html
- Igor's Lab, "DLSS 5 at Gamescom 2026": https://www.igorslab.de/en/dlss-5-gamescom-2026-3d-guided-neural-rendering/
- Jon Peddie Research, "DLSS 5 moves AI into rendering": https://www.jonpeddie.com/news/dlss-5-moves-ai-into-rendering/
- TechSpot, "The real DLSS 5 performance cost": https://www.techspot.com/article/3170-real-dlss-5-performance/
- Notebookcheck (Digital Foundry figures), "First DLSS 5 game tested":
  https://www.notebookcheck.net/First-DLSS-5-game-tested-DLSS-5-humbles-RTX-5090-with-over-50-performance-loss.1391978.0.html
- Guru3D, "NVIDIA DLSS 5 Technical Preview Review": https://www.guru3d.com/review/nvidia-dlss-5-technical-preview-review/page-3/
- VideoCardz, "NVIDIA reveals DLSS 5 ... launches this fall" (GTC 2026):
  https://videocardz.com/newz/nvidia-reveals-dlss-5-powered-by-neural-rendering-launches-this-fall
- WinBuzzer, GTC 2026 coverage: https://winbuzzer.com/2026/03/17/nvidia-dlss-5-gpt-moment-graphics-gtc-2026-xcxwbn/
- Wccftech, DLSS 5 roundup: https://wccftech.com/roundup/nvidia-dlss-5/
- TweakTown / Guru3D, driver profiles "DLSS-NR":
  https://www.tweaktown.com/news/111830/nvidias-latest-driver-quietly-adds-the-first-signs-of-dlss-5-neural-rendering-support/index.html
- Phoronix, Vulkan 1.3.266 and `VK_NV_low_latency2`: https://www.phoronix.com/news/Vulkan-1.3.266-Released
- dlss_wgpu (Bevy's DLSS wrapper; shipping files, licence handling): https://docs.rs/dlss_wgpu
- GamingOnLinux comment thread on Godot and DLSS (maintainer reasoning; Quake II RTX remark):
  https://gamingonlinux.com/2023/02/godot-engine-4-first-release-candidate/comment_id=239580
- NVIDIA developer forum, DRS below 1/2 with DLSS:
  https://forums.developer.nvidia.com/t/dynamic-resolution-scaling-drs-with-dlss-scaling-factors-below-1-2/267840
- Wccftech, frame generation usage guidance (base frame rates):
  https://wccftech.com/how-to/how-to-use-nvidia-dlss-amd-fsr-intel-xess-frame-generation-technologies/
- PC Games Hardware, DLSS 4 CNN vs transformer cost on RTX 5090:
  https://www.pcgameshardware.de/Deep-Learning-Super-Sampling-Software-277618/Specials/DLSS-4-vs-DLSS-3-Test-RTX-5090-Performance-1464233/2/
- TechSpot, DLSS 4.5 vs 4.0 preset costs: https://www.techspot.com/article/3080-nvidia-dlss-45-vs-40/
- Remap Radio opinion piece on DLSS 5 and art direction:
  https://remapradio.com/articles/dlss-5-is-an-aesthetic-abomination-its-also-a-technical-failure/
