# OPT-045 AVC444 — private KPipeWire (`Avc444Split`, NAL rewriter, two-context produce path) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the private KPipeWire an AVC444 encode path: a bit-exact, runtime-dispatched RGB → (main 4:2:0, auxiliary chroma 4:2:0) split; a second, intra-only `h264_vaapi` context for the chroma pictures whose access units are rewritten into non-reference pictures of the main stream; a `PipeWireEncodedStream` mode that pairs both by timestamp into `Packet::data()` + `Packet::aux()` under a motion/at-rest policy, with the aux stream switchable and the per-frame cost measured.

**Architecture:** `Avc444Split` (new, `src/avc444split*.cpp`) is a pure unit — RGBX/BGRX bytes in, I420 main + I420 aux planes out, scalar reference plus `FreeRdp` (libfreerdp primitives), `Avx2` and `Avx512` kernels chosen at runtime, all bit-exact; its layout is pinned against libfreerdp's *decoder* primitive `YUV420CombineToYUV444`, the code every FreeRDP client runs. The **main stream** is the existing `H264VAAPIEncoder` context (gop 600, IDR on demand, quality reopen), fed in 444 mode with the split's main picture through a `buffer(yuv420p) → format=nv12 → hwupload → buffersink` graph — its P chain never sees an aux picture, so the luma stream costs what AVC420 costs today. The **aux stream** is a second `h264_vaapi` context, intra-only (`gop_size = 1`), same size/QP/options; `Avc444NalRewriter` (new, pure) turns each aux access unit into a non-reference, non-IDR I picture the client's single decoder accepts under the main SPS (aux SPS/SEI dropped, aux PPS renumbered to id 1 and sent inline, slice headers rewritten with `nal_ref_idc 0`, `frame_num = mainFrameNum + 1`, no `idr_pic_id`/`dec_ref_pic_marking`). `H264VAAPIAvc444Encoder` (new, an `Encoder` owning both contexts) downloads and splits on the produce thread, applies the aux policy (aux with a frame only when the gap since the previous encoded frame ≥ 100 ms or the main is a keyframe; one aux-only refresh after 150 ms at rest), and `Avc444PacketPairer` (pure) pairs the two encoders' packets by pts into one `Packet` per frame — `data()` empty for an at-rest chroma refresh. `PipeWireBaseEncodedStream` gains `setChromaMode()`, `activeChromaMode()`, `setAuxStreamEnabled()`; `PipeWireEncodedStream` gains `Packet::aux()/auxIsKey()` and a once-per-second `chromaTimingReported()` signal.

**Tech Stack:** C++20, Qt 6 / KF6 (ECM, QtTest), FFmpeg 8.0 (`libavcodec` `h264_vaapi` + software `h264` decoder for the tests, `libavfilter` `hwupload`, `libavutil` buffer pools), libva/Mesa radeonsi on the Radeon 780M (`supported references: 1 / 1`, `max_b_frames = 0`, CABAC, `pic_order_cnt_type 2`, one slice per picture — measured 2026-09-19, `/tmp/claude-1000/-home-westers-dev-rdp/83d17fcc-96b3-4d30-b88b-2e3d0c725a20/scratchpad/refcost/`), libfreerdp3 3.31 primitives (`primitives_get()`, `RGBToAVC444YUV[v2]`, `YUV420CombineToYUV444`), GCC function-level `target("avx2")` / `target("avx512f,avx512bw,avx512vl,avx512vbmi")` attributes, `__builtin_cpu_supports`, H.264 syntax per ITU-T H.264 §7.3.2.1.1 (SPS), §7.3.2.2 (PPS), §7.3.3 (slice header), §7.4.3 (`frame_num` for non-reference pictures), §8.2.1.3 (POC type 2).

**Spec:** `~/dev/krdp/docs/superpowers/specs/2026-09-19-opt-045-avc444-design.md` — §2 R2/R4/R8, §4.1, **§4.2 as amended 2026-09-19** (two contexts + rewriter, aux policy), §7 unit tests. Companion plan (server + client): `~/dev/krdp/docs/superpowers/plans/2026-09-19-opt-045-krdp-avc444.md` — its S2 consumes the API this plan produces; its S4 harness is the hardware smoke of this encoder with a real screencast.

## Global Constraints

