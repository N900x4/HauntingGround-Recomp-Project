# Audio — sound banks, Hewie, replacement

Tags: [OBS] observed, [INF] inferred, [UNK] unknown. Tools: `poc/hdbd.py` (HD/BD parser + PS-ADPCM decoder),
`poc/vagenc.py` (PS-ADPCM encoder + bank sample replacement), `poc/adx.py` (ADX decoder + SFD audio demux),
`poc/specgrid.py` (spectrogram sheets), `poc/xref.py`, `poc/callargs.py`, `poc/mipsdis.py` (code search).

## 1. What audio exists on the disc
| Kind | Files | Size | Format | Used for |
|---|---|---|---|---|
| Sound-effect banks | 134 .HD + 134 .BD (+130 .SDT) | 16.4 MB BD | Sony SCEI HD v2 header + BD body of PS-ADPCM (VAG) samples; .SDT = game's own SE trigger table | SFX, character voices, BGM instruments |
| Sequences | 13 .SQ in `BGM/` | tiny | Sony SQ (MIDI-like) played on `BGM/STAGEn_BANK` | in-game music (normal / chase / panic) |
| Streamed audio | 72 .ADX in `ADX00-02/` | 123 MB | CRI ADX | ambience / music streams |
| Cutscene + FMV audio | 135 .SFD (121 in EV*/DE* event folders) | 938 MB | Sofdec: MPEG-1 video + ADX audio (stream 0xC0), 48 kHz stereo, Dolby Pro Logic II matrix-encoded | cutscenes (pre-mixed) |

[OBS] Bank loaders: `C_0000.HD/SDT/BD` loaded by code at 0x2CFA14–0x2CFB68 (common, always resident);
`D_%01X000.HD/SDT/BD` by 0x16D350 (two variants, D_0000 and D_1000); per-stage `ST_%03X\ST1_%03X.*`; per-enemy `O_xxx\xxx_000.*`;
music `BGM\STAGEn_BANK.*` + `.SQ`. Files are read into an EE buffer and handed to the IOP. The sound driver is the game's own **`SNDDRV.IRX`**
(string at file offset 0x357310); see `exe/hardware.md` for the IOP side.

## 2. HD/BD/SDT format (decoded)
- [OBS] HD chunks: `Vers` @0, `Head` @0x10 (hd size, bd size, offsets of Prog/Sset/Smpl/Vagi), `Vagi` @0x50, `Smpl`, `Sset`, `Prog`.
- [OBS] `Vagi` entry = {u32 BD offset, u16 sample rate, u8 loop, 0xFF}. C_0000: 94 samples, mostly 16 kHz (some 8/11.025/22.05 kHz), all one-shot (no loops).
- [OBS] `C_0000` has a single program with 94 key splits: sample *i* is mapped to MIDI note 12+*i* at its native pitch. So a SE is simply "play sample *i*".
- [OBS] `.SDT` = 16-byte records, one per SE ID: byte0-1 `01 01`, byte2 category (0x84 footsteps / 0x85 SE / 0x86), byte3 voice/priority group,
  byte4 0xC0, byte5 signed volume/pan trim, byte6 variation, **byte9 = sample index in the bank**, byte10 flag. C_0000.SDT has 152 records.
- [OBS] SE play function: **`0x122C20(obj, a1 = SE ID)`** — 173 call sites. Dog-module wrapper **`0x13A430`**.

## 3. Hewie — where his sounds are and how they are triggered
- [OBS] The code region 0x130000–0x17FFFF is the dog AI (contains the debug strings "Dog New Hide Attack -> No Route" (ref 0x150048) and
  "Dog Bite Enemy -> No Route" (ref 0x159D04), and the D-bank loader 0x16D350).
- [OBS] That region plays **SE 0x58–0x70** through `0x13A430` (13 IDs seen as constants: 58,59,5A,5C,5D,5E,5F,60,65,66,68,6F,70) and
  0x60/0x66/0x69/0x6B/0x6C/0x1E via `0x122C20`. SDT maps these to **C_0000 samples 0x10–0x25** (22 samples, 209,664 bytes ADPCM, ~17 s at 16 kHz).
- [OBS] Another module (0x180000–0x1A0000, the player/Fiona [INF]) plays SE 0x2E–0x45 → samples 0x36–0x4C (23 samples), which acoustically are a
  female voice (F0 ≈ 200–450 Hz, two-syllable calls with a gap = "Hew-ie!" [INF]).
- [OBS] Acoustics of Hewie's set: short harmonic barks (0x12, 0x13, 0x15, F0 ≈ 350 Hz), descending whines (0x14, 0x18–0x1C, F0 500–900 Hz),
  long growls (0x22, 0x23: 2.2 s, F0 ≈ 105–140 Hz), panting (0x11: rhythmic broadband bursts). Spectrograms: `audio/hewie_voice_SE58-70_spectrograms.png`.
- [INF] `D_0000` / `D_1000` (13 and 10 samples, 11.025/16 kHz) are Hewie's paw footsteps / movement (L/R alternating voices 0x20/0x21, surface variants),
  loaded by the dog module; variant chosen by an event-script byte (0x206B10) or a flag test (0x3A0A24).
