# OPT-046 design: private HEVC and AV1 video for the own client

## Decision

Add two own-client-only RDPGFX vendor codecs: `0x8001` = HEVC Main 4:2:0 and `0x8002` = AV1 Profile 0 4:2:0. They carry one full-surface elementary-stream access unit per `RDPGFX_WIRE_TO_SURFACE_PDU_1`. The existing AVC420/AVC444 paths, their standard codec IDs, and all third-party behavior remain unchanged.

`HEVC` is the default private codec whenever the client advertises it: hal9000's Radeon 780M encodes it in VA-API and buzz's Intel UHD decodes it in VA-API. AV1 is negotiated only when explicitly preferred by the client and advertised as supported; buzz's current Gen9.5 hardware lacks AV1 decode, so it must not be selected by `auto`. This delivers both codec implementations without silently putting today's laptop on software AV1 decode.

The first private-codec release is 4:2:0. AVC444 remains available for text-heavy work. A dual HEVC/AV1 main+aux chroma design is deferred: it needs two independent decoder instances and a specified compositor before it can be accepted as an improvement rather than a regression.

## Capability and safety protocol

The own client sends this first `KRDPCTL` record before its `apply` record:

```json
{"v":1,"type":"codec","codecs":["hevc","av1"],"prefer":"auto"}
```

`prefer` is `auto|avc|hevc|av1`. The server responds before creating sessions:

```json
{"v":1,"type":"codec","ok":true,"selected":"hevc"}
```

Invalid values receive the existing `error invalid`; unavailable or unsupported requested codecs receive `error unsupported` and the connection falls back to AVC negotiation. A client without `KRDPCTL`, a client that omits this record, or any third-party client can receive only standard AVC RDPGFX commands. The server never emits a vendor codec ID until the capability record has been parsed for that connection.

The server's `Codec` config is extended to `auto|avc420|avc444|hevc|av1`. `auto` chooses the client-selected private HEVC codec when available, otherwise retains current standard AVC444/AVC420 selection. Pinned `hevc`/`av1` requires the corresponding `KRDPCTL` capability and otherwise falls back to AVC420 with a warning. The own client exposes `Private codec` in Advanced settings and a `codec=auto|avc|hevc|av1` debug action.

## Wire format

The vendor commands use the generic `RDPGFX_SURFACE_COMMAND` fields instead of an AVC `extra` structure:

- `codecId`: `0x8001` or `0x8002`
- `contextId`: zero
- `data` / `length`: one Annex-B HEVC or AV1 temporal-unit byte sequence from FFmpeg
- `left/top/right/bottom`: the full surface rectangle

The KRDP server serializer must write the generic `WIRE_TO_SURFACE_1` form for vendor IDs. The own client's `GraphicsPipeline::onSurfaceCommand` recognizes those IDs, decodes the data itself, paints the decoded RGB frame into the GDI primary buffer at the command rectangle, marks that rectangle invalid, and returns `CHANNEL_RC_OK` without calling FreeRDP's standard codec handler. The ordinary `EndFrame` path and its RDPGFX acknowledgements therefore remain the pacing mechanism.

Vendor data is never passed to FreeRDP's AVC decoder. Decoder flush/recreate occurs on ResetGraphics, size change, codec change, decode error, or a missing keyframe; the server requests an IDR for the affected session after such an error. A client treats a non-keyframe before successful decoder initialization as a dropped delta, not a fatal session error.

## Encoder and decoder

KPipeWire gains a parameterized VA-API 4:2:0 encoder whose DRM-prime input graph is identical to `H264VAAPIEncoder` and whose codec-specific contexts are `hevc_vaapi` and `av1_vaapi`. Both use `async_depth=1`, no B-frames, and CQP. The current 0–100 quality mapping remains QP 40–12 until measurements demonstrate codec-specific mapping is necessary. `PipeWireEncodedStream` packet ownership and keyframe signals are unchanged.

The client links `libavcodec`, `libavutil`, and `libswscale`. It first attempts a VA-API HEVC decoder using the render node selected by FFmpeg; it falls back to FFmpeg software decoding if VA-API setup or an individual hardware decode fails. AV1 follows the same behavior, but its UI text explicitly identifies a software fallback. Decoded YUV frames are converted to the existing opaque `QImage::Format_RGB32` primary surface; a later slice may export decoded VA surfaces directly to a Qt texture.

## Acceptance

1. Unit tests prove codec record validation/selection, vendor command construction, and that ordinary clients cannot select a vendor codec.
2. KPipeWire tests encode and software-decode one HEVC and AV1 VA-API frame on the Radeon; they skip only when VA-API is absent.
3. Isolated :3390 test-server runs from buzz verify HEVC auto negotiation, first IDR display, keyframe recovery after ResetGraphics, frame acknowledgements, and no output-restoration failure.
4. AV1 is accepted on hardware only where the client advertises it. On buzz, an explicit AV1 test must report whether software decode sustains the chosen desktop rate; it must not be selected by auto.
5. Compare same-scene AVC420 vs HEVC and AVC420 vs AV1: bitrate, Y/U/V PSNR, end-to-end frame latency, and client decode CPU. Claims of savings require those numbers.