- Repository `~/dev/kpipewire`, branch `westers/opt-015` (HEAD `a181fa2`, OPT-048; patch `0011` exported). Every commit of this plan is exported right after it lands: `git -C ~/dev/kpipewire format-patch -1 HEAD --start-number N -o ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/` with N = **0012** for the first commit of this plan and +1 per commit thereafter (check `ls ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/ | tail -1` before each export; never reuse or renumber an existing patch).
- Two build directories. Tests: `cmake -S ~/dev/kpipewire -B ~/dev/kpipewire/build-tests -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF -DCMAKE_INSTALL_PREFIX=/tmp/kpipewire-tests-prefix` once, then `cmake --build ~/dev/kpipewire/build-tests -j16 --target <test>` and `ctest --test-dir ~/dev/kpipewire/build-tests -R <name> --output-on-failure` (`src/autotests/CMakeLists.txt` uses `ecm_add_test`, gated by `BUILD_TESTING` in `src/CMakeLists.txt:207-209`; `videodamagetest` is the existing example). Install/relink: **only** `~/dev/krdp/scripts/build-kpipewire.sh` (configures `~/dev/kpipewire/build` with `-DBUILD_TESTING=OFF`, installs into `~/dev/krdp/.deps/kpipewire`, rebuilds KRDP, runs `scripts/check-kpipewire-link.sh`). Zero new warnings in either build.
- **`build-kpipewire.sh`'s install replaces the library the LIVE service maps — run it only with no client on 3389 (`ss -tnp | grep ':3389' | grep ESTAB` must print nothing) and confirm the live MainPID afterwards** (`systemctl --user show -p MainPID app-org.kde.krdpserver` before and after must be identical; the running process keeps its already-mapped library and only picks the new one up on its next restart, which is Steve's call). Never restart the live service in this plan.
- Hardware rules (binding): never restart the live service (3389) while a client is connected; test instances use their own `XDG_CONFIG_HOME` and port (3390 via `~/dev/krdp-client/scripts/test-server.sh`, or 3393 by hand); never `kwriteconfig6 --notify` (it broadcasts by config-file name to every running `krdpserver`, the live one included); nothing is opened on hal9000's display; no sudo; no X11 tooling (`sdl-freerdp3`/`wlfreerdp3`, never `xfreerdp`); nothing on buzz beyond `scripts/deploy.sh dev|deb` and no package changes on buzz (all FreeRDP packages there are on `apt-mark hold` on purpose). The GPU tests in this plan (`avc444streamtest`, K5's smoke) run on hal9000's 780M and touch no display.
- Colour matrix: FreeRDP's own RGB→YUV constants, `Y = (54R + 183G + 18B) >> 8`, `U = ((-29R - 99G + 128B) >> 8) + 128`, `V = ((128R - 116G - 12B) >> 8) + 128` (arithmetic shift, no clipping needed — the full-range BT.709 pair that libfreerdp's `YUV420ToRGB`/`YUV444ToRGB` primitives invert). Every variant is bit-exact with the scalar reference; the AVC420 dma-buf path (`H264VAAPIEncoder` in `Input::DmaBuf` mode) is not touched.
- Wire layout (MS-RDPEGFX 3.3.8.3.2 / 3.3.8.3.3 as libfreerdp's `general_ChromaV1ToYUV444` / `general_ChromaV2ToYUV444` read it) — main: Y full size, U/V = truncated mean of each 2×2 block. **v2 aux:** Y plane row `y`: columns `[0, W/2)` = U444(2x+1, y), columns `[W/2, W)` = V444(2x+1, y); U plane row `y'`: `[0, W/4)` = U444(4x, 2y'+1), `[W/4, W/2)` = V444(4x, 2y'+1); V plane row `y'`: `[0, W/4)` = U444(4x+2, 2y'+1), `[W/4, W/2)` = V444(4x+2, 2y'+1) (`W/2`, `W/4` are integer divisions of the full frame width; when `W % 4 != 0` the U-part of the U plane has `ceil(W/4)` entries and its last one lands on column `W/4`, the V-part's first — a one-column overlap inherent to the format, also in libfreerdp's encoder and decoder; the block written later, i.e. the right-most one, wins, and the SIMD kernels leave that column to the scalar tail so the order is the same). **v1 aux:** Y plane in 16-row blocks, block `k` rows `16k+j` (`j < 8`) = U444(x, 2(8k+j)+1), rows `16k+8+j` = V444(x, 2(8k+j)+1) — the split's v1 aux Y plane is `roundUp16(H)` rows tall; U plane row `y'` = U444(2x+1, 2y'), V plane likewise. Positions the wire format leaves unspecified (ragged last column/row, v1 block rows whose source row is ≥ H) are written as 128 so every byte of every plane is defined.
- **Picture geometry:** every picture the encoders see is exactly `W × H` (the aux is decoded under the main SPS, so its coded size is the main's; rows ≥ H are cropped by the SPS and unreachable). Consequence for **v1** with `H % 16 ≠ 0`: the last partial 16-row block's V rows (block rows `16k+8+j`) fall at or beyond H — with `r = H % 16` the V444 samples of the last `ceil(r/2) − max(0, r − 8)` odd rows never reach the client (libfreerdp's `general_ChromaV1ToYUV444` reads them from the decoded picture's rows ≥ H, i.e. from padding). v2 is height-exact and is what every FreeRDP ≥ 2 and mstsc negotiates; v1 is kept for caps-10.0/10.1 clients with this documented limitation. The split still fills its `roundUp16(H)` v1 plane (the pooled aux buffer has that many rows; the AVFrame's height is H).
- **Two contexts, one client-side stream (spec §4.2 as amended):** the main context is `H264VAAPIEncoder` as today (gop 600, IDR on demand, quality reopen), in 444 mode fed the split's main picture by upload; the aux context is a second `h264_vaapi` with `gop_size = 1` (every picture an IDR), otherwise identical parameters (size, profile, QP, `async_depth=1`, `rc_mode=CQP`, time base). Each aux access unit is rewritten by `Avc444NalRewriter` into a **non-reference, non-IDR I picture** under the main SPS: SPS, SEI, AUD and filler NALs dropped; the aux PPS renumbered to `pic_parameter_set_id = 1` and emitted inline before the slices of every aux AU; each slice NAL header byte → `nal_ref_idc = 0, nal_unit_type = 1`; `first_mb_in_slice`, `slice_type` copied; `pic_parameter_set_id → 1`; `frame_num → (mainFrameNum + 1) mod MaxFrameNum` (width from the main SPS); `idr_pic_id` dropped; POC type 2 → nothing to write, type 0 → `pic_order_cnt_lsb = (mainPocLsb + 1) mod MaxPicOrderCntLsb` (and `delta_pic_order_cnt_bottom` copied when the PPS asks for it), type 1 → refused; `redundant_pic_cnt`, `slice_qp_delta`, the deblocking fields copied per the PPS flags; `dec_ref_pic_marking` (the IDR's two flag bits) dropped; CABAC: re-align with `cabac_alignment_one_bit`s and copy the slice data bytes; CAVLC: bit-shift the remainder; emulation prevention removed before and re-applied after. Precondition, checked at stream start and on every SPS/PPS change: the aux SPS's parse-relevant fields (profile_idc, chroma_format_idc, bit depths, qpprime_y_zero_transform_bypass_flag, scaling matrices, log2_max_frame_num, pic_order_cnt_type (+ lsb bits), frame_mbs_only_flag, mb_adaptive_frame_field_flag, direct_8x8_inference_flag, picture size, cropping) equal the main SPS's — `max_num_ref_frames`, `level_idc`, `gaps_in_frame_num_value_allowed_flag` and the VUI may differ. On mismatch: warning + the session falls back to `Yuv420` (`activeChromaMode()` reports it). POC type 2 forbids two consecutive non-reference pictures, so the produce side never emits two aux pictures without a main between them.
- **Aux policy (produce thread; constants with environment overrides for tuning during acceptance):** an aux picture is split-fed, encoded and paired with a frame when `setAuxStreamEnabled(true)` AND (the gap since the previous encoded frame ≥ `KPIPEWIRE_AVC444_MOTION_GAP_MS` (default 100) OR the main is (about to be) a keyframe); during fast motion frames go out luma-only. At rest: when the last emitted frame was luma-only and no frame has arrived for `KPIPEWIRE_AVC444_REST_MS` (default 150), the aux planes of that last frame are encoded and emitted as ONE aux-only `Packet` (empty `data()`; KRDP sends it as `LC = 2`), never a second one until a main has gone out. `setAuxStreamEnabled(false)` (the congestion rung) suppresses both. The per-second timing line reports `aux sent / skipped-motion / rest-refresh` counts.
- Threads as today: `filterFrame()` on the produce thread (`PipeWireProduce::input`), `encodeFrame()` on `PipeWireProduce::passthrough`, `receivePacket()` on `PipeWireProduce::output`. Shared state between them is atomic or under a mutex; each context's `m_avCodecMutex` guards its `m_avCodecContext` (as `Encoder` does today). An encoded main picture is never dropped after `avcodec_send_frame` succeeded; aux pictures are independent (intra) and may be dropped.
- Commit messages end with `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`; no amend/rebase/stash; no push. KRDP's own tests (`ctest --test-dir ~/dev/krdp/build --output-on-failure`) stay green after the relink.

---

## File structure

| File | Responsibility |
|---|---|
| `src/avc444split_p.h` (new) | The split's interface: `Version`, `Variant`, `PixelOrder`, `Plane`/`I420`/`Planes`, `Input`, `auxHeight()`, `best()`, `available()`, `name()`, `split()` |
| `src/avc444split.cpp` (new) | Scalar reference (per-2×2-block), `FreeRdp` variant (`primitives_get()->RGBToAVC444YUV[v2]`, `#ifdef HAVE_FREERDP_PRIMITIVES`), dispatch + `KPIPEWIRE_AVC444_SPLIT` override |
| `src/avc444split_avx2.cpp`, `src/avc444split_avx512.cpp` (new) | The SIMD kernels, one TU each, function-level `target` attributes so the rest of the library is compiled without `-mavx*` |
| `src/autotests/avc444splittest.cpp` (new) | Layout pin via libfreerdp's decoder primitive, bit-exactness vs `primitives_get_generic()`, variants vs scalar on random and odd sizes, env override, timing print |
| `src/h264bitstream_p.h` (new, header-only) | `H264Bits::{BitReader, BitWriter, ebspToRbsp, rbspToEbsp, splitAnnexB, NalHeader}` — pure |
| `src/avc444nalrewriter_p.h`, `src/avc444nalrewriter.cpp` (new) | `Avc444NalRewriter`: SPS/PPS/slice-header parsing, the equality check, `MainStreamState` tracking from main packets, aux AU rewrite — pure (no FFmpeg) |
| `src/vaapih264_p.h`, `src/vaapih264.cpp` (new) | `VaapiH264::{Params, encodingOptions, openContext, UploadGraph, createUploadGraph}` — the one place `h264_vaapi` is configured, shared by the encoder classes and the GPU test |
| `src/autotests/avc444nalrewritertest.cpp` (new) | bit reader/writer, EBSP↔RBSP, PPS renumber, slice header rewrite on synthetic CABAC/CAVLC headers, SPS equality |
| `src/autotests/avc444streamtest.cpp` (new) | **GPU go/no-go**: real main + intra aux contexts on the 780M, rewritten and interleaved, decoded by libavcodec's software `h264` (skipped without VA-API) |
| `src/avc444pairer_p.h` (new, header-only) | `Avc444PacketPairer`: pts pairing across two encoders, luma-only frames, aux-only refresh, stragglers — pure |
| `src/autotests/avc444pairertest.cpp` (new) | The pairing rules |
| `src/avc444planes_p.h` (new, header-only) | `PlaneLayout`: byte offsets/strides of one pooled picture buffer — pure |
| `src/autotests/avc444planestest.cpp` (new) | Sizes/offsets |
| `src/pipewirebaseencodedstream.{h,cpp}` | `ChromaMode`, `setChromaMode/chromaMode/activeChromaMode`, `setAuxStreamEnabled/auxStreamEnabled`, `KPIPEWIRE_CHROMA_MODE` override |
| `src/pipewireencodedstream.{h,cpp,_p.h}` | `Packet(isKey, data, aux, auxIsKey)`, `aux()`, `auxIsKey()`; `ChromaTiming` + `chromaTimingReported()`; `PipeWireEncodeProduce::processPacketPair()`, `reportChromaTiming()` |
| `src/pipewireproduce.{cpp,_p.h}` | `m_chromaMode`, `m_activeChromaMode`, `m_auxStreamEnabled`, `processPacketPair()` default, `reportChromaTiming()` default, `makeEncoder()` choosing `H264VAAPIAvc444Encoder` |
| `src/encoder.{cpp,_p.h}` | `avCodecContext()`, `setQuality()`, `requestKeyFrame()` virtual; `m_packetSink`; `DmaBufHandler` moved up from `SoftwareEncoder` (+ `downloadToImage()`, `maybeDumpRgba()`) |
| `src/h264vaapiencoder.{cpp,_p.h}` | `Input::{DmaBuf, Yuv420Upload}`, `gopSize`, `queueSoftwareFrame()`, `keyFrameRequested()`/`qualityChangePending()` readers; `createCodecContext()` via `VaapiH264` |
| `src/h264vaapiavc444encoder.{cpp,_p.h}` (new) | The 444 encoder: two `H264VAAPIEncoder`s, pooled pictures, download → split → policy → queue, pair-aware `encodeFrame()`/`receivePacket()`, rewriter + tracker, rest-refresh timer, timing, `KPIPEWIRE_DUMP_RGBA` |
| `CMakeLists.txt`, `src/CMakeLists.txt`, `src/autotests/CMakeLists.txt` | `WITH_FREERDP_PRIMITIVES` option, new sources, tests |
| `~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/0012-…0016-…` | Exported patches |

Task order: K1 split → K2 SIMD → **K3 rewriter + GPU go/no-go** (the whole approach is decided here, before anything is built on it) → K4 API/pairer/planes → K5 encoder + policy → K6 patches/relink.

---

### Task K1: `Avc444Split` — scalar reference, `FreeRdp` variant, layout-pinning tests

**Files:**
- Create: `src/avc444split_p.h`, `src/avc444split.cpp`, `src/autotests/avc444splittest.cpp`
- Modify: `CMakeLists.txt` (FreeRDP option), `src/CMakeLists.txt` (sources + define + link), `src/autotests/CMakeLists.txt` (test)

**Interfaces:**
- Consumes: `primitives_get()`, `primitives_get_generic()`, `primitives_t::RGBToAVC444YUV`, `RGBToAVC444YUVv2`, `YUV420CombineToYUV444` (`/usr/include/freerdp3/freerdp/primitives.h:195-211, 290-294, 317-325`), `PIXEL_FORMAT_RGBX32` / `PIXEL_FORMAT_BGRX32` (`freerdp/codec/color.h:80-82`; FreeRDP names 32-bit formats by their byte order in memory).
- Produces (used by K2, K5 and the tests):

```cpp
// src/avc444split_p.h
#pragma once
#include <cstdint>
namespace Avc444Split
{
enum class Version { V1 = 1, V2 = 2 };                    // RDPGFX_CODECID_AVC444 / AVC444v2 layouts
enum class Variant { Auto, Scalar, FreeRdp, Avx2, Avx512 };
enum class PixelOrder { Rgbx, Bgrx };                      // byte order in memory of each 4-byte pixel
struct Plane { uint8_t *data = nullptr; int stride = 0; };
struct I420 { Plane y, u, v; };                             // u/v are ceil(w/2) x ceil(h/2)
struct Planes { I420 main; I420 aux; };                     // aux.y is w x auxHeight(version, h)
struct Input { const uint8_t *pixels = nullptr; int stride = 0; int width = 0; int height = 0; PixelOrder order = PixelOrder::Rgbx; };

constexpr int roundUp16(int v) { return (v + 15) & ~15; }
constexpr int auxHeight(Version version, int height) { return version == Version::V1 ? roundUp16(height) : height; }

Variant best();                       // KPIPEWIRE_AVC444_SPLIT override, else Avx512 -> Avx2 -> FreeRdp -> Scalar
bool available(Variant variant);      // Auto is always available; Scalar always; others per CPU / build
const char *name(Variant variant);    // "auto" "scalar" "freerdp" "avx2" "avx512"
// Every byte of every plane inside the declared sizes is written. variant Auto = best().
// An unavailable explicit variant falls back to Scalar (warning once).
void split(const Input &in, Version version, Planes &out, Variant variant = Variant::Auto);

// Internal: exact per-pixel matrix (FreeRDP's), shared by every variant and by the tests.
inline uint8_t rgbToY(int r, int g, int b) { return uint8_t((54 * r + 183 * g + 18 * b) >> 8); }
inline uint8_t rgbToU(int r, int g, int b) { return uint8_t(((-29 * r - 99 * g + 128 * b) >> 8) + 128); }
inline uint8_t rgbToV(int r, int g, int b) { return uint8_t(((128 * r - 116 * g - 12 * b) >> 8) + 128); }
// Defined in avc444split.cpp; the SIMD TUs call these for the ragged tail (columns >= x0 of rows [y0, y0+2) ...).
void splitScalar(const Input &in, Version version, Planes &out);
void splitScalarRegion(const Input &in, Version version, Planes &out, int x0, int y0, int x1, int y1); // 2x2 blocks whose even corner is in [x0,x1) x [y0,y1)
void fillUndefinedPositions(const Input &in, Version version, Planes &out); // the 128 pre-fill every entry point runs first
void splitAvx2(const Input &in, Version version, Planes &out);    // avc444split_avx2.cpp   (K2)
void splitAvx512(const Input &in, Version version, Planes &out);  // avc444split_avx512.cpp (K2)
}
```

- [ ] **Step 1: CMake — the FreeRDP option and the test target**

`CMakeLists.txt`, after `pkg_check_modules(LIBVA-drm …)` (line 42):

```cmake
option(WITH_FREERDP_PRIMITIVES "Use libfreerdp's YUV primitives as an Avc444Split variant and as the test oracle" ON)
if (WITH_FREERDP_PRIMITIVES)
    pkg_check_modules(FreeRDP IMPORTED_TARGET freerdp3 winpr3)
    if (NOT FreeRDP_FOUND)
        message(STATUS "freerdp3/winpr3 not found; Avc444Split::Variant::FreeRdp disabled")
        set(WITH_FREERDP_PRIMITIVES OFF)
    endif()
endif()
```

`src/CMakeLists.txt`: add `avc444split.cpp avc444split_avx2.cpp avc444split_avx512.cpp` to the `add_library(KPipeWireRecord …)` source list (after `libwebpencoder.cpp`, line 149). K1 creates the two SIMD files as stubs that forward to `splitScalar` (K2 fills them). After the `target_link_libraries(KPipeWireRecord …)` block (line 151-155):

```cmake
if (WITH_FREERDP_PRIMITIVES)
    target_compile_definitions(KPipeWireRecord PRIVATE HAVE_FREERDP_PRIMITIVES)
    target_link_libraries(KPipeWireRecord PRIVATE PkgConfig::FreeRDP)
endif()
```

`src/autotests/CMakeLists.txt` (the split sources are compiled into the test, so nothing needs exporting from the library — same reasoning as KRDP's `LayoutControlTest`):

```cmake
set(avc444_split_sources ../avc444split.cpp ../avc444split_avx2.cpp ../avc444split_avx512.cpp)
ecm_add_test(avc444splittest.cpp ${avc444_split_sources} TEST_NAME avc444splittest LINK_LIBRARIES Qt::Test Qt::Core)
target_include_directories(avc444splittest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if (WITH_FREERDP_PRIMITIVES)
    target_compile_definitions(avc444splittest PRIVATE HAVE_FREERDP_PRIMITIVES)
    target_link_libraries(avc444splittest PkgConfig::FreeRDP)
endif()
```

- [ ] **Step 2: Write the failing tests**

`src/autotests/avc444splittest.cpp`:

```cpp
/*
    SPDX-FileCopyrightText: 2026 Steve Westerhouse <steve.westerhouse@origami-analytics.com>
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "avc444split_p.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTest>

#include <vector>

#ifdef HAVE_FREERDP_PRIMITIVES
extern "C" {
#include <freerdp/codec/color.h>
#include <freerdp/primitives.h>
}
#endif

using namespace Avc444Split;

namespace
{
struct Frame {
    int width, height;
    PixelOrder order;
    std::vector<uint8_t> pixels; // stride = width * 4 + 12 (deliberately not a multiple of 16)
    int stride() const { return width * 4 + 12; }
    Input input() const { return {pixels.data(), stride(), width, height, order}; }
    void randomize(quint32 seed)
    {
        QRandomGenerator rng(seed);
        pixels.resize(size_t(stride()) * height);
        for (auto &b : pixels) b = uint8_t(rng.bounded(256));
    }
    // r/g/b of pixel (x, y) as the split must read them
    void rgb(int x, int y, int &r, int &g, int &b) const
    {
        const uint8_t *p = pixels.data() + size_t(y) * stride() + size_t(x) * 4;
        if (order == PixelOrder::Rgbx) { r = p[0]; g = p[1]; b = p[2]; } else { b = p[0]; g = p[1]; r = p[2]; }
    }
};

// Owns the six planes with strides that are not the width, filled with a sentinel.
struct PlaneSet {
    int w, h, hw, hh, auxH;
    std::vector<uint8_t> mainY, mainU, mainV, auxY, auxU, auxV;
    Planes planes;
    PlaneSet(int width, int height, Version version, uint8_t fill = 0xA5)
        : w(width), h(height), hw((width + 1) / 2), hh((height + 1) / 2), auxH(auxHeight(version, height))
    {
        auto make = [fill](std::vector<uint8_t> &v, Plane &p, int width, int rows) {
            p.stride = width + 24;
            v.assign(size_t(p.stride) * rows, fill);
            p.data = v.data();
        };
        make(mainY, planes.main.y, w, h); make(mainU, planes.main.u, hw, hh); make(mainV, planes.main.v, hw, hh);
        make(auxY, planes.aux.y, w, auxH); make(auxU, planes.aux.u, hw, hh); make(auxV, planes.aux.v, hw, hh);
    }
    static int compare(const Plane &a, const Plane &b, int width, int rows, const char *what, int maxDiff = 0, uint8_t skipIfB = 0, bool useSkip = false)
    {
        int diffs = 0;
        for (int y = 0; y < rows; ++y) for (int x = 0; x < width; ++x) {
            const int va = a.data[y * a.stride + x], vb = b.data[y * b.stride + x];
            if (useSkip && vb == skipIfB) continue;
            if (std::abs(va - vb) > maxDiff) {
                if (diffs < 5) qWarning("%s differs at (%d,%d): %d vs %d", what, x, y, va, vb);
                ++diffs;
            }
        }
        return diffs;
    }
    int compareAll(const PlaneSet &o, int maxMainChromaDiff = 0, bool skipSentinelInOther = false) const
    {
        int d = 0;
        d += compare(planes.main.y, o.planes.main.y, w, h, "main.y", 0, 0xA5, skipSentinelInOther);
        d += compare(planes.main.u, o.planes.main.u, hw, hh, "main.u", maxMainChromaDiff, 0xA5, skipSentinelInOther);
        d += compare(planes.main.v, o.planes.main.v, hw, hh, "main.v", maxMainChromaDiff, 0xA5, skipSentinelInOther);
        d += compare(planes.aux.y, o.planes.aux.y, w, auxH, "aux.y", 0, 0xA5, skipSentinelInOther);
        d += compare(planes.aux.u, o.planes.aux.u, hw, hh, "aux.u", 0, 0xA5, skipSentinelInOther);
        d += compare(planes.aux.v, o.planes.aux.v, hw, hh, "aux.v", 0, 0xA5, skipSentinelInOther);
        return d;
    }
};
}

class Avc444SplitTest : public QObject
{
    Q_OBJECT
private:
    // Full-resolution reference planes straight from the matrix: what a 4:4:4 decoder should get back.
    static void reference444(const Frame &f, std::vector<uint8_t> &Y, std::vector<uint8_t> &U, std::vector<uint8_t> &V)
    {
        Y.resize(size_t(f.width) * f.height); U.resize(Y.size()); V.resize(Y.size());
        for (int y = 0; y < f.height; ++y) for (int x = 0; x < f.width; ++x) {
            int r, g, b; f.rgb(x, y, r, g, b);
            Y[y * f.width + x] = rgbToY(r, g, b); U[y * f.width + x] = rgbToU(r, g, b); V[y * f.width + x] = rgbToV(r, g, b);
        }
    }
    // The 2x2 truncated mean the main U/V plane must carry; missing neighbours replicate the even corner (FreeRDP's rule).
    static uint8_t mean2x2(const std::vector<uint8_t> &p, int w, int h, int x, int y)
    {
        const int x1 = x + 1 < w ? x + 1 : x, y1 = y + 1 < h ? y + 1 : y;
        return uint8_t((p[y * w + x] + p[y * w + x1] + p[y1 * w + x] + p[y1 * w + x1]) / 4);
    }

private Q_SLOTS:
    void scalarMatchesTheMatrixAndTheLayout_data()
    {
        QTest::addColumn<int>("width"); QTest::addColumn<int>("height"); QTest::addColumn<int>("version"); QTest::addColumn<int>("order");
        for (int version : {1, 2}) for (int order : {0, 1}) {
            QTest::addRow("64x32 v%d order%d", version, order) << 64 << 32 << version << order;
            QTest::addRow("48x20 v%d order%d (H%%16!=0)", version, order) << 48 << 20 << version << order;
            QTest::addRow("33x17 v%d order%d (odd)", version, order) << 33 << 17 << version << order;
            QTest::addRow("35x18 v%d order%d (W%%4==3)", version, order) << 35 << 18 << version << order;
        }
    }
    void scalarMatchesTheMatrixAndTheLayout()
    {
        QFETCH(int, width); QFETCH(int, height); QFETCH(int, version); QFETCH(int, order);
        const Version v = Version(version);
        Frame f{width, height, PixelOrder(order), {}}; f.randomize(7 + width);
        PlaneSet out(width, height, v);
        split(f.input(), v, out.planes, Variant::Scalar);

        std::vector<uint8_t> Y, U, V; reference444(f, Y, U, V);
        const int W = width, H = height, hw = (W + 1) / 2, hh = (H + 1) / 2;
        // main
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) QCOMPARE(out.planes.main.y.data[y * out.planes.main.y.stride + x], Y[y * W + x]);
        for (int y = 0; y < hh; ++y) for (int x = 0; x < hw; ++x) {
            QCOMPARE(out.planes.main.u.data[y * out.planes.main.u.stride + x], mean2x2(U, W, H, 2 * x, 2 * y));
            QCOMPARE(out.planes.main.v.data[y * out.planes.main.v.stride + x], mean2x2(V, W, H, 2 * x, 2 * y));
        }
        // aux, per the Global Constraints layout
        auto auxY = [&](int x, int y) { return out.planes.aux.y.data[y * out.planes.aux.y.stride + x]; };
        auto auxU = [&](int x, int y) { return out.planes.aux.u.data[y * out.planes.aux.u.stride + x]; };
        auto auxV = [&](int x, int y) { return out.planes.aux.v.data[y * out.planes.aux.v.stride + x]; };
        if (v == Version::V2) {
            for (int y = 0; y < H; ++y) for (int x = 0; 2 * x + 1 < W; ++x) {
                QCOMPARE(auxY(x, y), U[y * W + 2 * x + 1]);
                QCOMPARE(auxY(W / 2 + x, y), V[y * W + 2 * x + 1]);
            }
            // W % 4 != 0: the U-part's last entry (x = W/4) shares column W/4 with the V-part's
            // first entry; the right-most block writes last, so the U-part value stands and the
            // V-part's x = 0 is not checked. The V plane overlaps the same way only for W % 4 == 3.
            const bool overlapU = (W % 4) != 0, overlapV = (W % 4) == 3;
            for (int yy = 0; 2 * yy + 1 < H; ++yy) for (int x = 0; 4 * x < W; ++x) {
                const int y = 2 * yy + 1;
                QCOMPARE(auxU(x, yy), U[y * W + 4 * x]);
                if (!(overlapU && x == 0)) QCOMPARE(auxU(W / 4 + x, yy), V[y * W + 4 * x]);
                if (4 * x + 2 < W) {
                    QCOMPARE(auxV(x, yy), U[y * W + 4 * x + 2]);
                    if (!(overlapV && x == 0)) QCOMPARE(auxV(W / 4 + x, yy), V[y * W + 4 * x + 2]);
                }
            }
        } else {
            for (int yy = 0; 2 * yy + 1 < H; ++yy) {
                const int i = yy, n = (i & ~7) + i, y = 2 * yy + 1;
                for (int x = 0; x < W; ++x) { QCOMPARE(auxY(x, n), U[y * W + x]); QCOMPARE(auxY(x, n + 8), V[y * W + x]); }
            }
            for (int yy = 0; yy < hh; ++yy) for (int x = 0; 2 * x + 1 < W; ++x) {
                QCOMPARE(auxU(x, yy), U[(2 * yy) * W + 2 * x + 1]);
                QCOMPARE(auxV(x, yy), V[(2 * yy) * W + 2 * x + 1]);
            }
        }
        // Every byte inside the declared plane sizes is written: the same split into planes
        // pre-filled with 0x00 and with 0xFF must come out identical.
        PlaneSet zero(width, height, v, 0x00), ones(width, height, v, 0xFF);
        split(f.input(), v, zero.planes, Variant::Scalar);
        split(f.input(), v, ones.planes, Variant::Scalar);
        QCOMPARE(zero.compareAll(ones), 0);
    }

#ifdef HAVE_FREERDP_PRIMITIVES
    // The oracle every FreeRDP client runs: LUMA then CHROMA combine of our (main, aux) must give
    // the 4:4:4 planes back at every position except the (even, even) chroma samples, which the
    // decoder keeps as the 2x2 mean carried by the main frame.
    void roundTripsThroughFreeRdpDecoder_data()
    {
        QTest::addColumn<int>("width"); QTest::addColumn<int>("height"); QTest::addColumn<int>("version");
        for (int version : {1, 2}) {
            QTest::addRow("64x32 v%d", version) << 64 << 32 << version;
            QTest::addRow("48x20 v%d", version) << 48 << 20 << version;
            QTest::addRow("128x48 v%d", version) << 128 << 48 << version;
            QTest::addRow("36x16 v%d (W%%4==0, small)", version) << 36 << 16 << version;
            QTest::addRow("37x17 v%d (odd)", version) << 37 << 17 << version;
        }
    }
    void roundTripsThroughFreeRdpDecoder()
    {
        QFETCH(int, width); QFETCH(int, height); QFETCH(int, version);
        const Version v = Version(version);
        primitives_t *prims = primitives_get_generic();
        QVERIFY(prims && prims->YUV420CombineToYUV444);
        Frame f{width, height, PixelOrder::Rgbx, {}}; f.randomize(99 + width);
        PlaneSet out(width, height, v);
        split(f.input(), v, out.planes, Variant::Scalar);

        // The decoder writes into a 16-aligned 4:4:4 buffer; give it one.
        const int W = width, H = height, W16 = roundUp16(W), H16 = roundUp16(H);
        std::vector<uint8_t> Y(size_t(W16) * H16, 0), U(Y.size(), 0), V(Y.size(), 0);
        BYTE *dst[3] = {Y.data(), U.data(), V.data()};
        const UINT32 dstStep[3] = {UINT32(W16), UINT32(W16), UINT32(W16)};
        RECTANGLE_16 roi{0, 0, UINT16(W), UINT16(H)};
        {
            const BYTE *src[3] = {out.planes.main.y.data, out.planes.main.u.data, out.planes.main.v.data};
            const UINT32 step[3] = {UINT32(out.planes.main.y.stride), UINT32(out.planes.main.u.stride), UINT32(out.planes.main.v.stride)};
            QCOMPARE(prims->YUV420CombineToYUV444(AVC444_LUMA, src, step, W, H, dst, dstStep, &roi), PRIMITIVES_SUCCESS);
        }
        {
            const BYTE *src[3] = {out.planes.aux.y.data, out.planes.aux.u.data, out.planes.aux.v.data};
            const UINT32 step[3] = {UINT32(out.planes.aux.y.stride), UINT32(out.planes.aux.u.stride), UINT32(out.planes.aux.v.stride)};
            QCOMPARE(prims->YUV420CombineToYUV444(v == Version::V1 ? AVC444_CHROMAv1 : AVC444_CHROMAv2, src, step, W, H, dst, dstStep, &roi), PRIMITIVES_SUCCESS);
        }
        std::vector<uint8_t> refY, refU, refV; reference444(f, refY, refU, refV);
        // v2, W % 4 != 0: the decoder reads the overlapped column (see the layout note) as both
        // U444(4*(W/4), odd row) and V444(0, odd row); only the former can be right. Likewise
        // V444(2, odd row) for W % 4 == 3. Those samples are excluded; everything else is exact.
        auto excludedV = [&](int x, int y) {
            if (v != Version::V2 || y % 2 == 0) return false;
            return ((W % 4) != 0 && x == 0) || ((W % 4) == 3 && x == 2);
        };
        int diffs = 0;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const bool meanPos = (x % 2 == 0) && (y % 2 == 0);
            const uint8_t eu = meanPos ? mean2x2(refU, W, H, x, y) : refU[y * W + x];
            const uint8_t ev = meanPos ? mean2x2(refV, W, H, x, y) : refV[y * W + x];
            const bool vOk = excludedV(x, y) || V[y * W16 + x] == ev;
            if (Y[y * W16 + x] != refY[y * W + x] || U[y * W16 + x] != eu || !vOk) {
                if (diffs < 8) qWarning("(%d,%d): Y %d/%d U %d/%d V %d/%d", x, y, Y[y * W16 + x], refY[y * W + x], U[y * W16 + x], eu, V[y * W16 + x], ev);
                ++diffs;
            }
        }
        QCOMPARE(diffs, 0);
    }

    // Bit-exact against libfreerdp's own encoder primitive on even, 16-aligned sizes (every byte
    // it writes), and on odd sizes at every position it writes (its unwritten positions keep the
    // 0xA5 sentinel and are skipped).
    void matchesFreeRdpEncoderPrimitive_data()
    {
        QTest::addColumn<int>("width"); QTest::addColumn<int>("height"); QTest::addColumn<int>("version"); QTest::addColumn<int>("order"); QTest::addColumn<bool>("generic");
        for (int version : {1, 2}) for (int order : {0, 1}) for (bool generic : {true, false}) {
            QTest::addRow("128x64 v%d order%d %s", version, order, generic ? "generic" : "primitives_get") << 128 << 64 << version << order << generic;
            QTest::addRow("45x19 v%d order%d %s", version, order, generic ? "generic" : "primitives_get") << 45 << 19 << version << order << generic;
        }
    }
    void matchesFreeRdpEncoderPrimitive()
    {
        QFETCH(int, width); QFETCH(int, height); QFETCH(int, version); QFETCH(int, order); QFETCH(bool, generic);
        const Version v = Version(version);
        primitives_t *prims = generic ? primitives_get_generic() : primitives_get();
        QVERIFY(prims);
        Frame f{width, height, PixelOrder(order), {}}; f.randomize(3 + width + version);
        PlaneSet ours(width, height, v), theirs(width, height, v);
        split(f.input(), v, ours.planes, Variant::Scalar);
        BYTE *mainDst[3] = {theirs.planes.main.y.data, theirs.planes.main.u.data, theirs.planes.main.v.data};
        const UINT32 mainStep[3] = {UINT32(theirs.planes.main.y.stride), UINT32(theirs.planes.main.u.stride), UINT32(theirs.planes.main.v.stride)};
        BYTE *auxDst[3] = {theirs.planes.aux.y.data, theirs.planes.aux.u.data, theirs.planes.aux.v.data};
        const UINT32 auxStep[3] = {UINT32(theirs.planes.aux.y.stride), UINT32(theirs.planes.aux.u.stride), UINT32(theirs.planes.aux.v.stride)};
        const prim_size_t roi{UINT32(width), UINT32(height)};
        const UINT32 fmt = PixelOrder(order) == PixelOrder::Rgbx ? PIXEL_FORMAT_RGBX32 : PIXEL_FORMAT_BGRX32;
        const auto fn = v == Version::V1 ? prims->RGBToAVC444YUV : prims->RGBToAVC444YUVv2;
        QCOMPARE(fn(f.pixels.data(), fmt, UINT32(f.stride()), mainDst, mainStep, auxDst, auxStep, &roi), PRIMITIVES_SUCCESS);
        // primitives_get() may hand back a SIMD flavour whose 2x2 mean rounds differently by at
        // most one; the generic C code must match exactly. Everything else is bit-exact.
        const int diffs = ours.compareAll(theirs, generic ? 0 : 1, /*skipSentinelInOther=*/true);
        QCOMPARE(diffs, 0);
    }

    void freeRdpVariantMatchesScalar()
    {
        if (!available(Variant::FreeRdp)) QSKIP("Variant::FreeRdp not available");
        for (int version : {1, 2}) {
            Frame f{256, 144, PixelOrder::Rgbx, {}}; f.randomize(11 + version);
            PlaneSet a(256, 144, Version(version)), b(256, 144, Version(version));
            split(f.input(), Version(version), a.planes, Variant::Scalar);
            split(f.input(), Version(version), b.planes, Variant::FreeRdp);
            QCOMPARE(a.compareAll(b, 1, true), 0); // aux/main.y exact, main u/v within 1, unwritten skipped
        }
    }
#endif

    void unavailableVariantFallsBackToScalar()
    {
        Frame f{40, 24, PixelOrder::Rgbx, {}}; f.randomize(5);
        PlaneSet a(40, 24, Version::V2);
        split(f.input(), Version::V2, a.planes, Variant::Scalar);
        // Whatever the CPU has, an explicit variant never produces different bytes than Scalar
        // (an unavailable one falls back to it; FreeRdp may round the 2x2 mean by one).
        for (Variant v : {Variant::Avx2, Variant::Avx512, Variant::FreeRdp, Variant::Auto}) {
            PlaneSet c(40, 24, Version::V2);
            split(f.input(), Version::V2, c.planes, v);
            QCOMPARE(a.compareAll(c, v == Variant::FreeRdp ? 1 : 0), 0);
        }
    }

    void envOverrideSelectsVariant()
    {
        // best() reads KPIPEWIRE_AVC444_SPLIT on every call (it runs once per encoder start, not per frame).
        qputenv("KPIPEWIRE_AVC444_SPLIT", "scalar");
        QCOMPARE(best(), Variant::Scalar);
        qputenv("KPIPEWIRE_AVC444_SPLIT", "nonsense");
        QVERIFY(best() != Variant::Auto); // warns, then auto-detects
        qunsetenv("KPIPEWIRE_AVC444_SPLIT");
        QVERIFY(best() != Variant::Auto);
        QVERIFY(available(best()));
    }

    void timingAt1440p()
    {
        Frame f{2560, 1440, PixelOrder::Rgbx, {}}; f.randomize(1);
        for (Variant v : {Variant::Scalar, Variant::FreeRdp, Variant::Avx2, Variant::Avx512}) {
            if (!available(v)) { qInfo("%-8s unavailable", name(v)); continue; }
            for (int version : {1, 2}) {
                PlaneSet out(2560, 1440, Version(version));
                split(f.input(), Version(version), out.planes, v); // warm up
                QElapsedTimer t; t.start();
                constexpr int runs = 10;
                for (int i = 0; i < runs; ++i) split(f.input(), Version(version), out.planes, v);
                qInfo("%-8s v%d: %6.0f us per 2560x1440 frame", name(v), version, t.nsecsElapsed() / 1000.0 / runs);
            }
        }
    }
};

QTEST_GUILESS_MAIN(Avc444SplitTest)
#include "avc444splittest.moc"
```

(`best()` honours the environment at call time — it runs once per encoder initialisation, not per frame.)

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S ~/dev/kpipewire -B ~/dev/kpipewire/build-tests -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF -DCMAKE_INSTALL_PREFIX=/tmp/kpipewire-tests-prefix 2>&1 | tail -3 && cmake --build ~/dev/kpipewire/build-tests -j16 --target avc444splittest 2>&1 | tail -5`
Expected: FAIL to build — `avc444split_p.h: No such file or directory`.

- [ ] **Step 4: Write the scalar reference, the FreeRdp variant and the dispatch**

`src/avc444split.cpp`:

```cpp
/*
    SPDX-FileCopyrightText: 2026 Steve Westerhouse <steve.westerhouse@origami-analytics.com>
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "avc444split_p.h"

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>
#include <cstring>

#include "logging_record.h"

#ifdef HAVE_FREERDP_PRIMITIVES
extern "C" {
#include <freerdp/codec/color.h>
#include <freerdp/primitives.h>
}
#endif

namespace Avc444Split
{

namespace
{
struct Yuv { uint8_t y, u, v; };

inline Yuv pixel(const Input &in, int x, int y)
{
    const uint8_t *p = in.pixels + size_t(y) * in.stride + size_t(x) * 4;
    int r, g, b;
    if (in.order == PixelOrder::Rgbx) { r = p[0]; g = p[1]; b = p[2]; } else { b = p[0]; g = p[1]; r = p[2]; }
    return {rgbToY(r, g, b), rgbToU(r, g, b), rgbToV(r, g, b)};
}

inline uint8_t *at(const Plane &p, int x, int y) { return p.data + size_t(y) * p.stride + x; }

// One 2x2 block whose even corner is (x, y). a = (x,y) b = (x+1,y) c = (x,y+1) d = (x+1,y+1);
// a missing right column or bottom row replicates a (libfreerdp's general_RGBToAVC444YUV*).
void block(const Input &in, Version version, Planes &out, int x, int y)
{
    const bool hasRight = x + 1 < in.width, hasBottom = y + 1 < in.height;
    const Yuv a = pixel(in, x, y);
    const Yuv b = hasRight ? pixel(in, x + 1, y) : a;
    const Yuv c = hasBottom ? pixel(in, x, y + 1) : a;
    const Yuv d = (hasRight && hasBottom) ? pixel(in, x + 1, y + 1) : a;
    const int W = in.width, hx = x / 2, hy = y / 2;

    *at(out.main.y, x, y) = a.y;
    if (hasRight) *at(out.main.y, x + 1, y) = b.y;
    if (hasBottom) *at(out.main.y, x, y + 1) = c.y;
    if (hasRight && hasBottom) *at(out.main.y, x + 1, y + 1) = d.y;
    *at(out.main.u, hx, hy) = uint8_t((a.u + b.u + c.u + d.u) / 4);
    *at(out.main.v, hx, hy) = uint8_t((a.v + b.v + c.v + d.v) / 4);

    if (version == Version::V2) {
        // aux.y row y: [0,W/2) U at odd columns, [W/2,W) V at odd columns; same for row y+1
        if (hasRight) {
            *at(out.aux.y, hx, y) = b.u; *at(out.aux.y, W / 2 + hx, y) = b.v;
            if (hasBottom) { *at(out.aux.y, hx, y + 1) = d.u; *at(out.aux.y, W / 2 + hx, y + 1) = d.v; }
        }
        // odd row y+1, column x (even): x%4==0 -> aux.u [U | V], x%4==2 -> aux.v [U | V], at column x/4
        if (hasBottom) {
            const Plane &plane = (x % 4 == 0) ? out.aux.u : out.aux.v;
            *at(plane, x / 4, hy) = c.u; *at(plane, W / 4 + x / 4, hy) = c.v;
        }
    } else {
        // aux.y: block rows n = (hy & ~7) + hy carry U of the odd row, n + 8 carry V of the odd row
        const int n = (hy & ~7) + hy;
        if (hasBottom) {
            *at(out.aux.y, x, n) = c.u; *at(out.aux.y, x, n + 8) = c.v;
            if (hasRight) { *at(out.aux.y, x + 1, n) = d.u; *at(out.aux.y, x + 1, n + 8) = d.v; }
        }
        // aux.u/aux.v row hy: U/V of the even row at odd columns
        if (hasRight) { *at(out.aux.u, hx, hy) = b.u; *at(out.aux.v, hx, hy) = b.v; }
    }
}

std::atomic<bool> warnedFallback{false};
}

// The positions the wire format does not define get 128, so every byte is deterministic.
// With an even width and height v2 defines every aux byte and v1 everything but the block rows
// whose odd source row would be >= H; an odd width or height leaves scattered positions
// (the last column's missing odd neighbour, the last row's missing odd row), so for those rare
// sizes the aux planes are simply pre-filled. Never touches the main planes: block() writes
// every main byte for every size.
void fillUndefinedPositions(const Input &in, Version version, Planes &out)
{
    const int W = in.width, H = in.height, hw = (W + 1) / 2, hh = (H + 1) / 2, auxH = auxHeight(version, H);
    if (W % 2 == 1 || H % 2 == 1) {
        for (int y = 0; y < auxH; ++y) std::memset(at(out.aux.y, 0, y), 128, size_t(W));
        for (int y = 0; y < hh; ++y) { std::memset(at(out.aux.u, 0, y), 128, size_t(hw)); std::memset(at(out.aux.v, 0, y), 128, size_t(hw)); }
        return;
    }
    if (version == Version::V1) {
        for (int n = 0; n < auxH; ++n) {
            const int j = n % 16, k = n / 16, i = 8 * k + (j % 8), srcRow = 2 * i + 1;
            if (srcRow >= H) std::memset(at(out.aux.y, 0, n), 128, size_t(W));
        }
    }
}

void splitScalarRegion(const Input &in, Version version, Planes &out, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y += 2) for (int x = x0; x < x1; x += 2) block(in, version, out, x, y);
}

void splitScalar(const Input &in, Version version, Planes &out)
{
    fillUndefinedPositions(in, version, out);
    splitScalarRegion(in, version, out, 0, 0, in.width, in.height);
}

#ifdef HAVE_FREERDP_PRIMITIVES
static bool splitFreeRdp(const Input &in, Version version, Planes &out)
{
    primitives_t *prims = primitives_get();
    if (!prims || !prims->RGBToAVC444YUV || !prims->RGBToAVC444YUVv2) return false;
    fillUndefinedPositions(in, version, out); // libfreerdp leaves the undefined positions alone
    BYTE *mainDst[3] = {out.main.y.data, out.main.u.data, out.main.v.data};
    const UINT32 mainStep[3] = {UINT32(out.main.y.stride), UINT32(out.main.u.stride), UINT32(out.main.v.stride)};
    BYTE *auxDst[3] = {out.aux.y.data, out.aux.u.data, out.aux.v.data};
    const UINT32 auxStep[3] = {UINT32(out.aux.y.stride), UINT32(out.aux.u.stride), UINT32(out.aux.v.stride)};
    const prim_size_t roi{UINT32(in.width), UINT32(in.height)};
    const UINT32 fmt = in.order == PixelOrder::Rgbx ? PIXEL_FORMAT_RGBX32 : PIXEL_FORMAT_BGRX32;
    const auto fn = version == Version::V1 ? prims->RGBToAVC444YUV : prims->RGBToAVC444YUVv2;
    return fn(in.pixels, fmt, UINT32(in.stride), mainDst, mainStep, auxDst, auxStep, &roi) == PRIMITIVES_SUCCESS;
}
#endif

bool available(Variant variant)
{
    switch (variant) {
    case Variant::Auto:
    case Variant::Scalar:
        return true;
    case Variant::FreeRdp:
#ifdef HAVE_FREERDP_PRIMITIVES
        return primitives_get() != nullptr;
#else
        return false;
#endif
    case Variant::Avx2:
        return __builtin_cpu_supports("avx2");
    case Variant::Avx512:
        return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512vbmi");
    }
    return false;
}

const char *name(Variant variant)
{
    switch (variant) {
    case Variant::Auto: return "auto";
    case Variant::Scalar: return "scalar";
    case Variant::FreeRdp: return "freerdp";
    case Variant::Avx2: return "avx2";
    case Variant::Avx512: return "avx512";
    }
    return "?";
}

Variant best()
{
    const QByteArray forced = qgetenv("KPIPEWIRE_AVC444_SPLIT");
    if (!forced.isEmpty()) {
        for (Variant v : {Variant::Scalar, Variant::FreeRdp, Variant::Avx2, Variant::Avx512}) {
            if (forced == name(v)) {
                if (available(v)) return v;
                qCWarning(PIPEWIRERECORD_LOGGING) << "KPIPEWIRE_AVC444_SPLIT=" << forced << "is not available on this machine; auto-detecting";
                break;
            }
        }
        if (forced != "auto") qCWarning(PIPEWIRERECORD_LOGGING) << "Unknown KPIPEWIRE_AVC444_SPLIT value" << forced << "(scalar|freerdp|avx2|avx512)";
    }
    for (Variant v : {Variant::Avx512, Variant::Avx2, Variant::FreeRdp}) if (available(v)) return v;
    return Variant::Scalar;
}

void split(const Input &in, Version version, Planes &out, Variant variant)
{
    if (variant == Variant::Auto) variant = best();
    if (!available(variant)) {
        if (!warnedFallback.exchange(true)) qCWarning(PIPEWIRERECORD_LOGGING) << "Avc444Split variant" << name(variant) << "unavailable; using scalar";
        variant = Variant::Scalar;
    }
    switch (variant) {
    case Variant::Avx512: splitAvx512(in, version, out); return;
    case Variant::Avx2: splitAvx2(in, version, out); return;
    case Variant::FreeRdp:
#ifdef HAVE_FREERDP_PRIMITIVES
        if (splitFreeRdp(in, version, out)) return;
#endif
        [[fallthrough]];
    case Variant::Auto:
    case Variant::Scalar: splitScalar(in, version, out); return;
    }
}

}
```

K1 stubs (K2 replaces the bodies):

```cpp
// src/avc444split_avx2.cpp  (K1 stub)
#include "avc444split_p.h"
namespace Avc444Split { void splitAvx2(const Input &in, Version version, Planes &out) { splitScalar(in, version, out); } }
// src/avc444split_avx512.cpp (K1 stub)
#include "avc444split_p.h"
namespace Avc444Split { void splitAvx512(const Input &in, Version version, Planes &out) { splitScalar(in, version, out); } }
```

The v2 overlap for `W % 4 != 0` (Global Constraints) is not an undefined position: whichever `block()` writes last wins, and the blocks run left to right, so the V-part's first column (`V444(0, odd row)`) is overwritten by `U444(4·(W/4), odd row)`. Put that sentence in the header comment above `split()`; no encoder-side fix exists.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build ~/dev/kpipewire/build-tests -j16 --target avc444splittest 2>&1 | grep -E "warning|error" ; ctest --test-dir ~/dev/kpipewire/build-tests -R avc444splittest --output-on-failure 2>&1 | tail -30`
Expected: all rows PASS, zero warnings; the `timingAt1440p` lines print `scalar`, `freerdp` (and, until K2, `avx2`/`avx512` at scalar speed). If `roundTripsThroughFreeRdpDecoder` fails for one version only, the failing positions printed by the test say which quarter/half the decoder reads differently from the Global Constraints layout — correct `block()`'s aux placement for that version to what the decoder reads (the decoder primitive is the authority), re-run, and record the corrected layout in the header comment and in the krdp plan's research.md note.

- [ ] **Step 6: Commit and export patch 0012**

```bash
cd ~/dev/kpipewire && git add CMakeLists.txt src/CMakeLists.txt src/avc444split_p.h src/avc444split.cpp src/avc444split_avx2.cpp src/avc444split_avx512.cpp src/autotests/CMakeLists.txt src/autotests/avc444splittest.cpp
git commit -m "avc444: Avc444Split scalar reference and libfreerdp variant, layout pinned by libfreerdp's decoder primitive (KRDP OPT-045)

RGBX/BGRX -> (main 4:2:0, auxiliary chroma 4:2:0) for MS-RDPEGFX AVC444 v1 and
v2, FreeRDP's own RGB->YUV constants so its clients invert exactly. Every byte of
every plane is defined. Tests: round trip through YUV420CombineToYUV444, bit-exact
vs primitives_get_generic()->RGBToAVC444YUV[v2], odd sizes, env override, timing.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git format-patch -1 HEAD --start-number 12 -o ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/
```

---

### Task K2: AVX2 and AVX-512 kernels + dispatch, measured

**Files:**
- Modify: `src/avc444split_avx2.cpp`, `src/avc444split_avx512.cpp` (replace the K1 stubs)
- Test: `src/autotests/avc444splittest.cpp` (add `simdVariantsMatchScalar`)

**Interfaces:**
- Consumes: `splitScalarRegion`, `fillUndefinedPositions` (K1; both declared in `avc444split_p.h`). Each SIMD entry point = fill, SIMD body over full chunks, scalar tail.
- Produces: `splitAvx2`, `splitAvx512` with identical output to `splitScalar`.

Kernel design (identical structure in both TUs; 32-bit lanes keep the arithmetic literally the scalar one): for each pair of rows `(e, o = e+1)` and each chunk of `N` pixels (`N = 16` AVX-512, `N = 8` AVX2) starting at column `x` (`x % N == 0`, `x + N <= W`), load the `N` RGBX dwords of each row; `R = px & 0xFF`, `G = (px >> 8) & 0xFF`, `B = (px >> 16) & 0xFF` for `Rgbx` (for `Bgrx` swap the R and B shifts); `Y = (54R + 183G + 18B) >> 8` (logical shift, non-negative), `U = ((-29R - 99G + 128B) >>a 8) + 128`, `V = ((128R - 116G - 12B) >>a 8) + 128` (arithmetic shift) — all as dword vectors. Then:
- main Y rows e/o: pack the `N` dwords to bytes, store `N` bytes.
- main U/V at `(x/2, e/2)`: `S = Ue + Uo` (dwords), `P = S + (S >> 32 within each qword)` → the low dword of each qword holds the 2×2 sum, `>> 2`, take the low byte of each qword → `N/2` bytes.
- v2 aux Y row e: odd-column U = the high dword of each qword of `Ue` → `N/2` bytes at `aux.y[e][x/2]`; odd-column V likewise at `aux.y[e][W/2 + x/2]`; same for row o.
- v2 aux U/V row o/2: from `Uo`/`Vo`, dwords with lane index ≡ 0 mod 4 → `N/4` bytes at `aux.u[o/2][x/4]` (U) and `aux.u[o/2][W/4 + x/4]` (V); lane index ≡ 2 mod 4 → `aux.v[o/2][x/4]` (U) and `aux.v[o/2][W/4 + x/4]` (V).
- v1 aux Y: `Uo` packed → `aux.y[n][x]`, `Vo` packed → `aux.y[n+8][x]` with `n = ((o/2) & ~7) + o/2`; aux U/V row e/2: odd-column `Ue`/`Ve` → `N/2` bytes at `aux.u[e/2][x/2]` / `aux.v[e/2][x/2]`.
- Tail: columns `[W - W % N, W)` of every row pair, and the last row when `H` is odd, via `splitScalarRegion` (which handles the ragged edges exactly as K1).

- [ ] **Step 1: Add the failing test**

Append to `Avc444SplitTest`:

```cpp
    void simdVariantsMatchScalar_data()
    {
        QTest::addColumn<int>("variant"); QTest::addColumn<int>("width"); QTest::addColumn<int>("height"); QTest::addColumn<int>("version"); QTest::addColumn<int>("order");
        for (int variant : {int(Variant::Avx2), int(Variant::Avx512)}) for (int version : {1, 2}) for (int order : {0, 1}) {
            const char *vn = variant == int(Variant::Avx2) ? "avx2" : "avx512";
            QTest::addRow("%s 2560x1440 v%d o%d", vn, version, order) << variant << 2560 << 1440 << version << order;
            QTest::addRow("%s 1920x1080 v%d o%d", vn, version, order) << variant << 1920 << 1080 << version << order;
            QTest::addRow("%s 1366x768 v%d o%d (W%%4==2)", vn, version, order) << variant << 1366 << 768 << version << order;
            QTest::addRow("%s 333x211 v%d o%d (odd)", vn, version, order) << variant << 333 << 211 << version << order;
            QTest::addRow("%s 47x18 v%d o%d (tail-only)", vn, version, order) << variant << 47 << 18 << version << order;
            QTest::addRow("%s 16x2 v%d o%d (one chunk)", vn, version, order) << variant << 16 << 2 << version << order;
            QTest::addRow("%s 1x1 v%d o%d", vn, version, order) << variant << 1 << 1 << version << order;
        }
    }
    void simdVariantsMatchScalar()
    {
        QFETCH(int, variant); QFETCH(int, width); QFETCH(int, height); QFETCH(int, version); QFETCH(int, order);
        const Variant v = Variant(variant);
        if (!available(v)) QSKIP("variant not available on this CPU");
        for (quint32 seed : {1u, 2u, 3u}) {
            Frame f{width, height, PixelOrder(order), {}}; f.randomize(seed * 1000 + width);
            PlaneSet a(width, height, Version(version)), b(width, height, Version(version));
            split(f.input(), Version(version), a.planes, Variant::Scalar);
            split(f.input(), Version(version), b.planes, v);
            QCOMPARE(a.compareAll(b), 0);
        }
    }
```

- [ ] **Step 2: Run it to verify it passes trivially now (stubs forward to scalar) and record the baseline timing**

Run: `cmake --build ~/dev/kpipewire/build-tests -j16 --target avc444splittest 2>&1 | grep -E "warning|error"; ctest --test-dir ~/dev/kpipewire/build-tests -R avc444splittest --output-on-failure 2>&1 | grep -E "us per|Passed|Failed"`
Expected: PASS; note the `scalar` µs figure (the kernels below must beat it by ≥ 4× for AVX2 and ≥ 6× for AVX-512, else something is not vectorised).

- [ ] **Step 3: Write the AVX-512 kernel**

`src/avc444split_avx512.cpp`:

```cpp
/*
    SPDX-FileCopyrightText: 2026 Steve Westerhouse <steve.westerhouse@origami-analytics.com>
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "avc444split_p.h"

#include <immintrin.h>

// Compiled without -mavx512* for the library as a whole; only these functions carry the ISA.
#define AVX512_TARGET __attribute__((target("avx512f,avx512bw,avx512vl,avx512vbmi")))

namespace Avc444Split
{
namespace
{
struct YuvVec { __m512i y, u, v; };

AVX512_TARGET inline YuvVec convert16(const uint8_t *row, PixelOrder order)
{
    const __m512i px = _mm512_loadu_si512(reinterpret_cast<const void *>(row));
    const __m512i mask = _mm512_set1_epi32(0xFF);
    const __m512i c0 = _mm512_and_si512(px, mask);
    const __m512i g = _mm512_and_si512(_mm512_srli_epi32(px, 8), mask);
    const __m512i c2 = _mm512_and_si512(_mm512_srli_epi32(px, 16), mask);
    const __m512i r = order == PixelOrder::Rgbx ? c0 : c2;
    const __m512i b = order == PixelOrder::Rgbx ? c2 : c0;
    YuvVec out;
    out.y = _mm512_srli_epi32(_mm512_add_epi32(_mm512_add_epi32(_mm512_mullo_epi32(r, _mm512_set1_epi32(54)), _mm512_mullo_epi32(g, _mm512_set1_epi32(183))),
                                               _mm512_mullo_epi32(b, _mm512_set1_epi32(18))), 8);
    out.u = _mm512_add_epi32(_mm512_srai_epi32(_mm512_add_epi32(_mm512_add_epi32(_mm512_mullo_epi32(r, _mm512_set1_epi32(-29)), _mm512_mullo_epi32(g, _mm512_set1_epi32(-99))),
                                                                _mm512_mullo_epi32(b, _mm512_set1_epi32(128))), 8), _mm512_set1_epi32(128));
    out.v = _mm512_add_epi32(_mm512_srai_epi32(_mm512_add_epi32(_mm512_add_epi32(_mm512_mullo_epi32(r, _mm512_set1_epi32(128)), _mm512_mullo_epi32(g, _mm512_set1_epi32(-116))),
                                                                _mm512_mullo_epi32(b, _mm512_set1_epi32(-12))), 8), _mm512_set1_epi32(128));
    return out;
}

// 16 dwords (each 0..255) -> 16 bytes
AVX512_TARGET inline void store16(uint8_t *dst, __m512i dwords) { _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), _mm512_cvtepi32_epi8(dwords)); }
// low dword of each qword (8 of them, each 0..255) -> 8 bytes
AVX512_TARGET inline void storeLow8(uint8_t *dst, __m512i dwords) { _mm_storel_epi64(reinterpret_cast<__m128i *>(dst), _mm512_cvtepi64_epi8(dwords)); }
// odd dwords (lanes 1,3,...,15) -> 8 bytes
AVX512_TARGET inline void storeOdd8(uint8_t *dst, __m512i dwords) { storeLow8(dst, _mm512_srli_epi64(dwords, 32)); }
// lanes 0,4,8,12 -> 4 bytes; lanes 2,6,10,14 -> 4 bytes
AVX512_TARGET inline void storeEvery4(uint8_t *dst0, uint8_t *dst2, __m512i dwords)
{
    const __m512i idx0 = _mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12, 8, 4, 0);
    const __m512i idx2 = _mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 14, 10, 6, 2);
    const __m128i b0 = _mm512_cvtepi32_epi8(_mm512_permutexvar_epi32(idx0, dwords));
    const __m128i b2 = _mm512_cvtepi32_epi8(_mm512_permutexvar_epi32(idx2, dwords));
    *reinterpret_cast<uint32_t *>(dst0) = uint32_t(_mm_cvtsi128_si32(b0));
    *reinterpret_cast<uint32_t *>(dst2) = uint32_t(_mm_cvtsi128_si32(b2));
}
AVX512_TARGET inline __m512i mean2x2(__m512i ue, __m512i uo)
{
    const __m512i s = _mm512_add_epi32(ue, uo);
    const __m512i p = _mm512_add_epi32(s, _mm512_srli_epi64(s, 32));
    return _mm512_srli_epi32(p, 2); // valid in the low dword of each qword
}
inline uint8_t *at(const Plane &p, int x, int y) { return p.data + size_t(y) * p.stride + x; }

AVX512_TARGET void body(const Input &in, Version version, Planes &out)
{
    constexpr int N = 16;
    const int W = in.width, H = in.height, wN = W - W % N, hEven = H - H % 2;
    for (int e = 0; e < hEven; e += 2) {
        const int o = e + 1, hy = e / 2, n = (hy & ~7) + hy;
        const uint8_t *rowE = in.pixels + size_t(e) * in.stride, *rowO = in.pixels + size_t(o) * in.stride;
        for (int x = 0; x < wN; x += N) {
            const YuvVec pe = convert16(rowE + size_t(x) * 4, in.order), po = convert16(rowO + size_t(x) * 4, in.order);
            store16(at(out.main.y, x, e), pe.y);
            store16(at(out.main.y, x, o), po.y);
            storeLow8(at(out.main.u, x / 2, hy), mean2x2(pe.u, po.u));
            storeLow8(at(out.main.v, x / 2, hy), mean2x2(pe.v, po.v));
            if (version == Version::V2) {
                storeOdd8(at(out.aux.y, x / 2, e), pe.u); storeOdd8(at(out.aux.y, W / 2 + x / 2, e), pe.v);
                storeOdd8(at(out.aux.y, x / 2, o), po.u); storeOdd8(at(out.aux.y, W / 2 + x / 2, o), po.v);
                storeEvery4(at(out.aux.u, x / 4, hy), at(out.aux.v, x / 4, hy), po.u);
                storeEvery4(at(out.aux.u, W / 4 + x / 4, hy), at(out.aux.v, W / 4 + x / 4, hy), po.v);
            } else {
                store16(at(out.aux.y, x, n), po.u); store16(at(out.aux.y, x, n + 8), po.v);
                storeOdd8(at(out.aux.u, x / 2, hy), pe.u); storeOdd8(at(out.aux.v, x / 2, hy), pe.v);
            }
        }
        if (wN < W) splitScalarRegion(in, version, out, wN, e, W, e + 2);
    }
    if (hEven < H) splitScalarRegion(in, version, out, 0, hEven, W, H);
}
}

void splitAvx512(const Input &in, Version version, Planes &out)
{
    fillUndefinedPositions(in, version, out); // same preamble as splitScalar()
    body(in, version, out);
}
}
```

The 4-byte stores in `storeEvery4` go through `std::memcpy(dst, &word, 4)` rather than a `uint32_t *` cast (unaligned, strict aliasing).

Note on `storeEvery4` with the v2 quarter halves when `W % 4 == 2`: the SIMD body only touches full 16-pixel chunks, whose `x/4` columns never reach the overlapping column; the overlap is written by the scalar tail in the same left-to-right order as `splitScalar`, so the two remain bit-exact (the test row `1366x768` checks exactly this).

- [ ] **Step 4: Write the AVX2 kernel**

`src/avc444split_avx2.cpp` — same body with `N = 8`, `__m256i`, and these helpers (AVX2 has no `cvtepi32_epi8`; the byte gathers are done with an in-lane shuffle followed by a cross-lane dword permute):

```cpp
#define AVX2_TARGET __attribute__((target("avx2")))
// low byte of each of the 8 dwords -> 8 bytes
AVX2_TARGET inline void store8(uint8_t *dst, __m256i d)
{
    const __m256i sh = _mm256_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i g = _mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(d, sh), _mm256_setr_epi32(0, 4, 1, 1, 1, 1, 1, 1));
    _mm_storel_epi64(reinterpret_cast<__m128i *>(dst), _mm256_castsi256_si128(g));
}
// low byte of the low dword of each of the 4 qwords -> 4 bytes
AVX2_TARGET inline void storeLow4(uint8_t *dst, __m256i d)
{
    const __m256i sh = _mm256_setr_epi8(0, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i g = _mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(d, sh), _mm256_setr_epi32(0, 4, 1, 1, 1, 1, 1, 1));
    // bytes 0,1 = lanes 0,2 ; bytes 4,5 = lanes 4,6 -> pack to 4 contiguous bytes
    const __m128i lo = _mm256_castsi256_si128(g);
    const uint32_t v = uint32_t(_mm_cvtsi128_si32(lo)) & 0xFFFFu;
    const uint32_t w = uint32_t(_mm_extract_epi32(lo, 1)) & 0xFFFFu;
    *reinterpret_cast<uint32_t *>(dst) = v | (w << 16);
}
AVX2_TARGET inline void storeOdd4(uint8_t *dst, __m256i d) { storeLow4(dst, _mm256_srli_epi64(d, 32)); }
// lanes 0,4 -> 2 bytes at dst0 ; lanes 2,6 -> 2 bytes at dst2
AVX2_TARGET inline void storeEvery4(uint8_t *dst0, uint8_t *dst2, __m256i d)
{
    alignas(32) uint32_t lanes[8];
    _mm256_store_si256(reinterpret_cast<__m256i *>(lanes), d);
    dst0[0] = uint8_t(lanes[0]); dst0[1] = uint8_t(lanes[4]);
    dst2[0] = uint8_t(lanes[2]); dst2[1] = uint8_t(lanes[6]);
}
AVX2_TARGET inline __m256i mean2x2(__m256i ue, __m256i uo)
{
    const __m256i s = _mm256_add_epi32(ue, uo);
    return _mm256_srli_epi32(_mm256_add_epi32(s, _mm256_srli_epi64(s, 32)), 2);
}
```

`convert8` mirrors `convert16` with `_mm256_loadu_si256`, `_mm256_mullo_epi32`, `_mm256_srli_epi32`, `_mm256_srai_epi32`. Body: `store8` for main Y and v1 aux Y rows, `storeLow4` for main U/V, `storeOdd4` for the odd-column stores, `storeEvery4` for the v2 quarters; tail and fill as in the AVX-512 TU. `splitAvx2()` = `fillUndefinedPositions` + `body`.

- [ ] **Step 5: Run the tests; fix until bit-exact; record the timings**

Run: `cmake --build ~/dev/kpipewire/build-tests -j16 --target avc444splittest 2>&1 | grep -E "warning|error"; ctest --test-dir ~/dev/kpipewire/build-tests -R avc444splittest --output-on-failure 2>&1 | grep -E "differs|us per|Passed|Failed|FAIL"`
Expected: `simdVariantsMatchScalar` PASS for every row on hal9000 (Zen 4: both variants available); zero warnings; timing lines for all four variants. Paste the four `us per 2560x1440 frame` figures (v2) into the commit message. Target on hal9000: `avx512` ≤ 1500 µs, `avx2` ≤ 2500 µs (scalar is typically 8–12 ms). If a `differs at (x,y)` line names a column ≥ `W - W%16` or a row ≥ `H - H%2` the tail hand-off is wrong; if it names an aux quarter column, a `storeEvery4` lane index is wrong; if every value is off by a constant, a shift is logical where it must be arithmetic (or the reverse).

- [ ] **Step 6: Commit and export patch 0013**

```bash
cd ~/dev/kpipewire && git add src/avc444split_p.h src/avc444split.cpp src/avc444split_avx2.cpp src/avc444split_avx512.cpp src/autotests/avc444splittest.cpp
git commit -m "avc444: AVX2 and AVX-512 split kernels, runtime dispatch (KRDP OPT-045)

Dword-lane kernels with the scalar arithmetic verbatim (logical shift for Y,
arithmetic for U/V, truncated 2x2 mean), scalar tail for ragged edges. Bit-exact
with the scalar reference on 2560x1440, 1920x1080, 1366x768, odd and tiny sizes.
2560x1440 v2 on hal9000 (Ryzen 7 8845HS): scalar <S> us, freerdp <F> us,
avx2 <A2> us, avx512 <A5> us.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git format-patch -1 HEAD --start-number 13 -o ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/
```

---

### Task K3: `Avc444NalRewriter` + `VaapiH264` + the GPU go/no-go round trip

**Files:**
- Create: `src/h264bitstream_p.h`, `src/avc444nalrewriter_p.h`, `src/avc444nalrewriter.cpp`, `src/vaapih264_p.h`, `src/vaapih264.cpp`, `src/autotests/avc444nalrewritertest.cpp`, `src/autotests/avc444streamtest.cpp`
- Modify: `src/CMakeLists.txt` (add `avc444nalrewriter.cpp vaapih264.cpp` to `KPipeWireRecord`), `src/autotests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from K1/K2; FFmpeg (`libavcodec`, `libavfilter`, `libavutil`) for `VaapiH264` and the GPU test.
- Produces (K5 and the tests use exactly these):

```cpp
// src/h264bitstream_p.h (header-only, pure)
namespace H264Bits
{
std::vector<uint8_t> ebspToRbsp(std::span<const uint8_t> ebsp);                 // strip emulation-prevention 0x03
std::vector<uint8_t> rbspToEbsp(std::span<const uint8_t> rbsp);                 // re-insert it (and the trailing 0x03 after a final 0x00)
std::vector<std::span<const uint8_t>> splitAnnexB(std::span<const uint8_t> bytes); // NAL units without start codes
struct NalHeader { int refIdc; int type; static NalHeader parse(uint8_t b) { return {(b >> 5) & 3, b & 31}; } static uint8_t make(int refIdc, int type) { return uint8_t((refIdc << 5) | type); } };
class BitReader {  // over an RBSP; throws std::out_of_range past the end
public:
    explicit BitReader(std::span<const uint8_t> data);
    uint32_t u(int n); bool flag(); uint32_t ue(); int32_t se();
    size_t position() const; size_t bitsLeft() const; void skip(size_t bits);
    bool byteAligned() const; size_t bytePosition() const;           // bytePosition valid when aligned
    void skipAlignmentOnes();                                        // cabac_alignment_one_bit up to the byte boundary (asserts ones)
    static size_t lastOneBit(std::span<const uint8_t> rbsp);         // bit index of the rbsp_stop_one_bit (npos if none)
};
class BitWriter {
public:
    void u(uint32_t value, int n); void bit(bool b); void ue(uint32_t v); void se(int32_t v);
    void copyBits(BitReader &from, size_t count);
    void alignOnes();                                                // cabac_alignment_one_bits
    void appendBytes(std::span<const uint8_t> bytes);                // requires byteAligned()
    void trailingBits();                                             // rbsp_stop_one_bit + zero pad
    size_t bitPosition() const; bool byteAligned() const;
    std::vector<uint8_t> take();                                     // the RBSP bytes (must be byte aligned)
};
}

// src/avc444nalrewriter_p.h  (includes: <cstdint> <optional> <span> <vector> <QString>)
namespace Avc444Nal
{
struct Sps {  // the parse-relevant subset of H.264 §7.3.2.1.1
    int profileIdc = 0, levelIdc = 0, spsId = 0, chromaFormatIdc = 1; bool separateColourPlane = false;
    int bitDepthLumaMinus8 = 0, bitDepthChromaMinus8 = 0; bool qpprimeBypass = false;
    bool scalingMatrixPresent = false; std::vector<int32_t> scalingLists;   // every delta_scale, in order, lists concatenated
    int log2MaxFrameNum = 4, pocType = 0, log2MaxPocLsb = 4;
    bool deltaPicOrderAlwaysZero = false; int32_t offsetForNonRefPic = 0, offsetForTopToBottom = 0; std::vector<int32_t> offsetsForRefFrame;
    int maxNumRefFrames = 0; bool gapsAllowed = false;
    int picWidthInMbsMinus1 = 0, picHeightInMapUnitsMinus1 = 0; bool frameMbsOnly = true, mbAff = false, direct8x8 = false;
    bool cropping = false; uint32_t crop[4] = {0, 0, 0, 0};
    bool parseRelevantEquals(const Sps &o) const;   // everything above except levelIdc, maxNumRefFrames, gapsAllowed
};
struct Pps {  // §7.3.2.2, the fields that steer slice-header parsing
    int ppsId = 0, spsId = 0; bool cabac = false, bottomFieldPicOrderPresent = false; int numSliceGroupsMinus1 = 0;
    int numRefIdxL0Minus1 = 0, numRefIdxL1Minus1 = 0; bool weightedPred = false; int weightedBipredIdc = 0;
    int32_t picInitQpMinus26 = 0, picInitQsMinus26 = 0, chromaQpIndexOffset = 0;
    bool deblockingControlPresent = false, constrainedIntraPred = false, redundantPicCntPresent = false;
};
struct SliceHead { int firstMb = 0, sliceType = 0, ppsId = 0, frameNum = 0; bool idr = false; int idrPicId = 0; int pocLsb = 0; };
std::optional<Sps> parseSps(std::span<const uint8_t> rbsp);          // rbsp = NAL payload after the header byte, EP removed
std::optional<Pps> parsePps(std::span<const uint8_t> rbsp);
std::optional<SliceHead> parseSliceHead(std::span<const uint8_t> rbsp, bool idrNal, const Sps &sps); // up to pic_order_cnt_lsb

struct MainStreamState {  // fed every main packet in EMISSION order (output thread), right before the aux that follows it is rewritten
    std::optional<Sps> sps; std::vector<uint8_t> spsRbsp; std::optional<Pps> pps;
    int lastFrameNum = -1; int lastPocLsb = 0; bool haveIdr = false; int packets = 0;
    int serial = 0;               // +1 per tracked main packet
    int64_t lastTrackedPts = -1;  // the pts the caller passed with the last main packet
    bool trackMainPacket(std::span<const uint8_t> annexB, int64_t pts = -1);   // false when a NAL failed to parse (the rest is still applied)
};

class Rewriter {
public:
    enum class Status { Ok, NoMainSps, SpsMismatch, Unsupported, ParseError };
    struct Result { Status status = Status::ParseError; std::vector<uint8_t> annexB; QString detail; };
    // One aux access unit (Annex-B from the intra-only context) -> [PPS id 1][non-ref I slice(s)], each with a 4-byte start code.
    // CONTRACT (K5 honours it, the GPU test models it): rewrite() reads the tracker state at call time, so it is
    // called at EMISSION time - for a pair, right after trackMainPacket() consumed the paired main and before any
    // later main is tracked; for an at-rest refresh, while the last tracked main is the one the client last got
    // (main.lastTrackedPts == the refresh's pts). An aux whose main was followed by a later main is never rewritten,
    // it is dropped (its frame_num/POC would sit behind the last main). Two aux pictures are never emitted without
    // a main between them (POC type 2).
    Result rewrite(std::span<const uint8_t> auxAnnexB, const MainStreamState &main);
    static const char *statusName(Status s);
private:
    std::optional<Sps> m_auxSps; std::optional<Pps> m_auxPps;
    std::vector<uint8_t> m_auxPpsNal;                                  // rewritten PPS NAL (header byte + EBSP), reused when an AU carries none
    std::vector<uint8_t> m_checkedMainSps;                             // main SPS RBSP the last equality check ran against
    bool m_auxSpsChanged = true, m_equal = false;
};
}

// src/vaapih264_p.h — the one place h264_vaapi is configured (K5 switches H264VAAPIEncoder over to it)
namespace VaapiH264
{
enum class Profile { Baseline, Main, High };
struct Params { QSize size; Profile profile = Profile::Main; int qp = 18; int gopSize = 600; AVRational timeBase{1, 1000}; };
AVDictionary *encodingOptions();                                     // async_depth=1, rc_mode=CQP (today's H264VAAPIEncoder::buildEncodingOptions)
AVCodecContext *openContext(const Params &p, AVBufferRef *hwFramesCtx, QString *error = nullptr);  // nullptr on failure
AVBufferRef *createDevice(const QByteArray &renderNode, QString *error = nullptr);                 // AV_HWDEVICE_TYPE_VAAPI
struct UploadGraph { AVFilterGraph *graph = nullptr; AVFilterContext *in = nullptr; AVFilterContext *out = nullptr; ~UploadGraph(); };  // buffer(yuv420p) -> format=nv12 -> hwupload -> buffersink
bool createUploadGraph(UploadGraph &g, AVBufferRef *vaapiDevice, const QSize &size, AVRational timeBase, QString *error = nullptr);
}
```

- [ ] **Step 1: The GPU go/no-go test first (it fails to build until Steps 2–4 exist; its verdict decides whether K4–K6 are built at all)**

`src/autotests/avc444streamtest.cpp`:

```cpp
/*
    SPDX-FileCopyrightText: 2026 Steve Westerhouse <steve.westerhouse@origami-analytics.com>
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
// Encodes a synthetic sequence on the 780M the way H264VAAPIAvc444Encoder will: main pictures
// through a normal h264_vaapi context, chroma pictures through an intra-only one, each aux access
// unit rewritten into a non-reference picture of the main stream, interleaved main,aux,main,aux...
// and decoded with libavcodec's SOFTWARE h264 decoder, which is what FreeRDP clients use.
#include "avc444nalrewriter_p.h"
#include "h264bitstream_p.h"
#include "vaapih264_p.h"

#include <QDebug>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
}

namespace
{
// QP 12 (the private map's "quality 100") keeps the aux PSNR bar clear of the quantiser bound; the
// rewriter does not care about the QP, the reference chain does not either.
constexpr int W = 1280, H = 720, N = 48, QP = 12;

// The MAIN picture is static (a gradient with a soft-edged box that does not move): the guard
// below wants its P pictures near-empty, which motion would blur. The AUX picture moves its box
// 12 px per frame so every aux decode is checked against a different input; `aux` selects it.
AVFrame *makeFrame(int index, bool aux)
{
    AVFrame *f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUV420P; f->width = W; f->height = H;
    if (av_frame_get_buffer(f, 64) < 0) qFatal("alloc");
    const int bx = aux ? 100 + 12 * index : 300, by = aux ? 400 : 200;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        int v = aux ? ((x / 8 + y / 8) % 2 ? 140 : 110) : ((x + 2 * y) / 12) & 0xFF;
        const int dx = std::abs(x - bx - 48), dy = std::abs(y - by - 48);
        if (dx < 56 && dy < 56) v = std::clamp(v + 80 - std::max(0, std::max(dx, dy) - 40) * 10, 0, 255);
        f->data[0][y * f->linesize[0] + x] = uint8_t(v);
    }
    for (int y = 0; y < H / 2; ++y) for (int x = 0; x < W / 2; ++x) {
        f->data[1][y * f->linesize[1] + x] = uint8_t(aux ? 96 + (x / 16 % 4) * 8 : 128 + (y / 24 % 3) * 6);
        f->data[2][y * f->linesize[2] + x] = uint8_t(aux ? 160 : 128 - (x / 40 % 2) * 10);
    }
    return f;
}

struct Enc {
    VaapiH264::UploadGraph graph;
    AVCodecContext *codec = nullptr;
    std::vector<QByteArray> packets;
    bool open(AVBufferRef *device, int gop)
    {
        QString err;
        if (!VaapiH264::createUploadGraph(graph, device, QSize(W, H), {1, 1000}, &err)) { qWarning() << err; return false; }
        codec = VaapiH264::openContext({QSize(W, H), VaapiH264::Profile::Main, QP, gop, {1, 1000}}, av_buffersink_get_hw_frames_ctx(graph.out), &err);
        if (!codec) qWarning() << err;
        return codec != nullptr;
    }
    bool encode(AVFrame *in, int64_t pts)   // one picture in, its packet out (async_depth = 1)
    {
        in->pts = pts;
        if (av_buffersrc_add_frame(graph.in, in) < 0) return false;
        AVFrame *hw = av_frame_alloc();
        if (av_buffersink_get_frame(graph.out, hw) < 0) return false;
        const int sent = avcodec_send_frame(codec, hw);
        av_frame_free(&hw);
        if (sent < 0) return false;
        AVPacket *pkt = av_packet_alloc();
        const int got = avcodec_receive_packet(codec, pkt);
        if (got >= 0) packets.emplace_back(reinterpret_cast<const char *>(pkt->data), pkt->size);
        av_packet_free(&pkt);
        return got >= 0 || got == AVERROR(EAGAIN);
    }
    void flush()
    {
        avcodec_send_frame(codec, nullptr);
        AVPacket *pkt = av_packet_alloc();
        while (avcodec_receive_packet(codec, pkt) >= 0) { packets.emplace_back(reinterpret_cast<const char *>(pkt->data), pkt->size); av_packet_unref(pkt); }
        av_packet_free(&pkt);
    }
    ~Enc() { if (codec) avcodec_free_context(&codec); }
};

// libavcodec reports the failures that matter here BELOW warning level and then conceals them
// ("Frame num gap" is DEBUG and the concealment shares the previous reference's pixels, so a
// wrong frame_num rule can decode bit-identically; "no picture ooo" DEBUG = a picture dropped as
// out of order; "concealing" and "Increasing reorder buffer" are INFO; "Invalid POC" VERBOSE).
// A custom callback sees every level; fail on the deny-list at any level and on anything at
// warning level or worse.
int g_decoderWarnings = 0;
QStringList g_decoderMessages;
void captureLog(void *avcl, int level, const char *fmt, va_list vl)
{
    static const char *deny[] = {"Frame num gap", "no picture", "reorder buffer", "Invalid POC", "concealing", "issing reference", "no frame", "exceeds max", "corrupt", "invalid", "rror"};
    char buf[512]; vsnprintf(buf, sizeof buf, fmt, vl);
    bool denied = level <= AV_LOG_WARNING;
    for (const char *d : deny) if (!denied && strstr(buf, d)) denied = true;
    if (denied) {
        g_decoderWarnings++;
        if (g_decoderMessages.size() < 20) g_decoderMessages << QStringLiteral("[%1] %2").arg(level).arg(QString::fromLatin1(buf).trimmed());
    }
    Q_UNUSED(avcl);
}

// Feeds the access units one by one (each is one AVPacket with a sequence pts) and collects the pictures in output order.
std::vector<AVFrame *> decode(const std::vector<QByteArray> &accessUnits)
{
    const AVCodec *dec = avcodec_find_decoder_by_name("h264");
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_count = 1;   // frame threading changes when prev_frame_num is updated; keep the walk deterministic
    if (avcodec_open2(ctx, dec, nullptr) < 0) qFatal("h264 decoder");
    std::vector<AVFrame *> out;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    auto drain = [&]() { while (avcodec_receive_frame(ctx, frame) >= 0) { out.push_back(av_frame_clone(frame)); av_frame_unref(frame); } };
    for (size_t i = 0; i < accessUnits.size(); ++i) {
        av_new_packet(pkt, accessUnits[i].size());
        memcpy(pkt->data, accessUnits[i].constData(), accessUnits[i].size());
        pkt->pts = int64_t(i);
        avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        drain();
    }
    avcodec_send_packet(ctx, nullptr);
    drain();
    av_packet_free(&pkt); av_frame_free(&frame); avcodec_free_context(&ctx);
    return out;
}

// Decoder-independent conformance walk over the interleaved stream with the plan's own parsers:
// H.264 7.4.3 (frame_num == (PrevRefFrameNum + 1) % Max for every non-IDR picture, PrevRefFrameNum
// advanced by reference pictures only), 8.2.1.3 (POC type 2 strictly increasing), and the design's
// "never two non-reference pictures in a row". This is what mstsc holds the stream to, whatever
// libavcodec tolerates.
QString conformanceWalk(const std::vector<QByteArray> &accessUnits)
{
    std::optional<Avc444Nal::Sps> sps;
    int prevRefFrameNum = -1, prevFrameNum = 0, prevFrameNumOffset = 0; bool prevNonRef = false;
    long long lastPoc = -1;
    int index = 0;
    for (const QByteArray &au : accessUnits) {
        const auto nals = H264Bits::splitAnnexB(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(au.constData()), size_t(au.size())));
        bool sawSlice = false;
        for (const auto nal : nals) {
            const auto hdr = H264Bits::NalHeader::parse(nal[0]);
            const auto rbsp = H264Bits::ebspToRbsp(nal.subspan(1));
            if (hdr.type == 7) { sps = Avc444Nal::parseSps(rbsp); continue; }
            if (hdr.type != 1 && hdr.type != 5) continue;
            if (sawSlice) continue;   // one check per picture
            sawSlice = true;
            if (!sps) return QStringLiteral("AU %1: slice before any SPS").arg(index);
            if (sps->pocType != 2) return QStringLiteral("AU %1: POC type %2, the walk covers type 2 only").arg(index).arg(sps->pocType);
            const auto head = Avc444Nal::parseSliceHead(rbsp, hdr.type == 5, *sps);
            if (!head) return QStringLiteral("AU %1: slice header did not parse").arg(index);
            const int maxFrameNum = 1 << sps->log2MaxFrameNum;
            const bool ref = hdr.refIdc != 0, idr = hdr.type == 5;
            if (!ref && prevNonRef) return QStringLiteral("AU %1: two consecutive non-reference pictures").arg(index);
            if (idr) { if (head->frameNum != 0) return QStringLiteral("AU %1: IDR with frame_num %2").arg(index).arg(head->frameNum); }
            else if (prevRefFrameNum < 0 || head->frameNum != (prevRefFrameNum + 1) % maxFrameNum)
                return QStringLiteral("AU %1: frame_num %2, expected %3 (PrevRefFrameNum %4)").arg(index).arg(head->frameNum).arg((prevRefFrameNum + 1) % maxFrameNum).arg(prevRefFrameNum);
            // 8.2.1.3 (prevFrameNum / prevFrameNumOffset are those of the previous picture in decoding
            // order, reference or not; after an IDR they are 0 / 0)
            const int frameNumOffset = idr ? 0 : (prevFrameNum > head->frameNum ? prevFrameNumOffset + maxFrameNum : prevFrameNumOffset);
            const long long poc = idr ? 0 : 2LL * (frameNumOffset + head->frameNum) - (ref ? 0 : 1);
            if (poc <= lastPoc) return QStringLiteral("AU %1: POC %2 not above the previous %3").arg(index).arg(poc).arg(lastPoc);
            lastPoc = poc; prevFrameNum = head->frameNum; prevFrameNumOffset = frameNumOffset; prevNonRef = !ref;
            if (ref) prevRefFrameNum = head->frameNum;
        }
        ++index;
    }
    return QString();
}

bool planesEqual(const AVFrame *a, const AVFrame *b)
{
    for (int p = 0; p < 3; ++p) {
        const int w = p ? W / 2 : W, h = p ? H / 2 : H;
        for (int y = 0; y < h; ++y) if (memcmp(a->data[p] + y * a->linesize[p], b->data[p] + y * b->linesize[p], size_t(w)) != 0) return false;
    }
    return true;
}
double psnrPlane(const AVFrame *a, const AVFrame *b, int p)
{
    const int w = p ? W / 2 : W, h = p ? H / 2 : H;
    double se = 0;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { const double d = double(a->data[p][y * a->linesize[p] + x]) - double(b->data[p][y * b->linesize[p] + x]); se += d * d; }
    const double mse = se / (double(w) * h);
    return mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
}
}

class Avc444StreamTest : public QObject
{
    Q_OBJECT
    AVBufferRef *m_device = nullptr;

private Q_SLOTS:
    void initTestCase()
    {
        QString err;
        m_device = VaapiH264::createDevice(qgetenv("KPIPEWIRE_TEST_RENDER_NODE").isEmpty() ? QByteArrayLiteral("/dev/dri/renderD128") : qgetenv("KPIPEWIRE_TEST_RENDER_NODE"), &err);
        if (!m_device) QSKIP(qPrintable(QStringLiteral("no VAAPI device: ") + err));
        Enc probe;
        if (!probe.open(m_device, 600)) QSKIP("h264_vaapi does not open on this device");
    }
    void cleanupTestCase() { if (m_device) av_buffer_unref(&m_device); }

    void interleavedNonReferenceAuxDecodesLikeMainOnly()
    {
        Enc main, aux;
        QVERIFY(main.open(m_device, 600));
        QVERIFY(aux.open(m_device, 1));
        std::vector<AVFrame *> mainIn, auxIn;
        for (int i = 0; i < N; ++i) {
            mainIn.push_back(makeFrame(i, false)); auxIn.push_back(makeFrame(i, true));
            AVFrame *m = av_frame_clone(mainIn.back()), *a = av_frame_clone(auxIn.back());
            QVERIFY(main.encode(m, i)); QVERIFY(aux.encode(a, i));
            av_frame_free(&m); av_frame_free(&a);
        }
        main.flush(); aux.flush();
        QCOMPARE(int(main.packets.size()), N);
        QCOMPARE(int(aux.packets.size()), N);

        // The x740 guard: the main context never sees an aux picture, so with a static main its P
        // pictures stay near-empty (the broken single-context design made every P cost an I: 100 %).
        const qint64 iSize = main.packets[0].size();
        qint64 pSum = 0; for (int i = 1; i < N; ++i) pSum += main.packets[i].size();
        const double pAvg = double(pSum) / (N - 1);
        qInfo("main I %lld B, main P avg %.0f B, aux avg %.0f B", iSize, pAvg, double([&] { qint64 s = 0; for (auto &p : aux.packets) s += p.size(); return s; }()) / aux.packets.size());
        QVERIFY2(pAvg < 0.10 * iSize, "main P pictures cost like I pictures: the main chain saw the aux pictures");

        // Rewrite and interleave in the shape the produce path emits: main[i] + aux[i] for the first
        // N-1 frames, main[N-1] luma-only (as during motion), then the at-rest refresh = the chroma of
        // that last frame as an aux-only access unit. The rewrite happens at emission time, right after
        // the main it follows was tracked (the K3 contract).
        Avc444Nal::MainStreamState state;
        Avc444Nal::Rewriter rewriter;
        std::vector<QByteArray> interleaved, mainOnly;
        auto rewriteAux = [&](int i) {
            const auto r = rewriter.rewrite(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(aux.packets[i].constData()), size_t(aux.packets[i].size())), state);
            if (r.status != Avc444Nal::Rewriter::Status::Ok) qWarning() << "rewrite" << i << Avc444Nal::Rewriter::statusName(r.status) << r.detail;
            return r;
        };
        for (int i = 0; i < N; ++i) {
            const auto mainBytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(main.packets[i].constData()), size_t(main.packets[i].size()));
            QVERIFY(state.trackMainPacket(mainBytes, i));
            QCOMPARE(state.lastTrackedPts, int64_t(i)); QCOMPARE(state.serial, i + 1);
            mainOnly.push_back(main.packets[i]);
            interleaved.push_back(main.packets[i]);
            if (i < N - 1) {
                const auto r = rewriteAux(i);
                QVERIFY(r.status == Avc444Nal::Rewriter::Status::Ok);
                interleaved.emplace_back(reinterpret_cast<const char *>(r.annexB.data()), int(r.annexB.size()));
            }
        }
        {   // the at-rest refresh after the luma-only main[N-1]: still tracked against that main
            const auto r = rewriteAux(N - 1);
            QVERIFY(r.status == Avc444Nal::Rewriter::Status::Ok);
            interleaved.emplace_back(reinterpret_cast<const char *>(r.annexB.data()), int(r.annexB.size()));
        }
        QCOMPARE(state.lastFrameNum, (N - 1) % (1 << state.sps->log2MaxFrameNum));
        qInfo("main SPS: poc type %d log2_max_frame_num %d cabac %d", state.sps->pocType, state.sps->log2MaxFrameNum, state.pps->cabac);

        // Spec-level conformance of the interleaved stream, independent of any decoder.
        const QString violation = conformanceWalk(interleaved);
        QVERIFY2(violation.isEmpty(), qPrintable(violation));

        // Decode both with the software decoder; the interleaved stream must raise no denied message.
        const auto refPictures = decode(mainOnly);
        g_decoderWarnings = 0; g_decoderMessages.clear();
        av_log_set_callback(captureLog);
        const auto pictures = decode(interleaved);
        av_log_set_callback(av_log_default_callback);
        QVERIFY2(g_decoderWarnings == 0, qPrintable(g_decoderMessages.join(u" | ")));
        QCOMPARE(int(refPictures.size()), N);
        QCOMPARE(int(pictures.size()), 2 * N);   // (N-1) pairs + luma-only main + the refresh
        for (size_t i = 1; i < pictures.size(); ++i) QVERIFY2(pictures[i]->pts > pictures[i - 1]->pts, "pictures came out of order");

        double auxMinY = 99, auxMinU = 99, auxMinV = 99;
        for (int i = 0; i < N - 1; ++i) {
            QVERIFY2(planesEqual(pictures[2 * i], refPictures[i]), qPrintable(QStringLiteral("main picture %1 differs from the main-only decode").arg(i)));
            auxMinY = std::min(auxMinY, psnrPlane(pictures[2 * i + 1], auxIn[i], 0));
            auxMinU = std::min(auxMinU, psnrPlane(pictures[2 * i + 1], auxIn[i], 1));
            auxMinV = std::min(auxMinV, psnrPlane(pictures[2 * i + 1], auxIn[i], 2));
        }
        QVERIFY2(planesEqual(pictures[2 * N - 2], refPictures[N - 1]), "the luma-only main differs from the main-only decode");
        const double restY = psnrPlane(pictures[2 * N - 1], auxIn[N - 1], 0), restU = psnrPlane(pictures[2 * N - 1], auxIn[N - 1], 1), restV = psnrPlane(pictures[2 * N - 1], auxIn[N - 1], 2);
        qInfo("aux PSNR min Y %.1f U %.1f V %.1f dB; rest-refresh Y %.1f U %.1f V %.1f dB", auxMinY, auxMinU, auxMinV, restY, restU, restV);
        QVERIFY(auxMinY >= 45.0); QVERIFY(auxMinU >= 45.0); QVERIFY(auxMinV >= 45.0);
        QVERIFY(restY >= 45.0); QVERIFY(restU >= 45.0); QVERIFY(restV >= 45.0);

        for (auto *f : mainIn) av_frame_free(&f);
        for (auto *f : auxIn) av_frame_free(&f);
        for (auto *f : refPictures) { AVFrame *x = f; av_frame_free(&x); }
        for (auto *f : pictures) { AVFrame *x = f; av_frame_free(&x); }
    }
};

QTEST_GUILESS_MAIN(Avc444StreamTest)
#include "avc444streamtest.moc"
```

`src/autotests/CMakeLists.txt`:

```cmake
ecm_add_test(avc444nalrewritertest.cpp ../avc444nalrewriter.cpp TEST_NAME avc444nalrewritertest LINK_LIBRARIES Qt::Test Qt::Core)
target_include_directories(avc444nalrewritertest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
# GPU test: real h264_vaapi contexts on the render node, software h264 decode. Skips without VAAPI.
ecm_add_test(avc444streamtest.cpp ../avc444nalrewriter.cpp ../vaapih264.cpp TEST_NAME avc444streamtest
             LINK_LIBRARIES Qt::Test Qt::Core Qt::Gui PkgConfig::AVCodec PkgConfig::AVUtil PkgConfig::AVFilter)
target_include_directories(avc444streamtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 2: The pure unit tests for the bit tools and the rewriter**

`src/autotests/avc444nalrewritertest.cpp`:

```cpp
#include "avc444nalrewriter_p.h"
#include "h264bitstream_p.h"
#include <QTest>

#include <initializer_list>

using namespace H264Bits;
using namespace Avc444Nal;

namespace
{
// A minimal SPS RBSP as h264_vaapi writes it for Main profile: no scaling matrix, poc type 2, frame_mbs_only.
std::vector<uint8_t> makeSps(int log2MaxFrameNum = 8, int pocType = 2, int maxRefs = 1, int widthMbsMinus1 = 79, int heightMbsMinus1 = 44)
{
    BitWriter w;
    w.u(77, 8); w.u(0, 8); w.u(40, 8);      // profile_idc 77 (Main), constraint flags, level 4.0
    w.ue(0);                                // sps id
    w.ue(log2MaxFrameNum - 4);
    w.ue(uint32_t(pocType));
    if (pocType == 0) w.ue(4);              // log2_max_pic_order_cnt_lsb_minus4 -> 8 bits
    w.ue(uint32_t(maxRefs)); w.bit(false);  // max_num_ref_frames, gaps
    w.ue(uint32_t(widthMbsMinus1)); w.ue(uint32_t(heightMbsMinus1));
    w.bit(true);                            // frame_mbs_only
    w.bit(true);                            // direct_8x8_inference
    w.bit(false);                           // frame_cropping
    w.bit(false);                           // vui
    w.trailingBits();
    return w.take();
}
std::vector<uint8_t> makePps(int ppsId, bool cabac, bool deblockingControl)
{
    BitWriter w;
    w.ue(uint32_t(ppsId)); w.ue(0);         // pps id, sps id
    w.bit(cabac); w.bit(false);             // entropy_coding_mode, bottom_field_pic_order_in_frame_present
    w.ue(0);                                // num_slice_groups_minus1
    w.ue(0); w.ue(0);                       // num_ref_idx defaults
    w.bit(false); w.u(0, 2);                // weighted pred / bipred
    w.se(-8); w.se(0); w.se(0);             // pic_init_qp_minus26, qs, chroma_qp_index_offset
    w.bit(deblockingControl); w.bit(false); w.bit(false); // deblocking control, constrained intra, redundant pic cnt
    w.trailingBits();
    return w.take();
}
// An IDR I-slice header + fake slice data (CABAC: aligned bytes; CAVLC: a bit string), as the intra context would emit.
std::vector<uint8_t> makeIdrSlice(const Sps &sps, const Pps &pps, int frameNum, int idrPicId, int sliceQpDelta, const std::vector<uint8_t> &data)
{
    BitWriter w;
    w.ue(0); w.ue(7); w.ue(uint32_t(pps.ppsId));         // first_mb, slice_type I(7), pps id
    w.u(uint32_t(frameNum), sps.log2MaxFrameNum);
    w.ue(uint32_t(idrPicId));
    if (sps.pocType == 0) w.u(6, sps.log2MaxPocLsb);      // pic_order_cnt_lsb
    w.bit(false); w.bit(false);                           // dec_ref_pic_marking for an IDR
    w.se(sliceQpDelta);
    if (pps.deblockingControlPresent) { w.ue(0); w.se(-1); w.se(2); }
    if (pps.cabac) { w.alignOnes(); w.appendBytes(data); }
    else { for (uint8_t b : data) w.u(b, 8); w.trailingBits(); }
    return w.take();
}
std::vector<uint8_t> nal(int refIdc, int type, const std::vector<uint8_t> &rbsp)
{
    std::vector<uint8_t> out{0, 0, 0, 1, NalHeader::make(refIdc, type)};
    const auto ebsp = rbspToEbsp(rbsp);
    out.insert(out.end(), ebsp.begin(), ebsp.end());
    return out;
}
std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts) { std::vector<uint8_t> o; for (auto &p : parts) o.insert(o.end(), p.begin(), p.end()); return o; }
}

class Avc444NalRewriterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void bitsRoundTrip()
    {
        BitWriter w;
        w.ue(0); w.ue(1); w.ue(2); w.ue(255); w.se(0); w.se(-3); w.se(7); w.u(0x2A5, 10); w.bit(true); w.trailingBits();
        const auto bytes = w.take();
        BitReader r(bytes);
        QCOMPARE(r.ue(), 0u); QCOMPARE(r.ue(), 1u); QCOMPARE(r.ue(), 2u); QCOMPARE(r.ue(), 255u);
        QCOMPARE(r.se(), 0); QCOMPARE(r.se(), -3); QCOMPARE(r.se(), 7); QCOMPARE(r.u(10), 0x2A5u); QVERIFY(r.flag());
        QCOMPARE(BitReader::lastOneBit(bytes), r.position());
    }
    void emulationPrevention()
    {
        const std::vector<uint8_t> rbsp{0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0x7F, 0x00, 0x00};
        const auto ebsp = rbspToEbsp(rbsp);
        QCOMPARE(ebsp, (std::vector<uint8_t>{0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x03, 0x7F, 0x00, 0x00, 0x03}));
        QCOMPARE(ebspToRbsp(ebsp), rbsp);
        const std::vector<uint8_t> stream{0, 0, 0, 1, 0x67, 0xAA, 0, 0, 1, 0x68, 0xBB, 0xCC, 0, 0, 0, 1, 0x65, 0xDD};
        const auto nals = splitAnnexB(stream);
        QCOMPARE(nals.size(), size_t(3));
        QCOMPARE(nals[0].size(), size_t(2)); QCOMPARE(nals[1].size(), size_t(3)); QCOMPARE(nals[2][0], uint8_t(0x65));
    }
    void parsesSpsAndPps()
    {
        const auto sps = parseSps(makeSps(8, 2, 1, 79, 44));
        QVERIFY(sps); QCOMPARE(sps->profileIdc, 77); QCOMPARE(sps->log2MaxFrameNum, 8); QCOMPARE(sps->pocType, 2);
        QCOMPARE(sps->picWidthInMbsMinus1, 79); QVERIFY(sps->frameMbsOnly); QVERIFY(sps->direct8x8); QCOMPARE(sps->maxNumRefFrames, 1);
        QVERIFY(sps->parseRelevantEquals(*parseSps(makeSps(8, 2, 0, 79, 44))));   // max_num_ref_frames may differ
        QVERIFY(!sps->parseRelevantEquals(*parseSps(makeSps(4, 2, 1, 79, 44))));  // log2_max_frame_num may not
        QVERIFY(!sps->parseRelevantEquals(*parseSps(makeSps(8, 0, 1, 79, 44))));  // poc type may not
        const auto pps = parsePps(makePps(0, true, true));
        QVERIFY(pps); QVERIFY(pps->cabac); QVERIFY(pps->deblockingControlPresent); QCOMPARE(pps->picInitQpMinus26, -8);
    }
    void tracksTheMainStream()
    {
        const auto sps = *parseSps(makeSps()); const auto pps = *parsePps(makePps(0, true, true));
        MainStreamState st;
        QVERIFY(st.trackMainPacket(cat({nal(3, 7, makeSps()), nal(3, 8, makePps(0, true, true)), nal(3, 5, makeIdrSlice(sps, pps, 0, 0, -2, {0x12, 0x34, 0x80}))}), 100));
        QVERIFY(st.haveIdr); QCOMPARE(st.lastFrameNum, 0); QVERIFY(st.sps); QVERIFY(st.pps); QCOMPARE(st.serial, 1); QCOMPARE(st.lastTrackedPts, int64_t(100));
        // a P slice: nal type 1, frame_num 1 (first_mb 0, slice_type 5 (P), pps 0, frame_num, then whatever - the tracker stops at frame_num for poc type 2)
        BitWriter w; w.ue(0); w.ue(5); w.ue(0); w.u(1, 8); w.trailingBits();
        QVERIFY(st.trackMainPacket(nal(2, 1, w.take()), 116));
        QCOMPARE(st.lastFrameNum, 1); QCOMPARE(st.serial, 2); QCOMPARE(st.lastTrackedPts, int64_t(116));
        // A non-reference main slice (nal_ref_idc 0) does not move PrevRefFrameNum (7.4.3).
        BitWriter w2; w2.ue(0); w2.ue(5); w2.ue(0); w2.u(2, 8); w2.trailingBits();
        QVERIFY(st.trackMainPacket(nal(0, 1, w2.take()), 132));
        QCOMPARE(st.lastFrameNum, 1); QCOMPARE(st.serial, 3);
    }
    void rewritesAnIdrIntoANonReferencePicture_data()
    {
        QTest::addColumn<bool>("cabac"); QTest::addColumn<bool>("deblock"); QTest::addColumn<int>("pocType");
        QTest::newRow("cabac deblock poc2") << true << true << 2;
        QTest::newRow("cavlc poc2") << false << false << 2;
        QTest::newRow("cabac poc0") << true << false << 0;
    }
    void rewritesAnIdrIntoANonReferencePicture()
    {
        QFETCH(bool, cabac); QFETCH(bool, deblock); QFETCH(int, pocType);
        const auto spsBytes = makeSps(8, pocType);
        const auto sps = *parseSps(spsBytes);
        const auto ppsMain = *parsePps(makePps(0, cabac, deblock));
        MainStreamState st;
        QVERIFY(st.trackMainPacket(cat({nal(3, 7, spsBytes), nal(3, 8, makePps(0, cabac, deblock)), nal(3, 5, makeIdrSlice(sps, ppsMain, 0, 0, 0, {0xAB, 0xCD, 0x80}))})));
        st.lastFrameNum = 37; st.lastPocLsb = 74;   // pretend 37 main pictures went by

        const std::vector<uint8_t> data{0x01, 0x00, 0x00, 0x02, 0x9F, 0x00, 0x00, 0x00, 0x80}; // contains sequences that need emulation prevention
        const auto auxAu = cat({nal(3, 7, makeSps(8, pocType, 0)), nal(3, 8, makePps(0, cabac, deblock)), nal(0, 6, {0x05, 0x01, 0x00, 0x80}), nal(3, 5, makeIdrSlice(sps, ppsMain, 0, 3, -4, data))});
        Rewriter rw;
        const auto r = rw.rewrite(auxAu, st);
        QVERIFY2(r.status == Rewriter::Status::Ok, r.detail.toLatin1().constData());

        const auto nals = splitAnnexB(r.annexB);
        QCOMPARE(nals.size(), size_t(2));                                   // PPS' + slice' (SPS and SEI dropped)
        QCOMPARE(NalHeader::parse(nals[0][0]).type, 8);
        const auto pps1 = parsePps(ebspToRbsp(nals[0].subspan(1)));
        QVERIFY(pps1); QCOMPARE(pps1->ppsId, 1); QCOMPARE(pps1->cabac, cabac); QCOMPARE(pps1->deblockingControlPresent, deblock); QCOMPARE(pps1->picInitQpMinus26, -8);
        const auto hdr = NalHeader::parse(nals[1][0]);
        QCOMPARE(hdr.refIdc, 0); QCOMPARE(hdr.type, 1);
        const auto rbsp = ebspToRbsp(nals[1].subspan(1));
        BitReader rd(rbsp);
        QCOMPARE(rd.ue(), 0u); QCOMPARE(rd.ue(), 7u); QCOMPARE(rd.ue(), 1u);                 // first_mb, slice_type, pps id 1
        QCOMPARE(rd.u(8), 38u);                                                                // frame_num = main + 1
        if (pocType == 0) QCOMPARE(rd.u(8), 75u);                                              // poc lsb = main + 1
        QCOMPARE(rd.se(), -4);                                                                 // slice_qp_delta (no idr_pic_id, no marking bits)
        if (deblock) { QCOMPARE(rd.ue(), 0u); QCOMPARE(rd.se(), -1); QCOMPARE(rd.se(), 2); }
        if (cabac) {
            rd.skipAlignmentOnes();
            QVERIFY(rd.byteAligned());
            const std::vector<uint8_t> tail(rbsp.begin() + long(rd.bytePosition()), rbsp.end());
            QCOMPARE(tail, data);                                                              // slice data verbatim
        } else {
            for (uint8_t b : data) QCOMPARE(rd.u(8), uint32_t(b));
            QVERIFY(rd.flag());                                                                // rbsp_stop_one_bit
            while (rd.bitsLeft()) QVERIFY(!rd.flag());
        }
    }
    void refusesWhatItCannotRewrite()
    {
        const auto sps = *parseSps(makeSps()); const auto pps = *parsePps(makePps(0, true, false));
        MainStreamState st;
        Rewriter rw;
        // No main SPS yet.
        QCOMPARE(int(rw.rewrite(cat({nal(3, 7, makeSps()), nal(3, 8, makePps(0, true, false)), nal(3, 5, makeIdrSlice(sps, pps, 0, 0, 0, {0x80}))}), st).status), int(Rewriter::Status::NoMainSps));
        QVERIFY(st.trackMainPacket(cat({nal(3, 7, makeSps()), nal(3, 8, makePps(0, true, false)), nal(3, 5, makeIdrSlice(sps, pps, 0, 0, 0, {0x80}))})));
        // Aux SPS with another log2_max_frame_num: mismatch.
        const auto other = *parseSps(makeSps(4));
        QCOMPARE(int(rw.rewrite(cat({nal(3, 7, makeSps(4)), nal(3, 8, makePps(0, true, false)), nal(3, 5, makeIdrSlice(other, pps, 0, 0, 0, {0x80}))}), st).status), int(Rewriter::Status::SpsMismatch));
        // An aux PPS pointing at another SPS id than the main's: unsupported.
        {
            BitWriter w; w.ue(0); w.ue(1); w.bit(true); w.bit(false); w.ue(0); w.ue(0); w.ue(0); w.bit(false); w.u(0, 2); w.se(-8); w.se(0); w.se(0); w.bit(false); w.bit(false); w.bit(false); w.trailingBits();
            Rewriter rw3;
            QCOMPARE(int(rw3.rewrite(cat({nal(3, 7, makeSps()), nal(3, 8, w.take()), nal(3, 5, makeIdrSlice(sps, pps, 0, 0, 0, {0x80}))}), st).status), int(Rewriter::Status::Unsupported));
        }
        // A P slice from the "intra" context: unsupported.
        Rewriter rw2;
        BitWriter w; w.ue(0); w.ue(5); w.ue(0); w.u(0, 8); w.trailingBits();
        QCOMPARE(int(rw2.rewrite(cat({nal(3, 7, makeSps()), nal(3, 8, makePps(0, true, false)), nal(2, 1, w.take())}), st).status), int(Rewriter::Status::Unsupported));
    }
    void wrapsFrameNumAndKeepsPpsWhenAnAuLacksOne()
    {
        const auto sps = *parseSps(makeSps(4)); const auto pps = *parsePps(makePps(0, false, false));
        MainStreamState st;
        QVERIFY(st.trackMainPacket(cat({nal(3, 7, makeSps(4)), nal(3, 8, makePps(0, false, false)), nal(3, 5, makeIdrSlice(sps, pps, 0, 0, 0, {0x80}))})));
        st.lastFrameNum = 15;
        Rewriter rw;
        QVERIFY(rw.rewrite(cat({nal(3, 7, makeSps(4, 2, 0)), nal(3, 8, makePps(0, false, false)), nal(3, 5, makeIdrSlice(sps, pps, 0, 0, 0, {0x80}))}), st).status == Rewriter::Status::Ok);
        const auto r = rw.rewrite(nal(3, 5, makeIdrSlice(sps, pps, 0, 1, 0, {0x80})), st);   // slice only
        QVERIFY(r.status == Rewriter::Status::Ok);
        const auto nals = splitAnnexB(r.annexB);
        QCOMPARE(nals.size(), size_t(2));                                                     // the remembered PPS' is re-sent
        BitReader rd(ebspToRbsp(nals[1].subspan(1)));
        rd.ue(); rd.ue(); rd.ue();
        QCOMPARE(rd.u(4), 0u);                                                                // (15 + 1) mod 16
    }
};
QTEST_GUILESS_MAIN(Avc444NalRewriterTest)
#include "avc444nalrewritertest.moc"
```

- [ ] **Step 3: Run both to verify they fail**

Run: `cmake -S ~/dev/kpipewire -B ~/dev/kpipewire/build-tests -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF -DCMAKE_INSTALL_PREFIX=/tmp/kpipewire-tests-prefix 2>&1 | tail -2; cmake --build ~/dev/kpipewire/build-tests -j16 --target avc444nalrewritertest avc444streamtest 2>&1 | tail -3`
Expected: FAIL — missing headers.

- [ ] **Step 4: The bit tools**

`src/h264bitstream_p.h`:

```cpp
/*
    SPDX-FileCopyrightText: 2026 Steve Westerhouse <steve.westerhouse@origami-analytics.com>
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace H264Bits
{
inline std::vector<uint8_t> ebspToRbsp(std::span<const uint8_t> ebsp)
{
    std::vector<uint8_t> out;
    out.reserve(ebsp.size());
    int zeros = 0;
    for (uint8_t b : ebsp) {
        if (zeros >= 2 && b == 0x03) { zeros = 0; continue; }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}
inline std::vector<uint8_t> rbspToEbsp(std::span<const uint8_t> rbsp)
{
    std::vector<uint8_t> out;
    out.reserve(rbsp.size() + rbsp.size() / 32 + 4);
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 0x03) { out.push_back(0x03); zeros = 0; }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    if (!out.empty() && out.back() == 0x00) out.push_back(0x03); // 7.4.1.1: a trailing 0x00 (cabac_zero_word) gets a final 0x03
    return out;
}
inline std::vector<std::span<const uint8_t>> splitAnnexB(std::span<const uint8_t> bytes)
{
    std::vector<std::span<const uint8_t>> nals;
    size_t i = 0, start = std::string::npos;
    while (i + 2 < bytes.size()) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
            if (start != std::string::npos) {
                size_t end = i;
                while (end > start && bytes[end - 1] == 0) --end; // the 4-byte start code's leading zero, trailing_zero_8bits
                nals.push_back(bytes.subspan(start, end - start));
            }
            i += 3; start = i; continue;
        }
        ++i;
    }
    if (start != std::string::npos && start < bytes.size()) nals.push_back(bytes.subspan(start));
    return nals;
}
struct NalHeader {
    int refIdc; int type;
    static NalHeader parse(uint8_t b) { return {(b >> 5) & 3, b & 31}; }
    static uint8_t make(int refIdc, int type) { return uint8_t(((refIdc & 3) << 5) | (type & 31)); }
};

class BitReader
{
public:
    explicit BitReader(std::span<const uint8_t> data) : m_data(data) {}
    uint32_t u(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }
    bool flag() { return bit() != 0; }
    uint32_t ue()
    {
        int zeros = 0;
        while (bit() == 0) { if (++zeros > 31) throw std::out_of_range("ue overflow"); }
        return zeros == 0 ? 0 : ((1u << zeros) - 1 + u(zeros));
    }
    int32_t se()
    {
        const uint32_t k = ue();
        return (k & 1) ? int32_t((k + 1) / 2) : -int32_t(k / 2);
    }
    size_t position() const { return m_pos; }
    size_t bitsLeft() const { return m_data.size() * 8 - m_pos; }
    void skip(size_t bits) { if (bits > bitsLeft()) throw std::out_of_range("skip"); m_pos += bits; }
    bool byteAligned() const { return m_pos % 8 == 0; }
    size_t bytePosition() const { return m_pos / 8; }
    void skipAlignmentOnes()
    {
        while (!byteAligned()) { if (bit() != 1) throw std::out_of_range("cabac_alignment_one_bit is 0"); }
    }
    static size_t lastOneBit(std::span<const uint8_t> rbsp)
    {
        for (size_t i = rbsp.size(); i-- > 0;) {
            if (rbsp[i] == 0) continue;
            int b = 0; while (!(rbsp[i] & (1 << b))) ++b;
            return i * 8 + (7 - b);
        }
        return std::string::npos;
    }
private:
    uint32_t bit()
    {
        if (m_pos >= m_data.size() * 8) throw std::out_of_range("read past end");
        const uint32_t v = (m_data[m_pos / 8] >> (7 - m_pos % 8)) & 1;
        ++m_pos;
        return v;
    }
    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
};

class BitWriter
{
public:
    void bit(bool b)
    {
        if (m_bits % 8 == 0) m_out.push_back(0);
        if (b) m_out.back() |= uint8_t(1 << (7 - m_bits % 8));
        ++m_bits;
    }
    void u(uint32_t value, int n) { for (int i = n - 1; i >= 0; --i) bit((value >> i) & 1); }
    void ue(uint32_t v)
    {
        const uint64_t x = uint64_t(v) + 1;
        int len = 0; while ((x >> len) > 1) ++len;
        u(0, len); u(uint32_t(x), len + 1);
    }
    void se(int32_t v) { ue(v > 0 ? uint32_t(2 * v - 1) : uint32_t(-2 * int64_t(v))); }
    void copyBits(BitReader &from, size_t count) { for (size_t i = 0; i < count; ++i) bit(from.flag()); }
    void alignOnes() { while (m_bits % 8) bit(true); }
    void appendBytes(std::span<const uint8_t> bytes)
    {
        if (m_bits % 8) throw std::logic_error("appendBytes unaligned");
        m_out.insert(m_out.end(), bytes.begin(), bytes.end());
        m_bits += bytes.size() * 8;
    }
    void trailingBits() { bit(true); while (m_bits % 8) bit(false); }
    size_t bitPosition() const { return m_bits; }
    bool byteAligned() const { return m_bits % 8 == 0; }
    std::vector<uint8_t> take() { if (m_bits % 8) throw std::logic_error("take unaligned"); m_bits = 0; return std::move(m_out); }
private:
    std::vector<uint8_t> m_out;
    size_t m_bits = 0;
};
}
```

(`std::string::npos` needs `<string>`; include it.)

- [ ] **Step 5: The rewriter**

`src/avc444nalrewriter.cpp` (header per the interface block):

```cpp
#include "avc444nalrewriter_p.h"
#include "h264bitstream_p.h"

#include <QString>

#include <algorithm>
#include <iterator>

using namespace H264Bits;

namespace Avc444Nal
{
namespace
{
constexpr int kHighProfiles[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
bool isHighProfile(int p) { for (int h : kHighProfiles) if (h == p) return true; return false; }
void readScalingList(BitReader &r, int size, std::vector<int32_t> &out)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0) { const int32_t delta = r.se(); out.push_back(delta); next = (last + delta + 256) % 256; }
        last = next == 0 ? last : next;
    }
}
std::vector<uint8_t> startCode() { return {0, 0, 0, 1}; }
void append(std::vector<uint8_t> &out, const std::vector<uint8_t> &v) { out.insert(out.end(), v.begin(), v.end()); }
}

std::optional<Sps> parseSps(std::span<const uint8_t> rbsp)
{
    try {
        BitReader r(rbsp);
        Sps s;
        s.profileIdc = int(r.u(8)); r.u(8); s.levelIdc = int(r.u(8)); s.spsId = int(r.ue());
        if (isHighProfile(s.profileIdc)) {
            s.chromaFormatIdc = int(r.ue());
            if (s.chromaFormatIdc == 3) s.separateColourPlane = r.flag();
            s.bitDepthLumaMinus8 = int(r.ue()); s.bitDepthChromaMinus8 = int(r.ue()); s.qpprimeBypass = r.flag();
            s.scalingMatrixPresent = r.flag();
            if (s.scalingMatrixPresent) {
                for (int i = 0; i < (s.chromaFormatIdc != 3 ? 8 : 12); ++i) {
                    const bool present = r.flag(); s.scalingLists.push_back(present);
                    if (present) readScalingList(r, i < 6 ? 16 : 64, s.scalingLists);
                }
            }
        }
        s.log2MaxFrameNum = int(r.ue()) + 4; s.pocType = int(r.ue());
        if (s.pocType == 0) s.log2MaxPocLsb = int(r.ue()) + 4;
        else if (s.pocType == 1) {
            s.deltaPicOrderAlwaysZero = r.flag(); s.offsetForNonRefPic = r.se(); s.offsetForTopToBottom = r.se();
            const uint32_t n = r.ue(); for (uint32_t i = 0; i < n; ++i) s.offsetsForRefFrame.push_back(r.se());
        }
        s.maxNumRefFrames = int(r.ue()); s.gapsAllowed = r.flag();
        s.picWidthInMbsMinus1 = int(r.ue()); s.picHeightInMapUnitsMinus1 = int(r.ue());
        s.frameMbsOnly = r.flag(); if (!s.frameMbsOnly) s.mbAff = r.flag();
        s.direct8x8 = r.flag();
        s.cropping = r.flag(); if (s.cropping) for (auto &c : s.crop) c = r.ue();
        return s; // vui_parameters_present_flag and the VUI are not needed
    } catch (const std::exception &) { return std::nullopt; }
}

bool Sps::parseRelevantEquals(const Sps &o) const
{
    return profileIdc == o.profileIdc && chromaFormatIdc == o.chromaFormatIdc && separateColourPlane == o.separateColourPlane
        && bitDepthLumaMinus8 == o.bitDepthLumaMinus8 && bitDepthChromaMinus8 == o.bitDepthChromaMinus8 && qpprimeBypass == o.qpprimeBypass
        && scalingMatrixPresent == o.scalingMatrixPresent && scalingLists == o.scalingLists && log2MaxFrameNum == o.log2MaxFrameNum
        && pocType == o.pocType && log2MaxPocLsb == o.log2MaxPocLsb && deltaPicOrderAlwaysZero == o.deltaPicOrderAlwaysZero
        && offsetForNonRefPic == o.offsetForNonRefPic && offsetForTopToBottom == o.offsetForTopToBottom && offsetsForRefFrame == o.offsetsForRefFrame
        && picWidthInMbsMinus1 == o.picWidthInMbsMinus1 && picHeightInMapUnitsMinus1 == o.picHeightInMapUnitsMinus1 && frameMbsOnly == o.frameMbsOnly
        && mbAff == o.mbAff && direct8x8 == o.direct8x8 && cropping == o.cropping && std::equal(std::begin(crop), std::end(crop), std::begin(o.crop));
}

std::optional<Pps> parsePps(std::span<const uint8_t> rbsp)
{
    try {
        BitReader r(rbsp);
        Pps p;
        p.ppsId = int(r.ue()); p.spsId = int(r.ue()); p.cabac = r.flag(); p.bottomFieldPicOrderPresent = r.flag();
        p.numSliceGroupsMinus1 = int(r.ue());
        if (p.numSliceGroupsMinus1 > 0) return p; // slice groups: the rewriter refuses these anyway
        p.numRefIdxL0Minus1 = int(r.ue()); p.numRefIdxL1Minus1 = int(r.ue()); p.weightedPred = r.flag(); p.weightedBipredIdc = int(r.u(2));
        p.picInitQpMinus26 = r.se(); p.picInitQsMinus26 = r.se(); p.chromaQpIndexOffset = r.se();
        p.deblockingControlPresent = r.flag(); p.constrainedIntraPred = r.flag(); p.redundantPicCntPresent = r.flag();
        return p;
    } catch (const std::exception &) { return std::nullopt; }
}

std::optional<SliceHead> parseSliceHead(std::span<const uint8_t> rbsp, bool idrNal, const Sps &sps)
{
    try {
        BitReader r(rbsp);
        SliceHead h;
        h.firstMb = int(r.ue()); h.sliceType = int(r.ue()); h.ppsId = int(r.ue());
        if (sps.separateColourPlane) r.u(2);
        h.frameNum = int(r.u(sps.log2MaxFrameNum));
        if (!sps.frameMbsOnly) { if (r.flag()) r.flag(); }
        h.idr = idrNal;
        if (idrNal) h.idrPicId = int(r.ue());
        if (sps.pocType == 0) h.pocLsb = int(r.u(sps.log2MaxPocLsb));
        return h;
    } catch (const std::exception &) { return std::nullopt; }
}

bool MainStreamState::trackMainPacket(std::span<const uint8_t> annexB, int64_t pts)
{
    bool ok = true;
    packets++; serial++; lastTrackedPts = pts;
    for (const auto nal : splitAnnexB(annexB)) {
        if (nal.empty()) continue;
        const auto hdr = NalHeader::parse(nal[0]);
        const auto rbsp = ebspToRbsp(nal.subspan(1));
        if (hdr.type == 7) {
            if (auto s = parseSps(rbsp)) { sps = *s; spsRbsp = rbsp; } else ok = false;
        } else if (hdr.type == 8) {
            if (auto p = parsePps(rbsp)) pps = *p; else ok = false;
        } else if ((hdr.type == 1 || hdr.type == 5) && sps) {
            if (auto h = parseSliceHead(rbsp, hdr.type == 5, *sps)) {
                if (hdr.type == 5) haveIdr = true;
                // 7.4.3: PrevRefFrameNum is the frame_num of the previous REFERENCE picture. Every
                // main picture h264_vaapi emits with max_b_frames = 0 is one; guard anyway.
                if (hdr.refIdc != 0) { lastFrameNum = h->frameNum; lastPocLsb = h->pocLsb; }
            } else ok = false;
        }
    }
    return ok;
}

const char *Rewriter::statusName(Status s)
{
    switch (s) { case Status::Ok: return "ok"; case Status::NoMainSps: return "no-main-sps"; case Status::SpsMismatch: return "sps-mismatch"; case Status::Unsupported: return "unsupported"; case Status::ParseError: return "parse-error"; }
    return "?";
}

Rewriter::Result Rewriter::rewrite(std::span<const uint8_t> auxAnnexB, const MainStreamState &main)
{
    Result result;
    if (!main.sps || !main.haveIdr) { result.status = Status::NoMainSps; result.detail = QStringLiteral("no main SPS/IDR tracked yet"); return result; }
    std::vector<std::span<const uint8_t>> slices;
    try {
        for (const auto nal : splitAnnexB(auxAnnexB)) {
            if (nal.empty()) continue;
            const auto hdr = NalHeader::parse(nal[0]);
            switch (hdr.type) {
            case 7: {
                const auto rbsp = ebspToRbsp(nal.subspan(1));
                const auto s = parseSps(rbsp);
                if (!s) { result.status = Status::ParseError; result.detail = QStringLiteral("aux SPS"); return result; }
                m_auxSps = *s;
                m_auxSpsChanged = true; // re-check against the main SPS below
                break;
            }
            case 8: {
                const auto rbsp = ebspToRbsp(nal.subspan(1));
                const auto p = parsePps(rbsp);
                if (!p) { result.status = Status::ParseError; result.detail = QStringLiteral("aux PPS"); return result; }
                if (p->numSliceGroupsMinus1 > 0) { result.status = Status::Unsupported; result.detail = QStringLiteral("slice groups"); return result; }
                m_auxPps = *p;
                // Renumber: pps id 0 -> 1, everything after the first ue(v) bit-copied, trailing bits re-terminated.
                BitReader r(rbsp);
                r.ue();
                const size_t stop = BitReader::lastOneBit(rbsp);
                BitWriter w;
                w.ue(1);
                w.copyBits(r, stop - r.position());
                w.trailingBits();
                m_auxPpsNal.assign(1, nal[0]);
                const auto ebsp = rbspToEbsp(w.take());
                m_auxPpsNal.insert(m_auxPpsNal.end(), ebsp.begin(), ebsp.end());
                break;
            }
            case 5: case 1: slices.push_back(nal); break;
            case 6: case 9: case 12: break; // SEI, AUD, filler: dropped
            default: result.status = Status::Unsupported; result.detail = QStringLiteral("NAL type %1").arg(hdr.type); return result;
            }
        }
        if (!m_auxSps || !m_auxPps || m_auxPpsNal.empty()) { result.status = Status::ParseError; result.detail = QStringLiteral("aux SPS/PPS never seen"); return result; }
        if (m_auxPps->spsId != main.sps->spsId) { result.status = Status::Unsupported; result.detail = QStringLiteral("aux PPS refers to SPS %1, the main SPS is %2").arg(m_auxPps->spsId).arg(main.sps->spsId); return result; }
        if (m_auxSpsChanged || m_checkedMainSps != main.spsRbsp) {   // at stream start and on every SPS change on either side
            m_equal = m_auxSps->parseRelevantEquals(*main.sps);
            m_checkedMainSps = main.spsRbsp;
            m_auxSpsChanged = false;
        }
        if (!m_equal) { result.status = Status::SpsMismatch; result.detail = QStringLiteral("aux SPS differs from the main SPS in a parse-relevant field"); return result; }
        const Sps &sps = *main.sps;   // equal in every field the slice header depends on
        const Pps &pps = *m_auxPps;
        if (sps.pocType == 1 || sps.separateColourPlane || !sps.frameMbsOnly) { result.status = Status::Unsupported; result.detail = QStringLiteral("poc type 1 / colour planes / fields"); return result; }
        const int maxFrameNum = 1 << sps.log2MaxFrameNum, maxPocLsb = 1 << sps.log2MaxPocLsb;
        const int frameNum = (main.lastFrameNum + 1) % maxFrameNum, pocLsb = (main.lastPocLsb + 1) % maxPocLsb;

        result.annexB = startCode(); append(result.annexB, m_auxPpsNal);
        for (const auto nal : slices) {
            const auto nalHeader = NalHeader::parse(nal[0]);
            const bool idr = nalHeader.type == 5;
            const auto rbsp = ebspToRbsp(nal.subspan(1));
            BitReader r(rbsp);
            const uint32_t firstMb = r.ue(), sliceType = r.ue();
            r.ue();                                              // pps id (0)
            r.u(sps.log2MaxFrameNum);                            // frame_num
            if (idr) r.ue();                                     // idr_pic_id
            int32_t deltaBottom = 0; bool hasDeltaBottom = false;
            if (sps.pocType == 0) { r.u(sps.log2MaxPocLsb); if (pps.bottomFieldPicOrderPresent) { deltaBottom = r.se(); hasDeltaBottom = true; } }
            uint32_t redundant = 0; if (pps.redundantPicCntPresent) redundant = r.ue();
            if (sliceType % 5 != 2) { result.status = Status::Unsupported; result.detail = QStringLiteral("slice_type %1 from the intra context").arg(sliceType); return result; }
            // dec_ref_pic_marking() is present only when the original NAL is a reference picture (7.3.3)
            if (nalHeader.refIdc != 0) {
                if (idr) { r.u(2); }
                else if (r.flag()) { result.status = Status::Unsupported; result.detail = QStringLiteral("adaptive_ref_pic_marking on an aux slice"); return result; }
            }
            const int32_t sliceQpDelta = r.se();
            uint32_t disableDeblock = 0; int32_t alpha = 0, beta = 0;
            if (pps.deblockingControlPresent) { disableDeblock = r.ue(); if (disableDeblock != 1) { alpha = r.se(); beta = r.se(); } }

            BitWriter w;
            w.ue(firstMb); w.ue(sliceType); w.ue(1);
            w.u(uint32_t(frameNum), sps.log2MaxFrameNum);
            if (sps.pocType == 0) { w.u(uint32_t(pocLsb), sps.log2MaxPocLsb); if (hasDeltaBottom) w.se(deltaBottom); }
            if (pps.redundantPicCntPresent) w.ue(redundant);
            w.se(sliceQpDelta);
            if (pps.deblockingControlPresent) { w.ue(disableDeblock); if (disableDeblock != 1) { w.se(alpha); w.se(beta); } }
            if (pps.cabac) {
                r.skipAlignmentOnes();
                w.alignOnes();
                w.appendBytes(std::span<const uint8_t>(rbsp).subspan(r.bytePosition()));
            } else {
                const size_t stop = BitReader::lastOneBit(rbsp);
                w.copyBits(r, stop - r.position());
                w.trailingBits();
            }
            append(result.annexB, startCode());
            result.annexB.push_back(NalHeader::make(0, 1));
            append(result.annexB, rbspToEbsp(w.take()));
        }
        if (slices.empty()) { result.status = Status::ParseError; result.detail = QStringLiteral("aux AU without slices"); return result; }
        result.status = Status::Ok;
        return result;
    } catch (const std::exception &e) {
        result.status = Status::ParseError; result.detail = QString::fromLatin1(e.what()); return result;
    }
}
}
```

- [ ] **Step 6: `VaapiH264`**

`src/vaapih264.cpp` (mirrors `H264VAAPIEncoder::createCodecContext()`/`buildEncodingOptions()` at `h264vaapiencoder.cpp:163-220, 261-274` and the upload graph K5 needs; K5 switches the encoder over so this stays the single source of truth):

```cpp
#include "vaapih264_p.h"

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

// FFmpeg 8: AV_PROFILE_H264_* (the FF_PROFILE_* names are gone; h264vaapiencoder.cpp already uses the new ones).
namespace VaapiH264
{
namespace
{
QString errorString(int err) { char buf[AV_ERROR_MAX_STRING_SIZE]; return QString::fromLatin1(av_make_error_string(buf, sizeof buf, err)); }
}

AVDictionary *encodingOptions()
{
    AVDictionary *options = nullptr;
    av_dict_set_int(&options, "async_depth", 1, 0); // emit each picture as soon as it is encoded (see H264VAAPIEncoder)
    av_dict_set(&options, "rc_mode", "CQP", 0);     // constant QP from global_quality
    return options;
}

AVBufferRef *createDevice(const QByteArray &renderNode, QString *error)
{
    AVBufferRef *device = nullptr;
    if (const int err = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, renderNode.constData(), nullptr, 0); err < 0) {
        if (error) *error = QStringLiteral("av_hwdevice_ctx_create(vaapi, %1): %2").arg(QString::fromLatin1(renderNode), errorString(err));
        return nullptr;
    }
    return device;
}

AVCodecContext *openContext(const Params &p, AVBufferRef *hwFramesCtx, QString *error)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) { if (error) *error = QStringLiteral("h264_vaapi not found"); return nullptr; }
    AVCodecContext *context = avcodec_alloc_context3(codec);
    context->width = p.size.width();
    context->height = p.size.height();
    context->max_b_frames = 0;
    context->gop_size = p.gopSize;
    context->pix_fmt = AV_PIX_FMT_VAAPI;
    context->time_base = p.timeBase;
    context->global_quality = p.qp;
    switch (p.profile) {
    case Profile::Baseline: context->profile = AV_PROFILE_H264_CONSTRAINED_BASELINE; break;
    case Profile::Main: context->profile = AV_PROFILE_H264_MAIN; break;
    case Profile::High: context->profile = AV_PROFILE_H264_HIGH; break;
    }
    context->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
    AVDictionary *options = encodingOptions();
    const int err = avcodec_open2(context, codec, &options);
    av_dict_free(&options);
    if (err < 0) { if (error) *error = QStringLiteral("avcodec_open2(h264_vaapi): %1").arg(errorString(err)); avcodec_free_context(&context); return nullptr; }
    return context;
}

UploadGraph::~UploadGraph() { if (graph) avfilter_graph_free(&graph); }

bool createUploadGraph(UploadGraph &g, AVBufferRef *vaapiDevice, const QSize &size, AVRational timeBase, QString *error)
{
    g.graph = avfilter_graph_alloc();
    const QByteArray args = QStringLiteral("video_size=%1x%2:pix_fmt=yuv420p:time_base=%3/%4").arg(size.width()).arg(size.height()).arg(timeBase.num).arg(timeBase.den).toLatin1();
    int ret = avfilter_graph_create_filter(&g.in, avfilter_get_by_name("buffer"), "in", args.constData(), nullptr, g.graph);
    if (ret < 0) { if (error) *error = QStringLiteral("buffer: %1").arg(errorString(ret)); return false; }
    ret = avfilter_graph_create_filter(&g.out, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, g.graph);
    if (ret < 0) { if (error) *error = QStringLiteral("buffersink: %1").arg(errorString(ret)); return false; }
    AVFilterInOut *inputs = avfilter_inout_alloc(), *outputs = avfilter_inout_alloc();
    inputs->name = av_strdup("in"); inputs->filter_ctx = g.in; inputs->pad_idx = 0; inputs->next = nullptr;
    outputs->name = av_strdup("out"); outputs->filter_ctx = g.out; outputs->pad_idx = 0; outputs->next = nullptr;
    // format=nv12 interleaves the planar chroma on the CPU (swscale, well under a millisecond at
    // 1440p); hwupload places the picture in a VAAPI surface of the frames context the encoder
    // is opened on - the canonical software -> h264_vaapi path.
    ret = avfilter_graph_parse(g.graph, "format=nv12,hwupload", outputs, inputs, nullptr);
    if (ret < 0) { if (error) *error = QStringLiteral("format=nv12,hwupload: %1").arg(errorString(ret)); return false; }
    for (unsigned i = 0; i < g.graph->nb_filters; ++i) g.graph->filters[i]->hw_device_ctx = av_buffer_ref(vaapiDevice);
    ret = avfilter_graph_config(g.graph, nullptr);
    if (ret < 0) { if (error) *error = QStringLiteral("avfilter_graph_config: %1").arg(errorString(ret)); return false; }
    return true;
}
}
```

`src/CMakeLists.txt`: add `avc444nalrewriter.cpp vaapih264.cpp` to the `KPipeWireRecord` sources (they are compiled into the library now so K5 can use them; nothing uses them yet).

- [ ] **Step 7: Run the pure tests, then the GPU go/no-go**

Run: `cmake --build ~/dev/kpipewire/build-tests -j16 2>&1 | grep -E "warning|error"; ctest --test-dir ~/dev/kpipewire/build-tests -R "avc444nalrewritertest" --output-on-failure | tail -4`
Expected: PASS, zero warnings (the CAVLC row of `rewritesAnIdrIntoANonReferencePicture` catches a wrong `lastOneBit`/`copyBits`; the emulation case catches an EBSP slip).

Run: `LIBVA_DRIVER_NAME=radeonsi ctest --test-dir ~/dev/kpipewire/build-tests -R avc444streamtest --output-on-failure -V 2>&1 | grep -E "main I|aux PSNR|main SPS|PASS|FAIL|SKIP|QWARN|differs|order"`
Expected on hal9000: `main I ~<size> B, main P avg <≪ 10 %> B`, `main SPS: poc type 2 log2_max_frame_num <n> cabac 1`, `aux PSNR min Y ≥ 45 …`, PASS. **This is the go/no-go for K4–K6.** Three independent checks fail on a wrong rewrite: the conformance walk names the AU and the rule (`frame_num N, expected M` → the frame_num rule; `POC … not above` → POC ordering; `two consecutive non-reference pictures` → the interleave shape); the decoder deny-list catches what libavcodec would otherwise conceal (`Frame num gap` is DEBUG-level and concealed with a pixel copy, `no picture ooo` DEBUG = dropped as out of order, `concealing`/`reorder buffer` INFO — the callback sees every level); and `non-existing PPS 1 referenced` / `issing reference` mean the PPS' is not reaching the decoder before the slice / the picture is being treated as a reference (check `nal_ref_idc = 0` and that the `dec_ref_pic_marking` bits were removed). At QP 12 an aux PSNR below 45 dB on this content is a rewriter fault (garbage decodes at ~10 dB), not a quantiser question. Paste the three `qInfo` lines into the commit message.

- [ ] **Step 8: Commit and export patch 0014**

```bash
cd ~/dev/kpipewire && git add src/h264bitstream_p.h src/avc444nalrewriter_p.h src/avc444nalrewriter.cpp src/vaapih264_p.h src/vaapih264.cpp src/CMakeLists.txt src/autotests/CMakeLists.txt src/autotests/avc444nalrewritertest.cpp src/autotests/avc444streamtest.cpp
git commit -m "avc444: NAL rewriter turning intra aux pictures into non-reference pictures of the main stream; GPU round-trip test (KRDP OPT-045)

Two h264_vaapi contexts (main as today, aux gop 1); each aux access unit is
rewritten under the main SPS (PPS id 1 inline, nal_ref_idc 0, type 1,
frame_num = main + 1, no idr_pic_id / marking). Decoded with libavcodec's
software h264 on hal9000: <main I> B / P avg <..> B, aux PSNR min Y/U/V
<..>/<..>/<..> dB, no decoder warnings, main pictures identical to main-only.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git format-patch -1 HEAD --start-number 14 -o ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/
```

---

### Task K4: Public API, `Packet` aux, produce plumbing, `Avc444PacketPairer` (pts across two encoders) and `PlaneLayout`

**Files:**
- Create: `src/avc444pairer_p.h`, `src/avc444planes_p.h`, `src/autotests/avc444pairertest.cpp`, `src/autotests/avc444planestest.cpp`
- Modify: `src/pipewirebaseencodedstream.h:150-165` (after `ColorRange`), `src/pipewirebaseencodedstream.cpp:25-39` (private struct), `:153-202` (`start()`), `:307-313` (after `setColorRange`); `src/pipewireencodedstream.h:24-40`; `src/pipewireencodedstream_p.h`; `src/pipewireencodedstream.cpp:88-133`; `src/pipewireproduce_p.h:75-105, 107-160`; `src/pipewireproduce.cpp:173-227` (the two worker loops in `setupStream()`), `:280-341` (setters), `:481-527` (`makeEncoder`; the H264 case is `:491-500`); `src/encoder_p.h` (one no-op virtual `auxStreamEnabledChanged(bool)`); `src/autotests/CMakeLists.txt`

**Interfaces (produces; K5 and the krdp plan's S2/S3/S4 consume exactly these names):**

```cpp
// pipewirebaseencodedstream.h
class KPIPEWIRE_EXPORT PipeWireBaseEncodedStream : public QObject {
    ...
    enum class ChromaMode { Yuv420, Yuv444v1, Yuv444v2 };
    Q_ENUM(ChromaMode)
    /** Requested chroma mode; applied when the stream starts (a running stream keeps its mode until
     *  the next start(), and logs a warning). Only the H264* encoders honour 4:4:4; they need
     *  h264_vaapi. Default Yuv420. KPIPEWIRE_CHROMA_MODE=420|444v1|444v2 overrides it. */
    void setChromaMode(ChromaMode mode);
    ChromaMode chromaMode() const;
    /** The mode the running encoder actually uses: Yuv420 when 4:4:4 was requested but h264_vaapi is
     *  unavailable, or after the aux stream had to be abandoned (SPS mismatch, see the rewriter) - the
     *  latter can happen at any time after state() == Recording, so watch activeChromaModeChanged(). */
    ChromaMode activeChromaMode() const;
Q_SIGNALS:
    /** activeChromaMode() changed: once at start (the encoder chosen) and on a mid-session fallback to Yuv420. */
    void activeChromaModeChanged(PipeWireBaseEncodedStream::ChromaMode mode);
public:
    /** In a 4:4:4 mode: whether frames may carry the auxiliary chroma picture (default true). Off
     *  suppresses both the per-frame aux and the at-rest refresh. Thread-safe; a no-op in Yuv420. */
    void setAuxStreamEnabled(bool enabled);
    bool auxStreamEnabled() const;
};

// pipewireencodedstream.h
class KPIPEWIRE_EXPORT PipeWireEncodedStream : public PipeWireBaseEncodedStream {
    class Packet {
    public:
        Packet(bool isKey, const QByteArray &data);                                              // unchanged
        Packet(bool isKey, const QByteArray &data, const QByteArray &aux, bool auxIsKey);       // new
        bool isKeyFrame() const;
        /// The main (luma) picture; EMPTY for an at-rest chroma refresh, which carries only aux().
        QByteArray data() const;
        /// The auxiliary chroma picture of the same frame (AVC444); empty for a luma-only frame and in Yuv420.
        QByteArray aux() const;
        /// True for every aux picture (they are all intra); informational.
        bool auxIsKey() const;
    };
    struct ChromaTiming {   // one report per second while a 4:4:4 encoder runs; microseconds
        int frames = 0;              // frames captured in the interval
        int auxSent = 0;             // frames that carried an aux picture
        int auxSkippedMotion = 0;    // frames that went luma-only because of the motion gap
        int auxRestRefresh = 0;      // aux-only refreshes emitted
        int rewriteFailures = 0;
        QByteArray splitVariant;     // "avx512" ...
        qint64 downloadMin = 0, downloadAvg = 0, downloadMax = 0;
        qint64 splitMin = 0, splitAvg = 0, splitMax = 0;
        qint64 uploadMin = 0, uploadAvg = 0, uploadMax = 0;           // buffersink pull incl. format+hwupload, per picture (both contexts)
        qint64 encodeMainMin = 0, encodeMainAvg = 0, encodeMainMax = 0; // main picture: queued on the produce thread -> packet on the output thread
        qint64 encodeAuxMin = 0, encodeAuxAvg = 0, encodeAuxMax = 0;    // aux picture, same measure
    };
Q_SIGNALS:
    void chromaTimingReported(const PipeWireEncodedStream::ChromaTiming &timing);
};
Q_DECLARE_METATYPE(PipeWireEncodedStream::ChromaTiming)

// pipewireproduce_p.h (additions)
class PipeWireProduce : public QObject {
public:
    /** One frame's output. aux is null for a luma-only frame; main is null for an at-rest chroma refresh.
     *  Default: processPacket(main) when main is set, nothing otherwise. */
    virtual void processPacketPair(AVPacket *main, AVPacket *aux);
    virtual void reportChromaTiming(const PipeWireEncodedStream::ChromaTiming &) {}
    void setChromaMode(PipeWireBaseEncodedStream::ChromaMode mode);    // before setupStream() only
    void setAuxStreamEnabled(bool enabled);                              // any time, produce thread; tells the encoder (auxStreamEnabledChanged)
    /** Wake the passthrough / output worker. Counting wake-ups (not bare notifies) so a wake that lands
     *  while the worker is busy is not lost - the at-rest refresh has no next frame behind it to retry. */
    void wakePassthrough();
    void wakeOutput();
    PipeWireBaseEncodedStream::ChromaMode m_chromaMode = PipeWireBaseEncodedStream::ChromaMode::Yuv420;
    std::atomic<PipeWireBaseEncodedStream::ChromaMode> m_activeChromaMode = PipeWireBaseEncodedStream::ChromaMode::Yuv420;
    std::atomic_bool m_auxStreamEnabled = true;
    std::atomic_int m_passthroughWork = 0, m_outputWork = 0;
Q_SIGNALS:
    void activeChromaModeChanged(PipeWireBaseEncodedStream::ChromaMode mode);   // any thread -> the stream (queued)
};

// encoder_p.h (K4 adds only this; K5 fills the rest in)
class Encoder : public QObject {
    /** The produce's aux-stream switch moved (produce thread). Default no-op; the 444 encoder re-arms its at-rest refresh. */
    virtual void auxStreamEnabledChanged(bool enabled) { Q_UNUSED(enabled); }
};

// avc444pairer_p.h (header-only, pure) — two encoders, packets arrive by pts, in unknown relative order
class Avc444PacketPairer {
public:
    struct Emitted { int64_t pts; bool hasMain; bool hasAux; };   // in emission order
    struct Result { std::vector<Emitted> emitted; int droppedAux = 0; };
    // Produce thread, right after the pictures were queued:
    void expect(int64_t pts, bool auxExpected);   // a main picture (with or without its aux)
    void expectAuxOnly(int64_t pts);              // an at-rest refresh: aux for a pts whose main already went out
    // Passthrough thread: the aux picture for pts was pulled from the filter graph but never reached the
    // encoder (encode-queue gate or a failed avcodec_send_frame). A main held for it is released at once.
    Result auxLost(int64_t pts);
    // Output thread, one call per packet received from the respective context:
    Result mainReceived(int64_t pts);
    Result auxReceived(int64_t pts);
    int pendingMains() const;                     // 0 or 1: a main waiting for its aux
    std::optional<int64_t> pendingMainPts() const;
private:
    enum class Kind { MainOnly, Pair, AuxOnly };
    mutable std::mutex m_mutex;
    std::deque<std::pair<int64_t, Kind>> m_expected;
    std::optional<int64_t> m_pendingMain;   // main received, aux outstanding
    std::optional<int64_t> m_pendingAux;    // aux received before its main
};

// avc444planes_p.h (header-only, pure) — one pooled picture buffer
struct PlaneLayout {
    int width, height;                             // the picture (what the encoder codes)
    int bufferRows;                                // roundUp16(height): the v1 aux plane needs the rows, the AVFrame only uses `height`
    int strideY, strideUV;                         // 64-aligned
    size_t offsetY = 0, offsetU, offsetV, bytes;
    static PlaneLayout forSize(int width, int height);
    Avc444Split::I420 planes(uint8_t *base) const;
};
```

- [ ] **Step 1: Write the failing pure tests**

`src/autotests/avc444pairertest.cpp`:

```cpp
#include "avc444pairer_p.h"
#include <QTest>

class Avc444PairerTest : public QObject
{
    Q_OBJECT
private:
    static QList<QString> names(const Avc444PacketPairer::Result &r)
    {
        QList<QString> out;
        for (const auto &e : r.emitted) out << QStringLiteral("%1:%2%3").arg(e.pts).arg(e.hasMain ? "M" : "").arg(e.hasAux ? "A" : "");
        return out;
    }
private Q_SLOTS:
    void pairWaitsForBoth()
    {
        Avc444PacketPairer p;
        p.expect(10, true);
        QVERIFY(p.mainReceived(10).emitted.empty()); QCOMPARE(p.pendingMains(), 1);
        QCOMPARE(names(p.auxReceived(10)), (QList<QString>{QStringLiteral("10:MA")})); QCOMPARE(p.pendingMains(), 0);
    }
    void auxMayArriveFirst()
    {
        Avc444PacketPairer p;
        p.expect(10, true);
        QVERIFY(p.auxReceived(10).emitted.empty());
        QCOMPARE(names(p.mainReceived(10)), (QList<QString>{QStringLiteral("10:MA")}));
    }
    void lumaOnlyGoesStraightOut()
    {
        Avc444PacketPairer p;
        p.expect(10, false);
        QCOMPARE(names(p.mainReceived(10)), (QList<QString>{QStringLiteral("10:M")}));
    }
    void mixedSequenceStaysInMainOrder()
    {
        Avc444PacketPairer p;
        p.expect(10, true); p.expect(26, false); p.expect(42, true);
        QList<QString> order;
        order += names(p.mainReceived(10)); order += names(p.auxReceived(10)); order += names(p.mainReceived(26));
        order += names(p.auxReceived(42)); order += names(p.mainReceived(42));
        QCOMPARE(order, (QList<QString>{QStringLiteral("10:MA"), QStringLiteral("26:M"), QStringLiteral("42:MA")}));
    }
    void missingAuxIsReleasedWhenTheNextMainArrives()
    {
        Avc444PacketPairer p;
        p.expect(10, true); p.expect(26, true);
        QVERIFY(p.mainReceived(10).emitted.empty());
        QCOMPARE(names(p.mainReceived(26)), (QList<QString>{QStringLiteral("10:M")}));   // 10 goes luma-only, 26 waits
        const auto r = p.auxReceived(10);                                                 // the straggler
        QVERIFY(r.emitted.empty()); QCOMPARE(r.droppedAux, 1);
        QCOMPARE(names(p.auxReceived(26)), (QList<QString>{QStringLiteral("26:MA")}));
    }
    void restRefreshIsAnAuxOnlyEmission()
    {
        Avc444PacketPairer p;
        p.expect(10, false);
        QCOMPARE(names(p.mainReceived(10)), (QList<QString>{QStringLiteral("10:M")}));
        p.expectAuxOnly(10);
        QCOMPARE(names(p.auxReceived(10)), (QList<QString>{QStringLiteral("10:A")}));
        QCOMPARE(p.auxReceived(10).droppedAux, 1);                                        // a second one is not expected
    }
    void strayAuxIsDropped()
    {
        Avc444PacketPairer p;
        QCOMPARE(p.auxReceived(21).droppedAux, 1);
        p.expect(30, false);
        QCOMPARE(names(p.mainReceived(30)), (QList<QString>{QStringLiteral("30:M")}));
        QCOMPARE(p.auxReceived(30).droppedAux, 1);                                        // its frame already went out luma-only
    }
    void unexpectedMainIsEmittedAlone()
    {
        Avc444PacketPairer p;   // nothing announced (a picture that slipped through a reopen)
        QCOMPARE(names(p.mainReceived(5)), (QList<QString>{QStringLiteral("5:M")}));
    }
    void auxLostReleasesTheHeldMainAtOnce()
    {
        Avc444PacketPairer p;
        p.expect(10, true);
        QVERIFY(p.mainReceived(10).emitted.empty());
        QCOMPARE(p.pendingMainPts(), std::optional<int64_t>(10));
        QCOMPARE(names(p.auxLost(10)), (QList<QString>{QStringLiteral("10:M")}));   // no next main needed
        QCOMPARE(p.pendingMains(), 0);
        QCOMPARE(p.auxReceived(10).droppedAux, 1);                                   // if it shows up after all
    }
    void auxLostBeforeTheMainMakesTheFrameLumaOnly()
    {
        Avc444PacketPairer p;
        p.expect(10, true);
        QVERIFY(p.auxLost(10).emitted.empty());
        QCOMPARE(names(p.mainReceived(10)), (QList<QString>{QStringLiteral("10:M")}));
        p.expectAuxOnly(10);
        QVERIFY(p.auxLost(10).emitted.empty());                                      // a lost refresh is simply forgotten
        QCOMPARE(p.auxReceived(10).droppedAux, 1);
    }
    void sameFrameReFeedAfterARefresh()
    {
        // requestKeyFrame() re-feeds m_lastFrame with the SAME pts (pipewireproduce.cpp:299-336 bypasses
        // processFrame's pts checks): a refresh for pts 10 may be followed by a new pair for pts 10.
        Avc444PacketPairer p;
        p.expect(10, false);
        QCOMPARE(names(p.mainReceived(10)), (QList<QString>{QStringLiteral("10:M")}));
        p.expectAuxOnly(10);
        p.expect(10, true);
        QCOMPARE(names(p.auxReceived(10)), (QList<QString>{QStringLiteral("10:A")}));
        QVERIFY(p.mainReceived(10).emitted.empty());
        QCOMPARE(names(p.auxReceived(10)), (QList<QString>{QStringLiteral("10:MA")}));
    }
    void heldAuxForADiscardedMainIsDroppedByTheNextMain()
    {
        Avc444PacketPairer p;
        p.expect(10, true); p.expect(26, false);
        QVERIFY(p.auxReceived(10).emitted.empty());                                       // main 10 was discarded by the encode-queue gate
        const auto r = p.mainReceived(26);
        QCOMPARE(names(r), (QList<QString>{QStringLiteral("26:M")})); QCOMPARE(r.droppedAux, 1);
    }
};
QTEST_GUILESS_MAIN(Avc444PairerTest)
#include "avc444pairertest.moc"
```

`src/autotests/avc444planestest.cpp`:

```cpp
#include "avc444planes_p.h"
#include <QTest>
#include <vector>

class Avc444PlanesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sizesAndOffsets()
    {
        const auto l = PlaneLayout::forSize(2560, 1440);
        QCOMPARE(l.height, 1440); QCOMPARE(l.bufferRows, 1440); QCOMPARE(l.strideY, 2560); QCOMPARE(l.strideUV, 1280);
        QCOMPARE(l.offsetU, size_t(2560) * 1440); QCOMPARE(l.offsetV, l.offsetU + size_t(1280) * 720);
        QCOMPARE(l.bytes, l.offsetV + size_t(1280) * 720);
        const auto m = PlaneLayout::forSize(1920, 1080);
        QCOMPARE(m.height, 1080); QCOMPARE(m.bufferRows, 1088); QCOMPARE(m.strideY, 1920); QCOMPARE(m.strideUV, 960);
        QCOMPARE(m.offsetU, size_t(1920) * 1088); QCOMPARE(m.bytes, m.offsetU + 2 * size_t(960) * 544);
        const auto o = PlaneLayout::forSize(1367, 769);
        QCOMPARE(o.strideY, 1408); QCOMPARE(o.strideUV, 704); QCOMPARE(o.bufferRows, 784);
        QVERIFY(Avc444Split::auxHeight(Avc444Split::Version::V1, 769) <= o.bufferRows);
        std::vector<uint8_t> buf(o.bytes, 0);
        const auto planes = o.planes(buf.data());
        QCOMPARE(planes.y.stride, o.strideY); QCOMPARE(planes.u.data, buf.data() + o.offsetU); QCOMPARE(planes.v.stride, o.strideUV);
    }
};
QTEST_GUILESS_MAIN(Avc444PlanesTest)
#include "avc444planestest.moc"
```

`src/autotests/CMakeLists.txt`:

```cmake
ecm_add_test(avc444pairertest.cpp TEST_NAME avc444pairertest LINK_LIBRARIES Qt::Test Qt::Core)
target_include_directories(avc444pairertest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
ecm_add_test(avc444planestest.cpp TEST_NAME avc444planestest LINK_LIBRARIES Qt::Test Qt::Core)
target_include_directories(avc444planestest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build ~/dev/kpipewire/build-tests -j16 --target avc444pairertest avc444planestest 2>&1 | tail -3`
Expected: FAIL — missing headers.

- [ ] **Step 3: Implement the two pure headers**

`src/avc444pairer_p.h`:

```cpp
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

class Avc444PacketPairer
{
public:
    struct Emitted { int64_t pts; bool hasMain; bool hasAux; };
    struct Result { std::vector<Emitted> emitted; int droppedAux = 0; };

    void expect(int64_t pts, bool auxExpected)
    {
        std::lock_guard lock(m_mutex);
        m_expected.emplace_back(pts, auxExpected ? Kind::Pair : Kind::MainOnly);
    }
    void expectAuxOnly(int64_t pts)
    {
        std::lock_guard lock(m_mutex);
        m_expected.emplace_back(pts, Kind::AuxOnly);
    }

    Result mainReceived(int64_t pts)
    {
        std::lock_guard lock(m_mutex);
        Result result;
        // A main whose aux never came goes out luma-only, in order, before this one.
        if (m_pendingMain) { result.emitted.push_back({*m_pendingMain, true, false}); m_pendingMain.reset(); }
        // An aux held for an older frame whose main never came (discarded by the encode-queue gate).
        if (m_pendingAux && *m_pendingAux < pts) { m_pendingAux.reset(); result.droppedAux++; }
        Kind kind = Kind::MainOnly;
        // Announcements older than this main belong to pictures that never reached the encoder.
        while (!m_expected.empty() && m_expected.front().first < pts) m_expected.pop_front();
        if (!m_expected.empty() && m_expected.front().first == pts && m_expected.front().second != Kind::AuxOnly) {
            kind = m_expected.front().second;
            m_expected.pop_front();
        }
        if (kind == Kind::Pair) {
            if (m_pendingAux && *m_pendingAux == pts) { m_pendingAux.reset(); result.emitted.push_back({pts, true, true}); }
            else m_pendingMain = pts;
        } else {
            result.emitted.push_back({pts, true, false});
        }
        return result;
    }

    Result auxReceived(int64_t pts)
    {
        std::lock_guard lock(m_mutex);
        Result result;
        if (m_pendingMain && *m_pendingMain == pts) {                       // the usual case: main first, then its aux
            m_pendingMain.reset();
            result.emitted.push_back({pts, true, true});
            return result;
        }
        if (!m_expected.empty() && m_expected.front().first == pts) {
            if (m_expected.front().second == Kind::AuxOnly) {              // at-rest refresh
                m_expected.pop_front();
                result.emitted.push_back({pts, false, true});
                return result;
            }
            if (m_expected.front().second == Kind::Pair) {                 // aux before its main: hold it
                if (m_pendingAux) result.droppedAux++;
                m_pendingAux = pts;
                return result;
            }
        }
        // An aux-only expectation queued behind a still-pending pair for the same pts cannot happen
        // (a refresh is only queued after the main went out), so anything else is a straggler.
        result.droppedAux++;
        return result;
    }

    Result auxLost(int64_t pts)
    {
        std::lock_guard lock(m_mutex);
        Result result;
        if (m_pendingMain && *m_pendingMain == pts) {                       // its main is waiting: out it goes, luma-only
            m_pendingMain.reset();
            result.emitted.push_back({pts, true, false});
            return result;
        }
        for (auto &[expectedPts, kind] : m_expected) {                      // main not received yet: the frame becomes luma-only
            if (expectedPts == pts && kind == Kind::Pair) { kind = Kind::MainOnly; return result; }
        }
        std::erase_if(m_expected, [pts](const auto &e) { return e.first == pts && e.second == Kind::AuxOnly; }); // a lost refresh is forgotten
        return result;
    }

    int pendingMains() const
    {
        std::lock_guard lock(m_mutex);
        return m_pendingMain ? 1 : 0;
    }
    std::optional<int64_t> pendingMainPts() const
    {
        std::lock_guard lock(m_mutex);
        return m_pendingMain;
    }

private:
    enum class Kind { MainOnly, Pair, AuxOnly };
    mutable std::mutex m_mutex;
    std::deque<std::pair<int64_t, Kind>> m_expected;
    std::optional<int64_t> m_pendingMain;
    std::optional<int64_t> m_pendingAux;
};
```

`src/avc444planes_p.h`:

```cpp
#pragma once
#include "avc444split_p.h"
#include <cstddef>

struct PlaneLayout {
    int width = 0, height = 0, bufferRows = 0;
    int strideY = 0, strideUV = 0;
    size_t offsetY = 0, offsetU = 0, offsetV = 0, bytes = 0;

    static constexpr int roundUp(int v, int a) { return (v + a - 1) / a * a; }
    static PlaneLayout forSize(int width, int height)
    {
        PlaneLayout l;
        l.width = width; l.height = height; l.bufferRows = Avc444Split::roundUp16(height);
        l.strideY = roundUp(width, 64); l.strideUV = roundUp((width + 1) / 2, 64);
        l.offsetU = size_t(l.strideY) * l.bufferRows;
        l.offsetV = l.offsetU + size_t(l.strideUV) * (l.bufferRows / 2);
        l.bytes = l.offsetV + size_t(l.strideUV) * (l.bufferRows / 2);
        return l;
    }
    Avc444Split::I420 planes(uint8_t *base) const
    {
        return {{base + offsetY, strideY}, {base + offsetU, strideUV}, {base + offsetV, strideUV}};
    }
};
```

- [ ] **Step 4: Run the pure tests to verify they pass**

Run: `cmake --build ~/dev/kpipewire/build-tests -j16 --target avc444pairertest avc444planestest 2>&1 | grep -E "warning|error"; ctest --test-dir ~/dev/kpipewire/build-tests -R "avc444(pairer|planes)test" --output-on-failure | tail -5`
Expected: both PASS, zero warnings.

- [ ] **Step 5: Public API and produce plumbing**

`src/pipewirebaseencodedstream.h`: add the `ChromaMode` enum + four methods after `setColorRange` (line 156), per the interface block above.

`src/pipewirebaseencodedstream.cpp`: in `PipeWireEncodedStreamPrivate` add `PipeWireBaseEncodedStream::ChromaMode m_chromaMode = ChromaMode::Yuv420; bool m_auxStreamEnabled = true;`. In `start()` after `d->m_produce->setColorRange(d->m_colorRange);` (line 174):

```cpp
    auto chromaMode = d->m_chromaMode;
    const QByteArray forcedChroma = qgetenv("KPIPEWIRE_CHROMA_MODE");
    if (forcedChroma == "420") chromaMode = ChromaMode::Yuv420;
    else if (forcedChroma == "444v1") chromaMode = ChromaMode::Yuv444v1;
    else if (forcedChroma == "444v2") chromaMode = ChromaMode::Yuv444v2;
    else if (!forcedChroma.isEmpty()) qCWarning(PIPEWIRERECORD_LOGGING) << "Unknown KPIPEWIRE_CHROMA_MODE" << forcedChroma << "(420|444v1|444v2)";
    if (chromaMode != d->m_chromaMode) qCWarning(PIPEWIRERECORD_LOGGING) << "KPIPEWIRE_CHROMA_MODE overrides the requested chroma mode:" << int(chromaMode);
    d->m_produce->setChromaMode(chromaMode);
    d->m_produce->setAuxStreamEnabled(d->m_auxStreamEnabled);
    connect(d->m_produce.get(), &PipeWireProduce::activeChromaModeChanged, this, &PipeWireBaseEncodedStream::activeChromaModeChanged, Qt::QueuedConnection);
```

and the new methods (`setAuxStreamEnabled` hops to the stream's thread first when `QThread::currentThread() != thread()`, exactly as `requestKeyFrame()` does at `:229-242`, because `d->m_produce` is created and reset on that thread):

```cpp
void PipeWireBaseEncodedStream::setChromaMode(ChromaMode mode)
{
    if (d->m_chromaMode == mode) return;
    d->m_chromaMode = mode;
    if (d->m_produce) qCWarning(PIPEWIRERECORD_LOGGING) << "Chroma mode changes apply on the next start(); the running stream keeps its mode";
}
PipeWireBaseEncodedStream::ChromaMode PipeWireBaseEncodedStream::chromaMode() const { return d->m_chromaMode; }
PipeWireBaseEncodedStream::ChromaMode PipeWireBaseEncodedStream::activeChromaMode() const
{
    return d->m_produce ? d->m_produce->m_activeChromaMode.load() : ChromaMode::Yuv420;
}
void PipeWireBaseEncodedStream::setAuxStreamEnabled(bool enabled)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, enabled]() { setAuxStreamEnabled(enabled); }, Qt::QueuedConnection);
        return;
    }
    d->m_auxStreamEnabled = enabled;
    if (d->m_produce) {
        QMetaObject::invokeMethod(d->m_produce.get(), [produce = d->m_produce.get(), enabled]() { produce->setAuxStreamEnabled(enabled); }, Qt::QueuedConnection);
    }
}
bool PipeWireBaseEncodedStream::auxStreamEnabled() const { return d->m_auxStreamEnabled; }
```

`src/pipewireproduce_p.h`: `#include "pipewireencodedstream.h"` (for `ChromaTiming`; no include cycle: it only includes `pipewirebaseencodedstream.h`), the three members and the four methods from the interface block. `pipewireproduce.cpp`:

