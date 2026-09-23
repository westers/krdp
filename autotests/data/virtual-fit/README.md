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
