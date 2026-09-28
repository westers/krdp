# Motion reference chains (AUD-FIX11 R6)

The first four packets of FFmpeg's `testsrc2` pattern at 1280x720 and 30 fps, encoded on Hal's
Radeon 780M (VCN 4.0.2, Mesa radeonsi) with the VA-API encoders KRdp uses, one packet (temporal
unit or access unit) per file: `-0` is the keyframe with its headers in band (AV1 sequence header;
HEVC VPS/SPS/PPS; H.264 SPS/PPS), `-1` to `-3` are the delta frames that follow it, each
referencing the one before. No captured desktop, user data or third-party media.

```sh
for c in av1 hevc h264; do
  q=$([ $c = av1 ] && echo "-rc_mode CQP -global_quality 200" || echo "-qp 50")
  LIBVA_DRIVER_NAME=radeonsi ffmpeg -vaapi_device /dev/dri/renderD128 \
    -f lavfi -i testsrc2=size=1280x720:rate=30 -frames:v 4 -vf format=nv12,hwupload \
    -c:v ${c}_vaapi -g 100 -bf 0 $q -f $([ $c = av1 ] && echo obu || echo $c) s.$c
done
```

The streams were split into packets at temporal delimiters (AV1, which are then left out, as
KPipeWire's packets have none) and at access-unit boundaries (HEVC/H.264). libdav1d rejects the
AV1 packet `-2` with "Invalid data found when processing input" when `-1` never reached it (the R6
symptom on cray); FFmpeg's HEVC decoder returns no picture for `-2` and `-3` in that case.