```cpp
void PipeWireProduce::processPacketPair(AVPacket *main, AVPacket *aux)
{
    Q_UNUSED(aux); // PipeWireRecord and any other single-packet consumer: an aux-only refresh is nothing to them
    if (main) processPacket(main);
}
void PipeWireProduce::setChromaMode(PipeWireBaseEncodedStream::ChromaMode mode) { m_chromaMode = mode; }
void PipeWireProduce::setAuxStreamEnabled(bool enabled)
{
    m_auxStreamEnabled = enabled;
    if (m_encoder) m_encoder->auxStreamEnabledChanged(enabled); // produce thread: the 444 encoder re-arms its at-rest refresh
}
void PipeWireProduce::wakePassthrough() { m_passthroughWork++; m_passthroughCondition.notify_all(); }
void PipeWireProduce::wakeOutput() { m_outputWork++; m_outputCondition.notify_all(); }
```

Counted wake-ups (the two worker loops in `setupStream()`, `pipewireproduce.cpp:173-220`): today both loops `wait(lock)` with no predicate, so a `notify_all()` that lands while the worker is inside `encodeFrame()`/`receivePacket()` is lost. For a frame that only costs a one-frame delay; for the at-rest chroma refresh (K5), which has no next frame behind it, it means the refresh silently never goes out. Replace every `m_passthroughCondition.notify_all()` in `pipewireproduce.cpp` (`processFrame`, the frame-repeat timer, `requestKeyFrame`, `stateChanged`, `handleEncodedFramesChanged`, `destroy`) with `wakePassthrough()`, every `m_outputCondition.notify_all()` (the passthrough loop, `destroy`) with `wakeOutput()`, and make the loops wait on the counters:

