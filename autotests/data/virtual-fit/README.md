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
