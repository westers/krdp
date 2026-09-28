# AMD VAAPI encoder padding

This was measured on 2026-09-28 with FFmpeg 8.0.1 VA-API and Mesa 26.0.8 radeonsi, on Hal's Radeon 780M
(VCN 4.0.2) and on cray's Strix Halo 8060S (VCN 4.0.5). Each encode was one or two frames of `testsrc2`
with `-bf 0`. The coded and display sizes were read from the bitstream with `trace_headers`. The full
table, sweeps and raw headers are in `~/dev/rdp/evidence/2026-09-28-amd-padding/TABLE.md`.

## Rules (all even sizes, both hosts identical)

| Codec | Coded width | Coded height | Display size in the stream |
|---|---|---|---|
| H.264 (`h264_vaapi`) | round up to 16 | round up to 16 | yes: SPS `frame_cropping` gives the exact requested size |
| HEVC (`hevc_vaapi`) | round up to **64** | round up to 16 | yes: SPS `conformance_window` gives the exact requested size |
| AV1 (`av1_vaapi`) | round up to **64** | round up to 16, **but +2 when H % 16 == 8** | **no**: `render_and_frame_size_different = 0`, `frame_size_override_flag = 0` |

- AV1 has no display size of its own. The picture's real size is in the top-left corner of the coded
  frame, and the extra columns and rows are padding.
- An AV1 sweep on Hal covered 2765 sizes: 1280×H for every even H from 64 to 2400, and W×720 for every
  even W from 64 to 2806 and every sixth W up to 4160. Every accepted size matched the rule, and there
  were no exceptions.
- Odd requested sizes are floored to even in every codec. For example, 1917×1063 displays as 1916×1062
  in H.264 and HEVC, and AV1 codes it as 1920×1072 (the rule applied to 1916×1062). KRdp only ever
  requests even sizes.
- The encoder limits come from the aligned surface size. H.264 accepts 128–4096 × 128–4096, so it
  rejects 5120×1440 and 5120×2160. HEVC accepts widths 130–8192 and heights 128–4352, and AV1 accepts
  widths 128–8192 and heights 128–4352. KRdp caps outputs at 4096 anyway.
- Hal and cray give byte-identical HEVC and AV1 streams. Their H.264 streams differ only in FFmpeg's
  driver-name SEI.

## Common resolutions (coded size; `=` means it equals the requested size)

| Requested | H.264 | HEVC | AV1 (no display size) |
|---|---|---|---|
| 800×600 | 800×608 (crop) | 832×608 (conf) | **832×602** |
| 1280×720, 1280×800, 1280×1024, 1920×1200, 2560×1440, 2560×1600, 3840×2160 | = | = | = |
| 1366×768 | 1376×768 (crop) | 1408×768 (conf) | **1408×768** |
| 1440×900 | 1440×912 (crop) | 1472×912 (conf) | **1472×912** |
| 1600×900 | 1600×912 (crop) | 1600×912 (conf) | **1600×912** |
| 1680×1050 | 1680×1056 (crop) | 1728×1056 (conf) | **1728×1056** |
| 1920×1080 | 1920×1088 (crop) | 1920×1088 (conf) | **1920×1082** |
| 2256×1504 | = | 2304×1504 (conf) | **2304×1504** |
| 2560×1080 | 2560×1088 (crop) | 2560×1088 (conf) | **2560×1082** |
| 2880×1800 | 2880×1808 (crop) | 2880×1808 (conf) | **2880×1802** |
| 3000×2000 | 3008×2000 (crop) | 3008×2000 (conf) | **3008×2000** |
| 3440×1440 | = | 3456×1440 (conf) | **3456×1440** |
| 1080×1920 | 1088×1920 (crop) | 1088×1920 (conf) | **1088×1920** |

## What KRdp and krdp-client do about it

- **KRdp** (`server/H264KeyframeSize.{h,cpp}`, `encodedKeyframeShows()`, since `e1a8202` / `f9c57f5`)
  accepts a keyframe as proof of an output when its display size equals the requested size. H.264 and
  HEVC always have one.
- For AV1 without a render size, KRdp instead accepts a coded size that lies, in each dimension,
  between the requested size and the requested size rounded up to 64×16. Every measured AMD size lies
  inside that band.
- **krdp-client** (`src/rdp/PrivateFrameGeometry.h`, `hasKnownAv1Padding()`) crops a decoded AV1
  picture to the top-left of the RDPGFX surface. It does this only when the decoded size is exactly the
  rule applied to the surface size, and only for even surface sizes. The rule matched every
  measurement.

## Known limits (not bugs in the rule)

- **Ambiguous AV1 keyframes.** Several requested sizes map to the same AMD AV1 size. For example,
  1074–1088 map to 1088, except 1080, which maps to 1082. A stale encoder's keyframe can therefore
  prove a nearby size. One example is a 1920×1080 keyframe (1082 rows) for a 1920×1076 or 1920×1082
  request. The display-size check cannot tell these apart, and KRdp documents this as residual risk.
- **Other hardware.** The rules were measured only on VCN 4.0.x. The client's source comment says Mesa
  changes AV1 padding for VCN 5, which is untested here.