```cpp
    m_passthroughThread = std::thread([this]() {
        m_passthroughRunning = true;
        while (m_passthroughRunning) {
            {
                std::unique_lock<std::mutex> lock(m_passthroughMutex);
                m_passthroughCondition.wait(lock, [this]() { return !m_passthroughRunning || m_passthroughWork.load() > 0; });
            }
            if (!m_passthroughRunning) {
                break;
            }
            m_passthroughWork = 0; // wakes that arrive from here on are seen on the next iteration

            auto [filtered, queued] = m_encoder->encodeFrame(m_maxPendingFrames - m_pendingEncodeFrames);
            m_pendingFilterFrames -= filtered;
            m_pendingEncodeFrames += queued;

            wakeOutput();
        }
    });
    ...
    m_outputThread = std::thread([this]() {
        m_outputRunning = true;
        while (m_outputRunning) {
            {
                std::unique_lock<std::mutex> lock(m_outputMutex);
                m_outputCondition.wait(lock, [this]() { return !m_outputRunning || m_outputWork.load() > 0; });
            }
            if (!m_outputRunning) {
                break;
            }
            m_outputWork = 0;

            auto received = m_encoder->receivePacket();
            m_pendingEncodeFrames -= received;
            m_processedFrames += received;
            QMetaObject::invokeMethod(this, &PipeWireProduce::handleEncodedFramesChanged, Qt::QueuedConnection);
        }
    });
```

