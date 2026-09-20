# OPT-046 KPipeWire plan: VA-API HEVC and AV1 encoders

1. Extract the common DRM-prime and software-upload graph setup from `H264VAAPIEncoder` into a codec-parameterized VA-API encoder. Preserve the existing H.264 public behavior and AVC444 composite encoder.
2. Add `hevc_vaapi` and `av1_vaapi` context builders with VAAPI pixel format, CQP, `async_depth=1`, no B-frames, quality reopen, and keyframe-on-demand. Add a stream codec setter that is applied before start and recreates the encoder safely.
3. Add GPU autotests that open each encoder on the render node, encode a synthetic frame, and software-decode it. Export numbered patches starting at 0020 only after the KPipeWire commit is accepted; do not push the private branch.

