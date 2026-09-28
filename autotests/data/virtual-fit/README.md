# Synthetic keyframes

Blue test pictures, generated locally with FFmpeg8.0.1/libx264. No captured
desktop, user data or third-party media. One self-contained Annex-B IDR each.

Generation for each size1280x720,1920x1080,1366x768:

```sh
ffmpeg -f lavfi -i color=c=blue:s=1920x1080:r=1 -frames:v 1 \
  -c:v libx264 -preset ultrafast -tune zerolatency -threads 1 -f h264 1920x1080.h264
```

Independently checked with `ffprobe -show_entries frame=width,height,key_frame`.
1080 height and1366 width exercise SPS cropping, not just coded macroblock size.
`444.h264` uses320x200 and `-vf format=yuv444p`; `4098x200.h264`
exceeds the worker's width bound. Both are negative fixtures.

## Multi-slice regression

`1920x1080-multislice.h264` uses the same blue generator with `r=1000`,
`-profile:v baseline -crf 17 -threads 16`. It reproduces the production
encoder's sliced-thread output, which the original single-thread fixtures
did not cover.

`1280x720-production.h264` and `1920x1080-production.h264` are synthetic
frames encoded by the actual KPipeWire `LibX264Encoder` at commit183a140:
Baseline profile, Speed preference, quality80, Full color-range setter.
RGBA pixels are `(x/8+y/8)%256, (x/16)%256, 200, 255`; pts0. Frames enter
the production filter source and each fixture is the complete first key
AVPacket emitted by its normal packet sink. No captured desktop or user data.
The setter does not itself prove color-range signaling in the resulting SPS.
SHA256s respectively:

```
e9e812100209461548a3ac7451f5b6659af0bf18e80bfe9bff59b73df0d35e99
90322c7940703c2318d201be8ddf300223ea05a862c414087016aa961e11d04f
```

FFmpeg8.0.1 reports `FF_DECODE_ERROR_DECODE_SLICES` for these valid packets
when decoder error concealment is forced to zero. Keep its default error
resilience bookkeeping, but reject all reported decode/concealment errors.
Tests also remove and half-truncate every individual IDR slice in all three
multi-slice fixtures: no repaired incomplete picture may acknowledge Fit.

## HEVC and AV1 (AUD-FIX9)

`1280x720.hevc` (Annex-B, 339 bytes) and `1280x720.av1` (low-overhead OBUs, 85 bytes) are the same
blue picture as one keyframe each, encoded in hardware like KPipeWire's HEVC/AV1 path, with
FFmpeg 8.0.1 VA-API (radeonsi, Radeon 780M):

```sh
ffmpeg -vaapi_device /dev/dri/renderD128 -f lavfi -i color=c=blue:s=1280x720:r=1 \
  -vf format=nv12,hwupload -frames:v 1 -c:v hevc_vaapi -f hevc 1280x720.hevc   # av1_vaapi, -f obu
```

`ffprobe -show_entries frame=width,height,key_frame`: 1280x720, key_frame=1. SHA256s:

```
2811fdd18aee1f026273426d24739676f05b3088444561cf4a4fc8eaabf26e95  1280x720.hevc
df4e539f96e560b61f08d8d398d5879d433d670eb2acd966ca37677bc46588d8  1280x720.av1
```

## AMD VCN padding (AUD-FIX10, R5)

The same blue picture at 1920x1080 and 1366x768, one keyframe each, encoded like the fixtures above
with FFmpeg 8.0.1 VA-API (radeonsi) on Hal's Radeon 780M (VCN 4.0.2, `*-hal.*`) and on cray's Strix
Halo 8060S (VCN 4.0.5, `*-cray.*`; the files are byte-identical to Hal's):

```sh
ffmpeg -vaapi_device /dev/dri/renderD128 -f lavfi -i color=c=blue:s=1920x1080:r=1 \
  -vf format=nv12,hwupload -frames:v 1 -c:v av1_vaapi -f obu 1920x1080-hal.av1   # hevc_vaapi, -f hevc
```

| Fixture | Coded | Display | Header fields |
|---|---|---|---|
| `1920x1080-{hal,cray}.av1` | 1920x1082 | none (render = frame) | `max_frame_height_minus_1=1081`, `frame_size_override_flag=0`, `render_and_frame_size_different=0` |
| `1366x768-hal.av1` | 1408x768 | none | `max_frame_width_minus_1=1407` |
| `1920x1080-{hal,cray}.hevc` | 1920x1088 | 1920x1080 | `conformance_window_flag=1`, `conf_win_bottom_offset=4` |
| `1366x768-hal.hevc` | 1408x768 | 1366x768 | `conf_win_right_offset=21` |

AMD AV1 pads widths to 64 and heights to 16, or by 2 when height % 16 == 8 (measured on Hal for
heights 998-1350 and widths 1282-1700). H.264 (`frame_cropping_flag`) and HEVC crop correctly on both
hosts; 2560x1440 and 3840x2160 are exact in all three codecs.

`1920x1080-render1080.av1` and `1920x1080-render1076.av1` are `1920x1080-hal.av1` with an explicit
`render_size()` (1920x1080, 1920x1076) written into its frame header by `craft-av1-render-size.py`
(the 33 inserted bits grow the header by exactly 4 bytes; the tile group is unchanged). Both decode
cleanly with libdav1d. SHA256s:

```
9fb6ce6b5a33901a61bf1d968cbffe540e82f43f614dd9ebc454dd3bed99ef4e  1920x1080-hal.av1, 1920x1080-cray.av1
101f2f0da75d016ce714f9af8f6f57b456fd93ddbbed2935d5272129c5dcf173  1920x1080-hal.hevc, 1920x1080-cray.hevc
5f119d1dc31a1f4905357650a8542cd329f833e56f3399eab8c8234205affe55  1366x768-hal.av1
f176ba6c4f3252b53244a3b573aa5f92f76cae0c218847a9e0924b7e9e3fef58  1366x768-hal.hevc
38071652de20588036b5707f27213a639b1a86744adb2da754c136408611f1ba  1920x1080-render1080.av1
92c54c1b478619c7b4f43f4042ae73d77361ac0f9d368ba554a324fb9e50bbd3  1920x1080-render1076.av1
```