`destroy()` sets the running flags to false and calls `wakePassthrough()`/`wakeOutput()` before joining, as it notifies today. (`videodamagetest` and the K5 smoke cover the loops; the AVC420 path's behaviour is unchanged apart from never losing a wake.)

`makeEncoder()` (`pipewireproduce.cpp:481`; the `H264Baseline/H264Main` case is `:491-500`), before the `h264_vaapi` attempt, behind `#if 0 // K5` until K5 lands (so K4 builds and behaves exactly as before); `setupStream()` emits `activeChromaModeChanged(m_activeChromaMode)` once right after `makeEncoder()` returned a non-null encoder, so the stream side learns the mode the encoder really opened with:

```cpp
        if (m_chromaMode != PipeWireBaseEncodedStream::ChromaMode::Yuv420 && (forcedEncoder.isNull() || forcedEncoder == u"h264_vaapi")) {
            auto encoder = std::make_unique<H264VAAPIAvc444Encoder>(profile, m_chromaMode, this); // K5
            if (setupEncoder(encoder.get(), size)) {
                m_activeChromaMode = m_chromaMode;
                return encoder;
            }
            qCWarning(PIPEWIRERECORD_LOGGING) << "AVC444 (4:4:4 chroma) needs h264_vaapi; falling back to 4:2:0";
        }
        m_activeChromaMode = PipeWireBaseEncodedStream::ChromaMode::Yuv420;
```

`src/pipewireencodedstream.h/.cpp`: extend `PipeWirePacketPrivate` with `const QByteArray aux; const bool auxIsKey;` (two constructors), add `Packet::aux()`, `Packet::auxIsKey()`, the `ChromaTiming` struct + signal + `qRegisterMetaType<PipeWireEncodedStream::ChromaTiming>()` in the `PipeWireEncodedStream` constructor. `PipeWireEncodeProduce`:

```cpp
void PipeWireEncodeProduce::processPacketPair(AVPacket *main, AVPacket *aux)
{
    if (!main && !aux) return;
    Q_EMIT newPacket(PipeWireEncodedStream::Packet(main && (main->flags & AV_PKT_FLAG_KEY),
                                                   main ? QByteArray(reinterpret_cast<char *>(main->data), main->size) : QByteArray(),
                                                   aux ? QByteArray(reinterpret_cast<char *>(aux->data), aux->size) : QByteArray(),
                                                   aux != nullptr));
}
void PipeWireEncodeProduce::reportChromaTiming(const PipeWireEncodedStream::ChromaTiming &timing)
{
    Q_EMIT m_encodedStream->chromaTimingReported(timing); // cross-thread: metatype registered, queued by Qt
}
```

- [ ] **Step 6: Build both trees, run every test**

Run: `cmake --build ~/dev/kpipewire/build-tests -j16 2>&1 | grep -E "warning|error"; ctest --test-dir ~/dev/kpipewire/build-tests --output-on-failure | tail -8`
Expected: zero warnings; `avc444splittest`, `avc444nalrewritertest`, `avc444streamtest`, `avc444pairertest`, `avc444planestest`, `videodamagetest` PASS.

- [ ] **Step 7: Commit and export patch 0015**

```bash
cd ~/dev/kpipewire && git add src/avc444pairer_p.h src/avc444planes_p.h src/autotests/ src/pipewirebaseencodedstream.h src/pipewirebaseencodedstream.cpp src/pipewireencodedstream.h src/pipewireencodedstream.cpp src/pipewireencodedstream_p.h src/pipewireproduce.cpp src/pipewireproduce_p.h
git commit -m "record: chroma mode / aux stream API, Packet::aux(), pts pairer and plane layout for AVC444 (KRDP OPT-045)

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git format-patch -1 HEAD --start-number 15 -o ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/
```

---

### Task K5: `H264VAAPIAvc444Encoder` — two contexts, the aux policy, the rewriter in the packet path

**Files:**
- Create: `src/h264vaapiavc444encoder_p.h`, `src/h264vaapiavc444encoder.cpp`
- Modify: `src/encoder_p.h` (`avCodecContext()`, `setQuality()`, `requestKeyFrame()` become `virtual`; `m_packetSink` + `emitPacket()`; `m_dmaBufHandler` moves up from `SoftwareEncoder`; `downloadToImage()`, `maybeDumpRgba()`; per-encoder pull timing), `src/encoder.cpp:148-180` (`receivePacket` → `emitPacket`), `:80-146` (pull timing), `:282-297` (`SoftwareEncoder::filterFrame` uses the helper), `:415-463` (`HardwareEncoder::filterFrame` dump hook); `src/h264vaapiencoder_p.h`, `src/h264vaapiencoder.cpp` (`Input` mode, `gopSize`, `queueSoftwareFrame()`, readers, `createCodecContext()` via `VaapiH264`, `buildEncodingOptions()` → `VaapiH264::encodingOptions()`); `src/pipewireproduce.cpp` (drop the `#if 0 // K5` guard); `src/CMakeLists.txt` (add the .cpp)

**Interfaces:**
- Consumes: K1/K2 (`Avc444Split`), K3 (`Avc444Nal::{MainStreamState, Rewriter}`, `VaapiH264`), K4 (`Avc444PacketPairer` incl. `auxLost`/`pendingMainPts`, `PlaneLayout`, `PipeWireProduce::{processPacketPair, reportChromaTiming, wakePassthrough, activeChromaModeChanged, m_auxStreamEnabled, m_activeChromaMode}` — `framePts()`, `m_stream` are already public in the tree, `Encoder::auxStreamEnabledChanged`), `HardwareEncoder::checkVaapi()` (`encoder.cpp:465-486`), `Encoder::{m_avCodecMutex, m_keyFrameRequested, m_qualityChangePending, reopenForQuality()}`.
- Produces:

```cpp
// encoder_p.h additions
class Encoder : public QObject {
public:
    virtual AVCodecContext *avCodecContext() const;
    virtual void setQuality(std::optional<quint8> quality);
    virtual void requestKeyFrame();
    /** Where receivePacket() delivers: PipeWireProduce::processPacket() unless a composite encoder redirects it. */
    void setPacketSink(std::function<void(AVPacket *)> sink);
    /** Microseconds spent in av_buffersink_get_frame() since the last takePullStats() call (the upload for a software-fed graph). */
    struct PullStats { qint64 totalUs = 0, maxUs = 0; int pulls = 0; };
    PullStats takePullStats();
protected:
    void emitPacket(AVPacket *packet);
    bool downloadToImage(const PipeWireFrame &frame, QImage &image);   // dma-buf -> RGBA8888 (R,G,B,A bytes) / memfd -> RGBA8888
    void maybeDumpRgba(const QImage &image);                           // KPIPEWIRE_DUMP_RGBA=<path>: raw RGBA of the last frame + <path>.txt "WxH"
    DmaBufHandler m_dmaBufHandler;
    std::function<void(AVPacket *)> m_packetSink;
    std::mutex m_pullMutex; PullStats m_pull;
};

// h264vaapiencoder_p.h
class H264VAAPIEncoder : public HardwareEncoder {
public:
    enum class Input { DmaBuf, Yuv420Upload };
    H264VAAPIEncoder(H264Profile profile, PipeWireProduce *produce, Input input = Input::DmaBuf, int gopSize = 600);
    bool initialize(const QSize &size) override;                 // DmaBuf: today's DRM graph; Yuv420Upload: VaapiH264::createUploadGraph
    bool filterFrame(const PipeWireFrame &frame) override;       // DmaBuf: HardwareEncoder's; Yuv420Upload: returns false (fed by queueSoftwareFrame)
    bool queueSoftwareFrame(AVFrame *frame);                      // Yuv420Upload only; av_buffersrc_add_frame (takes the frame's references)
    bool keyFrameRequested() const { return m_keyFrameRequested; }
    bool qualityChangePending() const { return m_qualityChangePending; }
    static int qpForQuality(const std::optional<quint8> &quality); // the 40..12 map (percentageToAbsoluteQuality made static)
protected:
    AVCodecContext *createCodecContext();                         // VaapiH264::openContext({m_size, profile, qp, m_gopSize, m_timeBase}, sink hw_frames_ctx)
    AVDictionary *buildEncodingOptions() override;                // VaapiH264::encodingOptions()
    const Input m_input; const int m_gopSize; AVRational m_timeBase{1, 1000}; QSize m_size; AVBufferRef *m_vaapiDevice = nullptr; VaapiH264::UploadGraph m_upload;
};

// h264vaapiavc444encoder_p.h
class H264VAAPIAvc444Encoder : public Encoder {
public:
    H264VAAPIAvc444Encoder(Encoder::H264Profile profile, PipeWireBaseEncodedStream::ChromaMode mode, PipeWireProduce *produce);
    bool initialize(const QSize &size) override;
    bool filterFrame(const PipeWireFrame &frame) override;         // download, split, policy, queue
    std::pair<int, int> encodeFrame(int maximumFrames) override;   // both contexts; counts main frames
    int receivePacket() override;                                  // both contexts; returns frames emitted WITH a main
    void finish() override;                                        // both
    AVCodecContext *avCodecContext() const override;               // the main's (setupStream's frame-repeat check)
    void setQuality(std::optional<quint8> quality) override;       // both
    void requestKeyFrame() override;                               // main
protected:
    int percentageToAbsoluteQuality(const std::optional<quint8> &q) override { return H264VAAPIEncoder::qpForQuality(q); }
};
```

- [ ] **Step 1: `Encoder` and `H264VAAPIEncoder` groundwork**

`encoder_p.h`/`encoder.cpp`: the three virtuals; `emitPacket(packet)` = `m_packetSink ? m_packetSink(packet) : m_produce->processPacket(packet)` used at `encoder.cpp:173`; in `Encoder::encodeFrame` wrap `av_buffersink_get_frame` (`:91`) with a `steady_clock` measurement added to `m_pull` under `m_pullMutex`; `takePullStats()` swaps it out. Move `DmaBufHandler m_dmaBufHandler;` from `SoftwareEncoder` (`:181`) to `Encoder` (protected) and add:

```cpp
bool Encoder::downloadToImage(const PipeWireFrame &frame, QImage &image)
{
    const QSize size = m_produce->m_stream->size();
    if (frame.dmabuf) {
        if (image.size() != size || image.format() != QImage::Format_RGBA8888) image = QImage(size, QImage::Format_RGBA8888);
        if (!m_dmaBufHandler.downloadFrame(image, frame)) {
            m_produce->m_stream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
            return false;
        }
        return true;
    }
    if (frame.dataFrame) {
        // toImage() yields R,G,B,A bytes for every 32-bit spa format (BGRx/BGRA are rgbSwapped there).
        image = frame.dataFrame->toImage().convertToFormat(QImage::Format_RGBA8888);
        return !image.isNull();
    }
    return false;
}

void Encoder::maybeDumpRgba(const QImage &image)
{
    static const QByteArray path = qgetenv("KPIPEWIRE_DUMP_RGBA");
    if (path.isEmpty()) return;
    QFile f(QString::fromLocal8Bit(path));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        for (int y = 0; y < image.height(); ++y) f.write(reinterpret_cast<const char *>(image.constScanLine(y)), qsizetype(image.width()) * 4);
    }
    QFile meta(QString::fromLocal8Bit(path) + QStringLiteral(".txt"));
    if (meta.open(QIODevice::WriteOnly | QIODevice::Truncate)) meta.write(QByteArray::number(image.width()) + 'x' + QByteArray::number(image.height()));
}
```

`SoftwareEncoder::filterFrame` (`encoder.cpp:282-297`): the dmabuf/dataFrame branches become `QImage image; if (!downloadToImage(frame, image)) return false; maybeDumpRgba(image);` (add `case QImage::Format_RGBA8888:` next to `Format_RGBA8888_Premultiplied` in `convertQImageFormatToAVPixelFormat`, `:46-48`). `HardwareEncoder::filterFrame` (`:415`): first lines `if (!qEnvironmentVariableIsEmpty("KPIPEWIRE_DUMP_RGBA")) { QImage img; if (downloadToImage(frame, img)) maybeDumpRgba(img); }` so the AVC420 path can produce the same reference frame (only when the variable is set).

`h264vaapiencoder.cpp`: constructor stores `m_input`, `m_gopSize`; `initialize()`:

```cpp
bool H264VAAPIEncoder::initialize(const QSize &size)
{
    if (m_input == Input::Yuv420Upload) {
        const QByteArray device = checkVaapi(size);
        if (device.isEmpty()) return false;
        QString error;
        m_vaapiDevice = VaapiH264::createDevice(device, &error);
        if (!m_vaapiDevice || !VaapiH264::createUploadGraph(m_upload, m_vaapiDevice, size, m_timeBase, &error)) {
            qCWarning(PIPEWIRERECORD_LOGGING) << "h264_vaapi upload path:" << error;
            return false;
        }
        m_avFilterGraph = m_upload.graph; m_inputFilter = m_upload.in; m_outputFilter = m_upload.out;
        m_upload.graph = nullptr; // Encoder::~Encoder frees m_avFilterGraph
        m_size = size;
        m_avCodecContext = createCodecContext();
        return m_avCodecContext != nullptr;
    }
    … the existing body (createDrmContext, the DRM/hwmap/scale_vaapi graph) unchanged …
}
bool H264VAAPIEncoder::queueSoftwareFrame(AVFrame *frame)
{
    Q_ASSERT(m_input == Input::Yuv420Upload);
    if (const int r = av_buffersrc_add_frame(m_inputFilter, frame); r < 0) { qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to queue a software picture:" << av_err2str(r); return false; }
    return true;
}
AVCodecContext *H264VAAPIEncoder::createCodecContext()
{
    QString error;
    const auto profile = m_profile == H264Profile::Baseline ? VaapiH264::Profile::Baseline : m_profile == H264Profile::High ? VaapiH264::Profile::High : VaapiH264::Profile::Main;
    AVCodecContext *context = VaapiH264::openContext({m_size, profile, qpForQuality(m_quality.value_or(70)), m_gopSize, m_timeBase}, av_buffersink_get_hw_frames_ctx(m_outputFilter), &error);
    if (!context) qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec:" << error;
    else maybeLogOptions(nullptr /* the options now live in VaapiH264::encodingOptions(); log them once there */);
    return context;
}
```

(`filterFrame` in upload mode returns false with a one-time warning; `~H264VAAPIEncoder` unrefs `m_vaapiDevice`. `percentageToAbsoluteQuality` calls the new static `qpForQuality`. Delete the local `buildEncodingOptions()` body in favour of `return VaapiH264::encodingOptions();` and keep `maybeLogOptions` on a fresh `encodingOptions()` dictionary so the `Using encoding options:` info line stays.)

- [ ] **Step 2: The composite encoder**

`src/h264vaapiavc444encoder_p.h`:

```cpp
/*
    SPDX-FileCopyrightText: 2026 Steve Westerhouse <steve.westerhouse@origami-analytics.com>
    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#pragma once

#include "avc444nalrewriter_p.h"
#include "avc444pairer_p.h"
#include "avc444planes_p.h"
#include "avc444split_p.h"
#include "encoder_p.h"
#include "h264vaapiencoder_p.h"

#include <QTimer>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>

struct AVBufferPool;

/**
 * AVC444 through two h264_vaapi contexts (spec §4.2 as amended 2026-09-19).
 *
 * The main context is a normal H264VAAPIEncoder fed the split's main picture by upload, so its
 * P chain costs what AVC420 costs; the aux context is intra-only and its access units are
 * rewritten by Avc444Nal::Rewriter into non-reference pictures of the main stream, which is the
 * single stream libfreerdp's avc444_decompress() feeds to one decoder. filterFrame() (produce
 * thread) downloads, splits and applies the aux policy; encodeFrame() (passthrough thread)
 * drives both contexts; receivePacket() (output thread) tracks the main stream, rewrites the
 * aux, pairs by pts and hands PipeWireProduce::processPacketPair() one frame at a time.
 */
class H264VAAPIAvc444Encoder : public Encoder
{
    Q_OBJECT
public:
    H264VAAPIAvc444Encoder(H264Profile profile, PipeWireBaseEncodedStream::ChromaMode mode, PipeWireProduce *produce);
    ~H264VAAPIAvc444Encoder() override;

    bool initialize(const QSize &size) override;
    bool filterFrame(const PipeWireFrame &frame) override;
    std::pair<int, int> encodeFrame(int maximumFrames) override;
    int receivePacket() override;
    void finish() override;
    AVCodecContext *avCodecContext() const override;
    void setQuality(std::optional<quint8> quality) override;
    void requestKeyFrame() override;

protected:
    int percentageToAbsoluteQuality(const std::optional<quint8> &quality) override { return H264VAAPIEncoder::qpForQuality(quality); }

private:
    AVFrame *wrapPicture(AVBufferRef *buffer, int64_t pts);      // AVFrame over one pooled buffer (takes a new ref)
    void onRestTimeout();                                          // produce thread
    void auxStreamEnabledChanged(bool enabled) override;           // produce thread: re-arm the refresh after the congestion rung lifts
    void onMainPacket(AVPacket *packet);                           // output thread (main sink)
    void onAuxPacket(AVPacket *packet);                            // output thread (aux sink)
    void emitFrame(const Avc444PacketPairer::Emitted &e);          // output thread
    void disableAux(const char *why);                              // any thread: SPS mismatch / unsupported stream
    void maybeReport();                                            // output thread, once per second

    const H264Profile m_profile;
    const Avc444Split::Version m_version;
    std::unique_ptr<H264VAAPIEncoder> m_main, m_aux;
    Avc444Split::Variant m_variant = Avc444Split::Variant::Auto;
    PlaneLayout m_layout;
    AVBufferPool *m_picturePool = nullptr;
    QImage m_downloaded;

    // policy (produce thread)
    int m_motionGapMs = 100, m_restMs = 150;                       // KPIPEWIRE_AVC444_MOTION_GAP_MS / _REST_MS
    int64_t m_lastQueuedPts = -1;
    bool m_lastWasLumaOnly = false, m_restRefreshDone = false;
    AVBufferRef *m_lastAuxBuffer = nullptr;                         // the split's aux planes of the last luma-only frame
    int64_t m_lastAuxPts = -1;
    QTimer m_restTimer;
    std::atomic_bool m_auxDisabled = false;                         // rewriter refused the stream: 4:2:0 for the rest of the session

    // aux pictures queued into the aux graph, in order (produce thread pushes, passthrough thread pops):
    // Encoder::encodeFrame() reports (filtered, queued) - the pictures pulled beyond `queued` never reached
    // the encoder (gate full or a failed send), and their mains must not wait for them.
    std::mutex m_auxOutstandingMutex;
    std::deque<int64_t> m_auxOutstanding;
    std::chrono::steady_clock::time_point m_pendingMainSince;       // output thread: when the currently held main started waiting
    std::mutex m_deferredMutex;
    std::vector<Avc444PacketPairer::Emitted> m_deferredEmits;       // releases decided on the passthrough thread, emitted on the output thread

    // pairing / rewriting (output thread)
    Avc444PacketPairer m_pairer;
    std::map<int64_t, AVPacket *> m_mainPackets, m_auxPackets;      // received, not yet emitted
    Avc444Nal::MainStreamState m_mainState;
    Avc444Nal::Rewriter m_rewriter;
    int m_emittedWithMain = 0;                                      // frames handed on during the current receivePacket()

    // statistics
    std::mutex m_statMutex;
    struct Stat { qint64 min = 0, max = 0, sum = 0; int n = 0; void add(qint64 v) { if (n == 0 || v < min) min = v; if (v > max) max = v; sum += v; ++n; } };
    Stat m_download, m_split, m_upload, m_mainLatency, m_auxLatency;
    std::map<int64_t, std::chrono::steady_clock::time_point> m_mainQueuedAt, m_auxQueuedAt;
    int m_frames = 0, m_auxSent = 0, m_auxSkippedMotion = 0, m_auxRestRefresh = 0, m_rewriteFailures = 0;
    std::chrono::steady_clock::time_point m_lastReport = std::chrono::steady_clock::now();
};
```

`src/h264vaapiavc444encoder.cpp`:

```cpp
#include "h264vaapiavc444encoder_p.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
}

#include "logging_record.h"
#include "pipewireencodedstream.h"

using namespace std::chrono;

namespace
{
int envInt(const char *name, int fallback)
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue(name, &ok);
    return ok && v >= 0 ? v : fallback;
}
}

H264VAAPIAvc444Encoder::H264VAAPIAvc444Encoder(H264Profile profile, PipeWireBaseEncodedStream::ChromaMode mode, PipeWireProduce *produce)
    : Encoder(produce)
    , m_profile(profile)
    , m_version(mode == PipeWireBaseEncodedStream::ChromaMode::Yuv444v1 ? Avc444Split::Version::V1 : Avc444Split::Version::V2)
{
    m_motionGapMs = envInt("KPIPEWIRE_AVC444_MOTION_GAP_MS", 100);
    m_restMs = envInt("KPIPEWIRE_AVC444_REST_MS", 150);
    if (m_restMs < m_motionGapMs) {
        // A rest shorter than the motion gap would send the refresh before the next frame would have
        // carried its aux anyway; the semantics need REST >= GAP.
        qCWarning(PIPEWIRERECORD_LOGGING) << "KPIPEWIRE_AVC444_REST_MS" << m_restMs << "< KPIPEWIRE_AVC444_MOTION_GAP_MS" << m_motionGapMs << "- using" << m_motionGapMs;
        m_restMs = m_motionGapMs;
    }
    m_restTimer.setSingleShot(true);
    m_restTimer.setInterval(m_restMs);
    connect(&m_restTimer, &QTimer::timeout, this, &H264VAAPIAvc444Encoder::onRestTimeout); // this object lives on the produce thread
}

H264VAAPIAvc444Encoder::~H264VAAPIAvc444Encoder()
{
    for (auto &[pts, p] : m_mainPackets) av_packet_free(&p);
    for (auto &[pts, p] : m_auxPackets) av_packet_free(&p);
    if (m_lastAuxBuffer) av_buffer_unref(&m_lastAuxBuffer);
    m_main.reset(); m_aux.reset();
    if (m_picturePool) av_buffer_pool_uninit(&m_picturePool);
}

bool H264VAAPIAvc444Encoder::initialize(const QSize &size)
{
    m_layout = PlaneLayout::forSize(size.width(), size.height());
    m_picturePool = av_buffer_pool_init(m_layout.bytes, nullptr);
    auto make = [&](int gop, std::function<void(AVPacket *)> sink) -> std::unique_ptr<H264VAAPIEncoder> {
        auto e = std::make_unique<H264VAAPIEncoder>(m_profile, m_produce, H264VAAPIEncoder::Input::Yuv420Upload, gop);
        e->setQuality(m_quality);
        e->setEncodingPreference(m_encodingPreference);
        e->setColorRange(m_colorRange);
        e->setPacketSink(std::move(sink));
        if (!e->initialize(size)) return nullptr;
        return e;
    };
    m_main = make(600, [this](AVPacket *p) { onMainPacket(p); });
    if (!m_main) return false;
    m_aux = make(1, [this](AVPacket *p) { onAuxPacket(p); });     // intra-only: every aux picture is an IDR before the rewrite
    if (!m_aux) return false;
    m_variant = Avc444Split::best();
    m_downloaded = QImage(size, QImage::Format_RGBA8888);
    qCInfo(PIPEWIRERECORD_LOGGING) << "AVC444" << (m_version == Avc444Split::Version::V1 ? "v1" : "v2") << "encoder:" << size << "split variant" << Avc444Split::name(m_variant)
                                   << "motion gap" << m_motionGapMs << "ms rest" << m_restMs << "ms";
    return true;
}

AVFrame *H264VAAPIAvc444Encoder::wrapPicture(AVBufferRef *buffer, int64_t pts)
{
    AVFrame *frame = av_frame_alloc();
    frame->buf[0] = av_buffer_ref(buffer);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = m_layout.width;
    frame->height = m_layout.height;
    frame->data[0] = frame->buf[0]->data + m_layout.offsetY; frame->linesize[0] = m_layout.strideY;
    frame->data[1] = frame->buf[0]->data + m_layout.offsetU; frame->linesize[1] = m_layout.strideUV;
    frame->data[2] = frame->buf[0]->data + m_layout.offsetV; frame->linesize[2] = m_layout.strideUV;
    frame->pts = pts;
    return frame;
}

bool H264VAAPIAvc444Encoder::filterFrame(const PipeWireFrame &frame)
{
    const auto t0 = steady_clock::now();
    if (!downloadToImage(frame, m_downloaded)) return false;
    maybeDumpRgba(m_downloaded);
    const auto t1 = steady_clock::now();

    const int64_t pts = m_produce->framePts(frame.presentationTimestamp);
    const bool enabled = m_produce->m_auxStreamEnabled.load() && !m_auxDisabled.load();
    const bool firstFrame = m_lastQueuedPts < 0;
    const int64_t gapMs = firstFrame ? std::numeric_limits<int64_t>::max() : pts - m_lastQueuedPts;
    // Whether the main will be a keyframe is decided on the passthrough thread; the cases that are
    // knowable here are the first picture, a pending keyframe request and a pending quality reopen.
    const bool keyLikely = firstFrame || m_main->keyFrameRequested() || m_main->qualityChangePending();
    const bool sendAux = enabled && (gapMs >= m_motionGapMs || keyLikely);

    AVBufferRef *mainBuf = av_buffer_pool_get(m_picturePool), *auxBuf = av_buffer_pool_get(m_picturePool);
    if (!mainBuf || !auxBuf) { av_buffer_unref(&mainBuf); av_buffer_unref(&auxBuf); return false; }
    Avc444Split::Planes planes{m_layout.planes(mainBuf->data), m_layout.planes(auxBuf->data)};
    const Avc444Split::Input in{m_downloaded.constBits(), int(m_downloaded.bytesPerLine()), m_layout.width, m_layout.height, Avc444Split::PixelOrder::Rgbx};
    Avc444Split::split(in, m_version, planes, m_variant);
    const auto t2 = steady_clock::now();

    AVFrame *mainPic = wrapPicture(mainBuf, pts);
    av_buffer_unref(&mainBuf);
    const bool queued = m_main->queueSoftwareFrame(mainPic);
    av_frame_free(&mainPic);
    if (!queued) { av_buffer_unref(&auxBuf); return false; }
    { std::lock_guard lock(m_statMutex); m_mainQueuedAt[pts] = t2; }

    m_restTimer.stop();
    if (sendAux) {
        AVFrame *auxPic = wrapPicture(auxBuf, pts);
        const bool auxQueued = m_aux->queueSoftwareFrame(auxPic);
        av_frame_free(&auxPic);
        m_pairer.expect(pts, auxQueued);
        if (auxQueued) {
            { std::lock_guard lock(m_auxOutstandingMutex); m_auxOutstanding.push_back(pts); }
            std::lock_guard lock(m_statMutex); m_auxQueuedAt[pts] = t2; m_auxSent++;
        }
        m_lastWasLumaOnly = false;
        if (m_lastAuxBuffer) av_buffer_unref(&m_lastAuxBuffer);
        av_buffer_unref(&auxBuf);
    } else {
        m_pairer.expect(pts, false);
        if (enabled) { std::lock_guard lock(m_statMutex); m_auxSkippedMotion++; }
        // Keep this frame's chroma: if the desk goes quiet it is sent as an aux-only refresh.
        if (m_lastAuxBuffer) av_buffer_unref(&m_lastAuxBuffer);
        m_lastAuxBuffer = auxBuf;   // ownership moves
        m_lastAuxPts = pts;
        m_lastWasLumaOnly = true;
        m_restRefreshDone = false;
        if (enabled) m_restTimer.start();
    }
    m_lastQueuedPts = pts;
    {
        std::lock_guard lock(m_statMutex);
        m_download.add(duration_cast<microseconds>(t1 - t0).count());
        m_split.add(duration_cast<microseconds>(t2 - t1).count());
        m_frames++;
    }
    return true;
}

// Produce thread. No frame for m_restMs after a luma-only one: send that frame's chroma alone.
// Exactly one such refresh per luma-only stretch (POC type 2 forbids two consecutive
// non-reference pictures), and none while the congestion rung has the aux stream off.
void H264VAAPIAvc444Encoder::onRestTimeout()
{
    if (!m_lastWasLumaOnly || m_restRefreshDone || !m_lastAuxBuffer || m_auxDisabled.load() || !m_produce->m_auxStreamEnabled.load()) return;
    AVFrame *auxPic = wrapPicture(m_lastAuxBuffer, m_lastAuxPts);
    const bool queued = m_aux->queueSoftwareFrame(auxPic);
    av_frame_free(&auxPic);
    if (!queued) return;
    m_pairer.expectAuxOnly(m_lastAuxPts);
    m_restRefreshDone = true;
    { std::lock_guard lock(m_auxOutstandingMutex); m_auxOutstanding.push_back(m_lastAuxPts); }
    { std::lock_guard lock(m_statMutex); m_auxQueuedAt[m_lastAuxPts] = steady_clock::now(); m_auxRestRefresh++; }
    // Not a frame for the produce's pending counters: only the passthrough thread has to wake up,
    // through the counted wake so the refresh cannot be lost while that thread is busy.
    m_produce->wakePassthrough();
}

// Produce thread, from PipeWireProduce::setAuxStreamEnabled(). Lifting the congestion rung on a
// static desk would otherwise leave the picture 4:2:0 until the next damage: arm the refresh for
// the chroma kept from the last luma-only frame. Switching off cancels a pending refresh.
void H264VAAPIAvc444Encoder::auxStreamEnabledChanged(bool enabled)
{
    if (!enabled) { m_restTimer.stop(); return; }
    if (m_lastWasLumaOnly && !m_restRefreshDone && m_lastAuxBuffer && !m_auxDisabled.load()) m_restTimer.start();
}

std::pair<int, int> H264VAAPIAvc444Encoder::encodeFrame(int maximumFrames)
{
    const auto mainCounts = m_main->encodeFrame(maximumFrames);   // reopen / keyframe request / gate as today, per context
    const auto [auxFiltered, auxQueued] = m_aux->encodeFrame(maximumFrames);
    // Encoder::encodeFrame() pulls the aux pictures in queue order, sends the first `auxQueued` and
    // loses the rest (gate full, or the picture whose avcodec_send_frame failed): tell the pairer so a
    // main does not wait for an aux that will never come (a burst's last frame would otherwise freeze).
    std::vector<int64_t> lost;
    {
        std::lock_guard lock(m_auxOutstandingMutex);
        for (int i = 0; i < auxFiltered && !m_auxOutstanding.empty(); ++i) {
            if (i >= auxQueued) lost.push_back(m_auxOutstanding.front());
            m_auxOutstanding.pop_front();
        }
    }
    for (const int64_t pts : lost) {
        const auto result = m_pairer.auxLost(pts);
        qCDebug(PIPEWIRERECORD_LOGGING) << "AVC444: aux picture" << pts << "never reached the encoder";
        // Emission belongs to the output thread (packet maps, tracker, rewriter): park the release
        // there; the produce wakes the output worker right after this call returns.
        std::lock_guard lock(m_deferredMutex);
        m_deferredEmits.insert(m_deferredEmits.end(), result.emitted.begin(), result.emitted.end());
    }
    for (auto *e : {m_main.get(), m_aux.get()}) {
        const auto pull = e->takePullStats();
        std::lock_guard lock(m_statMutex);
        if (pull.pulls) { m_upload.add(pull.totalUs / pull.pulls); if (pull.maxUs > m_upload.max) m_upload.max = pull.maxUs; }
    }
    return mainCounts;
}

int H264VAAPIAvc444Encoder::receivePacket()
{
    m_emittedWithMain = 0;              // int member, written by the sinks below on this thread
    {
        std::vector<Avc444PacketPairer::Emitted> deferred;
        { std::lock_guard lock(m_deferredMutex); deferred.swap(m_deferredEmits); }
        for (const auto &e : deferred) emitFrame(e);   // mains released by auxLost() on the passthrough thread
    }
    m_main->receivePacket();
    m_aux->receivePacket();
    // Belt and braces for the held main: whatever went wrong with its aux, it goes out luma-only after
    // ~2 frame periods rather than at the next damage.
    if (const auto pending = m_pairer.pendingMainPts()) {
        if (m_pendingMainSince == steady_clock::time_point{}) m_pendingMainSince = steady_clock::now();
        else if (steady_clock::now() - m_pendingMainSince > milliseconds(40)) {
            qCDebug(PIPEWIRERECORD_LOGGING) << "AVC444: releasing main" << *pending << "without its aux after 40 ms";
            for (const auto &e : m_pairer.auxLost(*pending).emitted) emitFrame(e);
            m_pendingMainSince = {};
        }
    } else {
        m_pendingMainSince = {};
    }
    maybeReport();
    return m_emittedWithMain;
}

void H264VAAPIAvc444Encoder::onMainPacket(AVPacket *packet)
{
    AVPacket *copy = av_packet_clone(packet);
    m_mainPackets[copy->pts] = copy;
    { std::lock_guard lock(m_statMutex); if (auto it = m_mainQueuedAt.find(copy->pts); it != m_mainQueuedAt.end()) { m_mainLatency.add(duration_cast<microseconds>(steady_clock::now() - it->second).count()); m_mainQueuedAt.erase(it); } }
    const auto result = m_pairer.mainReceived(copy->pts);
    for (const auto &e : result.emitted) emitFrame(e);
    for (int i = 0; i < result.droppedAux; ++i) qCDebug(PIPEWIRERECORD_LOGGING) << "AVC444: dropped an aux picture whose frame already went out";
    // Anything older than the newest emitted main is dead.
    for (auto it = m_auxPackets.begin(); it != m_auxPackets.end();) { if (it->first < copy->pts) { av_packet_free(&it->second); it = m_auxPackets.erase(it); } else ++it; }
}

void H264VAAPIAvc444Encoder::onAuxPacket(AVPacket *packet)
{
    AVPacket *copy = av_packet_clone(packet);
    m_auxPackets[copy->pts] = copy;
    { std::lock_guard lock(m_statMutex); if (auto it = m_auxQueuedAt.find(copy->pts); it != m_auxQueuedAt.end()) { m_auxLatency.add(duration_cast<microseconds>(steady_clock::now() - it->second).count()); m_auxQueuedAt.erase(it); } }
    const auto result = m_pairer.auxReceived(copy->pts);
    for (const auto &e : result.emitted) emitFrame(e);
    if (result.droppedAux) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "AVC444: stray aux picture at" << copy->pts << "dropped";
        if (auto it = m_auxPackets.find(copy->pts); it != m_auxPackets.end()) { av_packet_free(&it->second); m_auxPackets.erase(it); }
    }
}

void H264VAAPIAvc444Encoder::emitFrame(const Avc444PacketPairer::Emitted &e)
{
    AVPacket *main = nullptr, *aux = nullptr, *rewritten = nullptr;
    if (e.hasMain) { if (auto it = m_mainPackets.find(e.pts); it != m_mainPackets.end()) { main = it->second; m_mainPackets.erase(it); } }
    if (e.hasAux) { if (auto it = m_auxPackets.find(e.pts); it != m_auxPackets.end()) { aux = it->second; m_auxPackets.erase(it); } }
    if (main) m_mainState.trackMainPacket(std::span<const uint8_t>(main->data, size_t(main->size)), e.pts);
    // The K3 contract: an aux is rewritten right after the main it follows was tracked. For a pair that
    // is the line above; an at-rest refresh must still find its own main as the last one tracked - if a
    // newer main went out in between, the refresh is stale and is dropped (the pairer normally catches
    // this first; this is the last line of defence against an aux with frame_num/POC behind the stream).
    if (aux && !main && m_mainState.lastTrackedPts != e.pts) {
        qCDebug(PIPEWIRERECORD_LOGGING) << "AVC444: refresh for" << e.pts << "arrived after main" << m_mainState.lastTrackedPts << "- dropped";
        av_packet_free(&aux);
        return;
    }
    if (aux && !m_auxDisabled.load()) {
        const auto r = m_rewriter.rewrite(std::span<const uint8_t>(aux->data, size_t(aux->size)), m_mainState);
        if (r.status == Avc444Nal::Rewriter::Status::Ok) {
            rewritten = av_packet_alloc();
            av_new_packet(rewritten, int(r.annexB.size()));
            memcpy(rewritten->data, r.annexB.data(), r.annexB.size());
            rewritten->pts = aux->pts; rewritten->dts = aux->dts; rewritten->flags = AV_PKT_FLAG_KEY;
        } else if (r.status == Avc444Nal::Rewriter::Status::NoMainSps) {
            qCDebug(PIPEWIRERECORD_LOGGING) << "AVC444: aux picture before the first main IDR, dropped";
        } else {
            disableAux(Avc444Nal::Rewriter::statusName(r.status));
            qCWarning(PIPEWIRERECORD_LOGGING) << "AVC444 rewrite failed:" << r.detail;
        }
        if (!rewritten) { std::lock_guard lock(m_statMutex); m_rewriteFailures++; }
    }
    if (main || rewritten) m_produce->processPacketPair(main, rewritten);
    if (main) m_emittedWithMain++;
    av_packet_free(&main); av_packet_free(&aux); av_packet_free(&rewritten);
}

void H264VAAPIAvc444Encoder::disableAux(const char *why)
{
    if (m_auxDisabled.exchange(true)) return;
    qCWarning(PIPEWIRERECORD_LOGGING) << "AVC444: abandoning the chroma stream for this session (" << why << "); frames continue as 4:2:0";
    m_produce->m_activeChromaMode = PipeWireBaseEncodedStream::ChromaMode::Yuv420;
    // Output thread here; the produce's signal is emitted from its own thread and reaches the stream queued.
    QMetaObject::invokeMethod(m_produce, [produce = m_produce]() { Q_EMIT produce->activeChromaModeChanged(PipeWireBaseEncodedStream::ChromaMode::Yuv420); }, Qt::QueuedConnection);
}

void H264VAAPIAvc444Encoder::finish()
{
    m_restTimer.stop(); // produce thread (stateChanged / handleEncodedFramesChanged); the encoder is deleted from another thread later
    m_main->finish();
    m_aux->finish();
}
AVCodecContext *H264VAAPIAvc444Encoder::avCodecContext() const { return m_main ? m_main->avCodecContext() : nullptr; }
void H264VAAPIAvc444Encoder::setQuality(std::optional<quint8> quality)
{
    m_quality = quality;
    if (m_main) m_main->setQuality(quality);
    if (m_aux) m_aux->setQuality(quality);
}
void H264VAAPIAvc444Encoder::requestKeyFrame() { if (m_main) m_main->requestKeyFrame(); }

void H264VAAPIAvc444Encoder::maybeReport()
{
    const auto now = steady_clock::now();
    if (now - m_lastReport < seconds(1)) return;
    m_lastReport = now;
    PipeWireEncodedStream::ChromaTiming t;
    {
        std::lock_guard lock(m_statMutex);
        auto fill = [](const Stat &s, qint64 &mn, qint64 &avg, qint64 &mx) { mn = s.min; mx = s.max; avg = s.n ? s.sum / s.n : 0; };
        fill(m_download, t.downloadMin, t.downloadAvg, t.downloadMax);
        fill(m_split, t.splitMin, t.splitAvg, t.splitMax);
        fill(m_upload, t.uploadMin, t.uploadAvg, t.uploadMax);
        fill(m_mainLatency, t.encodeMainMin, t.encodeMainAvg, t.encodeMainMax);
        fill(m_auxLatency, t.encodeAuxMin, t.encodeAuxAvg, t.encodeAuxMax);
        t.frames = m_frames; t.auxSent = m_auxSent; t.auxSkippedMotion = m_auxSkippedMotion; t.auxRestRefresh = m_auxRestRefresh; t.rewriteFailures = m_rewriteFailures;
        t.splitVariant = Avc444Split::name(m_variant);
        m_download = m_split = m_upload = m_mainLatency = m_auxLatency = Stat{};
        m_frames = m_auxSent = m_auxSkippedMotion = m_auxRestRefresh = m_rewriteFailures = 0;
        std::erase_if(m_mainQueuedAt, [now](const auto &kv) { return now - kv.second > seconds(5); });
        std::erase_if(m_auxQueuedAt, [now](const auto &kv) { return now - kv.second > seconds(5); });
    }
    if (t.frames == 0 && t.auxRestRefresh == 0) return;
    qCDebug(PIPEWIRERECORD_LOGGING).noquote()
        << QStringLiteral("avc444 timing: frames %1 aux sent %2 skipped-motion %3 rest-refresh %4 rewrite-failures %5 split=%6 download %7/%8/%9 us split %10/%11/%12 us upload %13/%14/%15 us encode-main %16/%17/%18 us encode-aux %19/%20/%21 us")
               .arg(t.frames).arg(t.auxSent).arg(t.auxSkippedMotion).arg(t.auxRestRefresh).arg(t.rewriteFailures).arg(QString::fromLatin1(t.splitVariant))
               .arg(t.downloadMin).arg(t.downloadAvg).arg(t.downloadMax).arg(t.splitMin).arg(t.splitAvg).arg(t.splitMax)
               .arg(t.uploadMin).arg(t.uploadAvg).arg(t.uploadMax).arg(t.encodeMainMin).arg(t.encodeMainAvg).arg(t.encodeMainMax)
               .arg(t.encodeAuxMin).arg(t.encodeAuxAvg).arg(t.encodeAuxMax);
    m_produce->reportChromaTiming(t);
}

#include "moc_h264vaapiavc444encoder_p.cpp"
```

(`QString::arg` takes at most nine arguments per call; chain them exactly as written. The `moc_…` include at the end follows `encoder.cpp:522`; `encoder_p.h` needs `<functional>`, `<QFile>`, `<QImage>` for the new members.)

`src/CMakeLists.txt`: add `h264vaapiavc444encoder.cpp` to `KPipeWireRecord`. `pipewireproduce.cpp`: `#include "h264vaapiavc444encoder_p.h"`, remove the `#if 0 // K5` guard from K4.

- [ ] **Step 3: Build both trees, run all tests, relink, smoke through the unchanged KRDP harness**

Run: `cmake --build ~/dev/kpipewire/build-tests -j16 2>&1 | grep -E "warning|error"; LIBVA_DRIVER_NAME=radeonsi ctest --test-dir ~/dev/kpipewire/build-tests --output-on-failure | tail -8`
Expected: zero warnings, all PASS (the GPU test still passes: `VaapiH264` is unchanged by the refactor).

Relink (rule: no client on 3389, MainPID unchanged):

```bash
ss -tnp | grep ':3389' | grep ESTAB; systemctl --user show -p MainPID app-org.kde.krdpserver
~/dev/krdp/scripts/build-kpipewire.sh 2>&1 | tail -3        # ends with "OK: all KPipeWire libraries resolve to ..."
systemctl --user show -p MainPID app-org.kde.krdpserver      # identical
ctest --test-dir ~/dev/krdp/build --output-on-failure | tail -3
```

Smoke — the existing harness forced into 444 through the environment; it writes `frame.data` (the main stream only), which is enough to prove the policy and the two contexts run. Use the monitor with some damage (a terminal with a blinking cursor is enough; on a fully static desk `--wake-after 3,6,9` — pointer motion alone makes no damage — so expect few frames and say so):

```bash
cd /tmp && KPIPEWIRE_CHROMA_MODE=444v2 KPIPEWIRE_DUMP_RGBA=/tmp/avc444-ref.rgba LIBVA_DRIVER_NAME=radeonsi \
  QT_LOGGING_RULES="org.kde.krdp.debug=true;kpipewire_record_logging.debug=true" \
  ~/dev/krdp/build/bin/krdpplasmastreamer --monitor 0 --quality 80 --quit-after 15 --output /tmp/avc444-smoke.raw 2>&1 | tee /tmp/avc444-smoke.log | grep -E "AVC444|avc444 timing|Total frames|Key frames|Reopened|rewrite|abandon"
ffprobe -f h264 -i /tmp/avc444-smoke.raw 2>&1 | grep -E "Video"; ffmpeg -v error -f h264 -i /tmp/avc444-smoke.raw -f null - && echo "main stream decodes"
cat /tmp/avc444-ref.rgba.txt; ls -l /tmp/avc444-ref.rgba
```

Expected: `AVC444 v2 encoder: QSize(2560, 1440) split variant avx512 motion gap 100 ms rest 150 ms`; per-second `avc444 timing:` lines with `aux sent`/`skipped-motion`/`rest-refresh` counts that move with the desk (a blinking cursor: mostly `aux sent`, since blinks are ≥ 100 ms apart; a drag: `skipped-motion` climbing, then one `rest-refresh` when it stops), `rewrite-failures 0`, no `abandoning` line; `download` avg ≤ 3000 µs, `split` avg ≤ 1500 µs; `Total frames` > 0 with `Key frames` ≥ 1; the raw decodes (`h264 (Main), yuv420p, 2560x1440`); the dump is `2560x1440` / 14745600 bytes. Second run with `KPIPEWIRE_AVC444_SPLIT=scalar` (the split figure moves); third with `--quality-at 5:50` → exactly one `Reopened h264_vaapi` per context (two lines) and `rewrite-failures 0` afterwards (the main SPS after the reopen still equals the aux SPS). The aux bytes themselves are only visible once the krdp plan's S4 harness writes `.aux.raw`; the GPU test of K3 is the evidence they decode. Known cost left on the table (record it in the krdp plan's research.md numbers): the split computes the aux planes on every frame, also while the aux stream is switched off — the K1 interface has no "main only" mode and K1 is executing; a `split()` flag for it is a follow-up once the K5 timing line shows the split's share.