- [OBS] **Cutscene barks are baked into the cutscene mixes.** `poc/cutscene_scan.py` decoded the ADX audio of 117 event/demo .SFD tracks
  (85 min) and matched every Hewie and Fiona bank sample against them with normalised cross-correlation (`cutscene_scan.tsv`).
  15 tracks contain a Hewie bank sample with NCC > 0.5 (20 with > 0.35), peaking at 0.95 (DE0036 sample 0x18 @17.1 s; EV0031 0x12 @30.5 s;
  EV0011, EV0017, DE0017, DE002A ...). Background levels are median 0.09 / p90 0.18, so these are the same recordings mixed into the track.
  Tracks: EV0005 EV0007 EV0008 EV0011 EV0017 EV0020 EV0023 EV0024 EV0031 DE0017 DE002A DE002F DE0032 DE0036 DE003C.
  6 tracks (DE0035, DE1000, DE1035, DE2035, DE3035, DE4035) failed to decode with my demuxer [UNK].
  [INF] Cutscenes may also contain dog sounds that are not in the bank (not detectable this way), so 15–20 is a lower bound.
  Consequence: re-voicing Hewie in cutscenes means editing those tracks. Because the exact sample, its time offset and (fit-able) gain are known,
  the original bark can be subtracted almost exactly (fit gain per channel, subtract, check residual) and a real bark mixed in at the same spot,
  then re-encoded to ADX and re-muxed into the SFD. No AI source separation needed for the matched instances.

## 4. Decoded samples for listening (`feasibility/audio/`)
- `hewie_voice_SE58-70/` — Hewie's 15 voice samples triggered by the dog AI (file names carry sample index and SE IDs).
- `fiona_voice_SE2E-45/` — Fiona's voice set, for comparison.
- `D_0000_dog_footsteps/`, `D_1000_dog_footsteps_variant/` — Hewie movement banks.
- `C_0000_all/` — all 94 common-bank samples + `C_0000_index.tsv` (offset, rate, duration, peak, spectral centroid).
- `poc/EV0002_cutscene_30-60s_stereo48k.wav` — 30 s of a decoded cutscene track (proves the SFD/ADX path).
- `poc/replacement_input_synthetic.wav` → `poc/replacement_decoded_back_from_rebuilt_bank.wav` — sample-replacement proof (below).

## 5. Proof of concept: replacing a sample in the bank
`poc/vagenc.py replace C_0000.HD C_0000.BD 0x3C new.wav OUT` re-encodes a WAV to PS-ADPCM, repacks BD and rewrites the Vagi offset/rate and the
Head BD-size field. Results [OBS]: rebuilt bank decodes cleanly; the replaced sample round-trips at 23.5 dB SNR (normal for 4-bit ADPCM on noisy
material); neighbouring samples are bit-identical; at 32 kHz the bank grows by only 4 KB per 0.5 s bark. Not yet tested in-game [UNK].

## 6. What replacing Hewie with a real dog takes, per route
**Emulator route (patch the disc data):**
1. Record/licence real dog audio: ~15 distinct cues (barks ×4–5, whines ×5, growls ×2, panting, yelp) + footsteps.
2. Re-encode to PS-ADPCM at 16 kHz (same rate keeps pitch maths in Smpl/SDT valid), write into C_0000.BD samples 0x10–0x25 with `vagenc.py`.
3. Rebuild DATA.CVM with the new C_0000.HD/BD (CVM = ISO9660-like "CRI ROFS" with a CVM header; needs a CVM rebuild tool or same-size in-place patch).
   Simplest safe variant: keep every sample ≤ original byte length (pad/trim) so the BD size and all offsets stay the same → in-place patch, no ROFS rebuild.
4. Constraint — SPU2 RAM (2 MiB): worst-case resident banks C 670 KB + D 40 KB + BGM 676 KB + enemy 351 KB + stage 149 KB = 1.89 MB (90 %),
   before reverb work area and ADX stream buffers. So longer or higher-rate samples have very little headroom [INF]; staying at 16 kHz and
   similar total length (≈210 KB) is the safe target. Real barks at 16 kHz ADPCM will still sound like a real dog, slightly lo-fi.
5. No new trigger logic needed: the game picks among ~15 cues; variety can be added only by reusing the same IDs.

**Native route (recompiled or rewritten engine):**
- A native SE player that reads SDT → sample is trivial (the bank format is fully understood here); the hook point is `0x122C20`/`0x13A430`.
- Then Hewie's cues can be swapped for any-length 48 kHz files, with random variation per cue, without SPU RAM limits.
- But the native build must also reimplement `SNDDRV.IRX` behaviour (voice groups/priority, pan/3D, volume envelopes ADSR from Smpl, reverb),
  the SQ sequencer for music (MIDI-like playback on STAGEn_BANK instruments — needs an SPU2-accurate ADSR/reverb or music will sound wrong),
  and CRI ADX + Sofdec decoding (open decoders exist: vgmstream, FFmpeg `adx`/`mpeg1video`).