- [ ] **Step 4: Commit and export patch 0016**

```bash
cd ~/dev/kpipewire && git add src/h264vaapiavc444encoder_p.h src/h264vaapiavc444encoder.cpp src/h264vaapiencoder_p.h src/h264vaapiencoder.cpp src/encoder_p.h src/encoder.cpp src/pipewireproduce.cpp src/CMakeLists.txt
git commit -m "h264vaapi: AVC444 encoder - main context as today plus an intra-only aux context rewritten into non-reference pictures; motion/at-rest aux policy (KRDP OPT-045)

Smoke on hal9000 (2560x1440): download <D> us, split <S> us (avx512), upload <U> us,
main queue->packet <M> us, aux <A> us; policy counts over a drag + rest: <..>.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git format-patch -1 HEAD --start-number 16 -o ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/
```

---

### Task K6: Patch series check, relink verification, hand-off to the krdp plan

**Files:**
- Verify: `~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/0012-*.patch … 0016-*.patch`
- Modify: `~/dev/rdp/kpipewire-vaapi-fix/README.md` (if present; else create `PATCHES.md` next to the patches) — one line per new patch

- [ ] **Step 1: The exported series applies cleanly to the pre-plan state**

```bash
cd /tmp && rm -rf kpw-check && git clone -q ~/dev/kpipewire kpw-check && cd kpw-check && git checkout -q a181fa2 \
  && git am ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/001[2-6]-*.patch && git log --oneline -6 && git diff --quiet HEAD westers/opt-015 && echo IDENTICAL
```

Expected: five patches apply; `IDENTICAL`.

- [ ] **Step 2: Relink state and KRDP tests (no client on 3389; MainPID unchanged)**

```bash
ss -tnp | grep ':3389' | grep ESTAB; systemctl --user show -p MainPID app-org.kde.krdpserver
~/dev/krdp/scripts/check-kpipewire-link.sh
ctest --test-dir ~/dev/krdp/build --output-on-failure | tail -3
nm -D ~/dev/krdp/.deps/kpipewire/lib/x86_64-linux-gnu/libKPipeWireRecord.so.6.6.4 | grep -c "setChromaMode\|setAuxStreamEnabled\|chromaTimingReported"
```

Expected: `OK: all KPipeWire libraries resolve to …`, KRDP tests 100 % pass, the installed library exports the three new symbols (count ≥ 3), MainPID unchanged.

- [ ] **Step 3: Record the series**

Append to `~/dev/rdp/kpipewire-vaapi-fix/README.md` (create `PATCHES.md` if there is no README):

```
0012 avc444: Avc444Split scalar + libfreerdp variant, layout pinned by the decoder primitive (OPT-045)
0013 avc444: AVX2 / AVX-512 kernels + dispatch (OPT-045)
0014 avc444: NAL rewriter (intra aux -> non-reference pictures of the main stream), VaapiH264, GPU round-trip test (OPT-045)
0015 record: ChromaMode / aux stream API, Packet::aux(), pts pairer + plane layout (OPT-045)
0016 h264vaapi: AVC444 encoder, two contexts + motion/at-rest aux policy (OPT-045)
```

This directory is not a git repository (`~/dev/rdp` is not one); nothing to commit here. The end-to-end smoke of the aux stream on the wire (`.main.raw` + `.aux.raw`, per-picture byte counts, decoder-side chroma proof, live clients) is the krdp plan's S4/S5; K3's GPU test and K5's smoke are the evidence this plan leaves behind.

---

## Self-review notes (kept for the executor)

- Spec §4.1 `Avc444Split` — K1/K2 (interface differs from the spec's sketch by `PixelOrder`, `Version`, `Input`, `auxHeight()`: the CPU download is R,G,B,A bytes, the two wire layouts need a version, v1's aux Y plane has 16-row blocks). Spec §4.1 tests — K1/K2, with the stronger decoder round trip added.
- Spec §4.2 as amended 2026-09-19: two contexts (K5), the rewriter with its precondition and fallback (K3/K5), pairing by pts across the two encoders with the straggler rule, the lost-aux release (`auxLost` + the 40 ms belt-and-braces) and the aux-only refresh (K4/K5), the motion-gap / at-rest policy with env overrides (`REST ≥ GAP` enforced), the re-arm on `setAuxStreamEnabled(true)` and the policy counts in the timing line (K5), `setAuxStreamEnabled(false)` suppressing both (K5), `requestKeyFrame()` → main (aux pictures are all intra, so R4's "aux IDR after a luma-only stretch" is automatic), counted wake-ups for the produce workers (K4), the GPU round trip with its conformance walk as the first step of the rewriter task (K3 Step 1/7).
- Review fixes 2026-09-19 (plan reviews K3 / cross-plan): GPU test tail = luma-only main + refresh, deny-list log capture at every level + `thread_count = 1`, conformance walk, static main content with `< 10 %`, QP 12; rewrite-at-emission contract with `serial`/`lastTrackedPts`; marking bits gated on `nal_ref_idc`, `PrevRefFrameNum` from reference slices only, aux PPS `sps_id` check; `activeChromaModeChanged`; the aux planes are still split while the stream is off (documented follow-up, K1 is executing).
- Spec R8 numbers — K5 log line + `chromaTimingReported()`; the harness summary is the krdp plan's S4.
- Picture geometry: W × H everywhere; `PlaneLayout::bufferRows = roundUp16(H)` only sizes the pooled buffer for the v1 split; the v1 loss for `H % 16 ≠ 0` is documented in the Global Constraints.
- Type consistency (used with these exact spellings here and in the krdp plan): `Avc444Split::{Version, Variant, PixelOrder, Plane, I420, Planes, Input, auxHeight, roundUp16, best, available, name, split, splitScalar, splitScalarRegion, fillUndefinedPositions, splitAvx2, splitAvx512}`; `H264Bits::{ebspToRbsp, rbspToEbsp, splitAnnexB, NalHeader, BitReader, BitWriter}`; `Avc444Nal::{Sps, Pps, SliceHead, parseSps, parsePps, parseSliceHead, MainStreamState::{trackMainPacket(annexB, pts), serial, lastTrackedPts, lastFrameNum, lastPocLsb, haveIdr, sps, spsRbsp, pps}, Rewriter::{Status, Result, rewrite, statusName}}`; `VaapiH264::{Profile, Params, encodingOptions, openContext, createDevice, UploadGraph, createUploadGraph}`; `PlaneLayout::{forSize, planes, width, height, bufferRows, strideY, strideUV, offsetY/U/V, bytes}`; `Avc444PacketPairer::{expect, expectAuxOnly, auxLost, mainReceived, auxReceived, pendingMains, pendingMainPts, Emitted{pts, hasMain, hasAux}, Result{emitted, droppedAux}}`; `PipeWireBaseEncodedStream::{ChromaMode, setChromaMode, chromaMode, activeChromaMode, activeChromaModeChanged, setAuxStreamEnabled, auxStreamEnabled}`; `PipeWireEncodedStream::{Packet::aux, Packet::auxIsKey, ChromaTiming{frames, auxSent, auxSkippedMotion, auxRestRefresh, rewriteFailures, splitVariant, download*, split*, upload*, encodeMain*, encodeAux*}, chromaTimingReported}`; `PipeWireProduce::{processPacketPair, reportChromaTiming, setChromaMode, setAuxStreamEnabled, wakePassthrough, wakeOutput, activeChromaModeChanged, m_chromaMode, m_activeChromaMode, m_auxStreamEnabled, m_passthroughWork, m_outputWork}`; `Encoder::{avCodecContext, setQuality, requestKeyFrame, auxStreamEnabledChanged, setPacketSink, takePullStats, emitPacket, downloadToImage, maybeDumpRgba, m_dmaBufHandler}`; `H264VAAPIEncoder::{Input, queueSoftwareFrame, keyFrameRequested, qualityChangePending, qpForQuality, createCodecContext, m_timeBase, m_size}`; environment `KPIPEWIRE_CHROMA_MODE`, `KPIPEWIRE_AVC444_SPLIT`, `KPIPEWIRE_AVC444_MOTION_GAP_MS`, `KPIPEWIRE_AVC444_REST_MS`, `KPIPEWIRE_DUMP_RGBA`, `KPIPEWIRE_TEST_RENDER_NODE`.
