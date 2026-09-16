# OPT-016: Adaptive Quality That Reaches the Encoder — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make KRDP's video quality follow the measured network goodput and RTT, and make quality changes actually change the encoded pictures — today `setQuality()` on a running `h264_vaapi` encoder is a no-op because FFmpeg derives the fixed QP when the codec is opened.

**Architecture:** In the private KPipeWire, `H264VAAPIEncoder` learns to reopen its codec context (keeping the DRM device and filter graph) when the quality changes; the reopen happens on the passthrough thread right before the next `avcodec_send_frame`, under the existing `m_avCodecMutex`, and the reopened codec's first picture is an IDR. In KRDP, `NetworkDetection` gains the goodput measurement upstream KRDP added (`bandwidth()` in kbit/s, smoothed), and `VideoStream` gains a pure, unit-tested adaptive-quality step function ported from upstream (resolution-based full-quality bitrate anchors, ±5/−10 steps, RTT congestion gate, 1.5 s update interval) whose result is delivered to the session as `setVideoQuality()`. The `Quality` setting becomes the cap; a new `AdaptiveQuality` setting (default on) enables the loop.

**Tech Stack:** C++20/Qt 6.10/KF6, FFmpeg 8.0.1 `h264_vaapi` (CQP via `global_quality`, from Plan 1), FreeRDP 3.31 `rdpAutoDetect` bandwidth measurement, QtTest for the pure function, KConfigXT for the new key.

**Spec:** `~/dev/rdp/RDP_QUALITY_RESEARCH.md` §5.2 (OPT-016), §5.6; `~/dev/rdp/FORK_PERFORMANCE_REVIEW.md` §2.3, §10 row OPT-016, §13 ("OPT-016's premise that `setQuality()` takes effect on the next picture for `h264_vaapi` is unverified and probably false — needs a test first"). Prerequisite: Plan 1 (`docs/superpowers/plans/2026-09-16-opt-015-private-kpipewire-encoder.md`) complete — private KPipeWire at `~/dev/kpipewire` branch `westers/opt-015` with `requestKeyFrame()` and the CQP configuration.

## Global Constraints

- No `sudo`, no package installs, nothing under `/usr`; KPipeWire changes go to `~/dev/kpipewire` branch `westers/opt-015` and are built with `~/dev/krdp/scripts/build-kpipewire.sh`; patches re-exported to `~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/` at the end.
- KRDP must still compile against stock KPipeWire 6.6.4; nothing in this plan adds a new KPipeWire API that KRDP calls (the reopen is internal to KPipeWire's `setQuality()`), so no new `requires` checks are needed.
- Quality→QP mapping stays `QP = round(40 − 0.28·quality)` (Plan 1). The RDPGFX metablock `quantQualityVals.qp` sent per frame must equal the QP the encoder is actually using for that quality.
- `VideoStream` runs on the FreeRDP peer thread (queueFrame/submission thread); sessions live on the main thread — quality changes cross that boundary with a `Qt::QueuedConnection`, exactly like `keyFrameRequested` in `server/SessionController.cpp:48`.
- Before `systemctl --user restart app-org.kde.krdpserver.service`: `ss -tnp | grep ':3389' | grep ESTAB` must be empty; otherwise skip and report.
- Every commit message ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. No amends, rebases, pushes, or `git stash`.
- Harness environment: `export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 QT_QPA_PLATFORM=wayland LIBVA_DRIVER_NAME=radeonsi QT_LOGGING_RULES="org.kde.krdp.debug=true;kpipewire*.debug=true;kpipewire*.info=true"`; binary `~/dev/krdp/build/bin/krdpplasmastreamer`, run from a scratch directory.

---

### Task 1: KPipeWire — reopen `h264_vaapi` on quality change

**Files:**
- Modify: `~/dev/kpipewire/src/encoder_p.h` (`Encoder`: add `virtual bool reopenForQuality()` returning `false`; add `std::atomic_bool m_qualityChangePending = false;`; `H264VAAPIEncoder`: declare `bool openCodec();` and `bool reopenForQuality() override;` in `h264vaapiencoder_p.h`)
- Modify: `~/dev/kpipewire/src/encoder.cpp` (`Encoder::setQuality`: set the pending flag; `Encoder::encodeFrame`: consume it before the send loop)
- Modify: `~/dev/kpipewire/src/h264vaapiencoder.cpp` (extract `openCodec()` from `initialize()`; implement `reopenForQuality()`)
- Modify: `~/dev/kpipewire/src/h264vaapiencoder_p.h`
- Modify: `~/dev/krdp/examples/plasmastreamer/main.cpp` (`--quality-at <s:q[,s:q…]>` option calling `session.setVideoQuality(q)`)

**Interfaces:**
- Consumes: Plan 1's `H264VAAPIEncoder::buildEncodingOptions()` / `percentageToAbsoluteQuality()`.
- Produces: `PipeWireBaseEncodedStream::setQuality(quint8)` now takes effect on the next picture for `h264_vaapi` (an IDR at the new QP); software encoders unchanged. Harness option `--quality-at`.

- [ ] **Step 1: Add the failing harness test**

In `examples/plasmastreamer/main.cpp` options block add:
```cpp
        {u"quality-at"_s, u"Set the session video quality at these offsets: seconds:quality, comma separated (e.g. 4:40,8:90)"_s, u"list"_s},
```
After the `keyframe-at` block add:
```cpp
    if (parser.isSet(u"quality-at"_s)) {
        const auto entries = parser.value(u"quality-at"_s).split(u',', Qt::SkipEmptyParts);
        QObject::connect(&session, &KRdp::AbstractSession::started, &application, [&session, &sinceStarted, entries]() {
            for (const auto &entry : entries) {
                const auto parts = entry.split(u':');
                if (parts.size() != 2) {
                    qWarning() << "Ignoring malformed --quality-at entry" << entry;
                    continue;
                }
                const int offset = parts[0].toInt();
                const int quality = std::clamp(parts[1].toInt(), 0, 100);
                QTimer::singleShot(offset * 1000, &session, [&session, &sinceStarted, quality]() {
                    qWarning() << "Setting video quality to" << quality << "at +" << sinceStarted.elapsed() << "ms";
                    session.setVideoQuality(quint8(quality));
                });
            }
        });
    }
```
Build (`cmake --build ~/dev/krdp/build -j16 --target krdpplasmastreamer`) and run from `/tmp/krdp-q1`:
```bash
timeout 40 ~/dev/krdp/build/bin/krdpplasmastreamer --monitor 0 --quality 80 --quit-after 12 --wake-after 2,5,9 --quality-at 4:40,8:100 --output q1a.raw > q1a.log 2>&1; echo exit=$?
grep -nE 'Setting video quality|fixed QP|keyframe true' q1a.log
```
Expected (RED): exactly one `Using fixed QP = 18 / 18 …` line (at open); no new QP line after the quality changes; only the initial keyframe. That is the defect.

- [ ] **Step 2: Extract `openCodec()` in H264VAAPIEncoder**

In `h264vaapiencoder_p.h` add to the class: `bool openCodec();` (private) and `bool reopenForQuality() override;` (public). In `h264vaapiencoder.cpp`, move everything in `initialize()` from `auto codec = avcodec_find_encoder_by_name("h264_vaapi");` through the `avcodec_open2` block into:
```cpp
bool H264VAAPIEncoder::openCodec()
{
    auto codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "h264_vaapi codec not found";
        return false;
    }

    m_avCodecContext = avcodec_alloc_context3(codec);
    if (!m_avCodecContext) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not allocate video codec context";
        return false;
    }

    Q_ASSERT(!m_size.isEmpty());
    m_avCodecContext->width = m_size.width();
    m_avCodecContext->height = m_size.height();
    m_avCodecContext->max_b_frames = 0;
    // Keyframes are requested on demand (Encoder::requestKeyFrame); the periodic
    // IDR only remains as a safety net for a lost one (10 s at 60 fps).
    m_avCodecContext->gop_size = 600;
    m_avCodecContext->pix_fmt = AV_PIX_FMT_VAAPI;
    m_avCodecContext->time_base = AVRational{1, 1000};
    // Constant QP; rc_mode=CQP is set explicitly in buildEncodingOptions().
    m_avCodecContext->global_quality = percentageToAbsoluteQuality(m_quality.value_or(70));

    switch (m_profile) {
    case H264Profile::Baseline:
        m_avCodecContext->profile = AV_PROFILE_H264_CONSTRAINED_BASELINE;
        break;
    case H264Profile::Main:
        m_avCodecContext->profile = AV_PROFILE_H264_MAIN;
        break;
    case H264Profile::High:
        m_avCodecContext->profile = AV_PROFILE_H264_HIGH;
        break;
    }

    AVDictionary *options = buildEncodingOptions();
    maybeLogOptions(options);

    // The codec needs the VAAPI frames context the filter graph created.
    m_avCodecContext->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(m_outputFilter));

    if (int result = avcodec_open2(m_avCodecContext, codec, &options); result < 0) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Could not open codec" << av_err2str(result);
        av_dict_free(&options);
        return false;
    }
    av_dict_free(&options);
    return true;
}
```
`initialize()` stores `m_size = size;` (add `QSize m_size;` to the class) and ends with `return openCodec();`. Note the pre-existing bug fixed in passing: the old code logged `av_err2str(ret)` (the filter-graph return) instead of `result`.

- [ ] **Step 3: Implement the reopen**

`h264vaapiencoder.cpp`:
```cpp
bool H264VAAPIEncoder::reopenForQuality()
{
    // Called on the passthrough thread with m_avCodecMutex held. FFmpeg's VA-API
    // encoder derives its fixed QPs when the codec is opened, so a quality change
    // needs a fresh codec context; the DRM device and filter graph are kept and
    // the first picture of the new context is an IDR.
    avcodec_free_context(&m_avCodecContext);
    if (!openCodec()) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "Failed to reopen h264_vaapi for the new quality";
        return false;
    }
    qCInfo(PIPEWIRERECORD_LOGGING) << "Reopened h264_vaapi at quality" << m_quality.value_or(70) << "(QP" << m_avCodecContext->global_quality << ")";
    return true;
}
```
`encoder_p.h` (`Encoder`, protected): `virtual bool reopenForQuality() { return false; }` and `std::atomic_bool m_qualityChangePending = false;`.
`encoder.cpp` `Encoder::setQuality`:
```cpp
void Encoder::setQuality(std::optional<quint8> quality)
{
    if (m_quality == quality) {
        return;
    }
    m_quality = quality;
    if (m_avCodecContext) {
        // Software encoders pick this up live; hardware encoders reopen on the
        // passthrough thread (see encodeFrame).
        m_avCodecContext->global_quality = percentageToAbsoluteQuality(quality);
        m_qualityChangePending = true;
    }
}
```
`encoder.cpp` `Encoder::encodeFrame`, inside the loop right before the existing `if (m_keyFrameRequested.exchange(false))` block:
```cpp
            if (m_qualityChangePending.exchange(false)) {
                std::lock_guard guard(m_avCodecMutex);
                if (!reopenForQuality()) {
                    // The old context is gone; nothing more can be encoded.
                    av_frame_unref(frame);
                    break;
                }
            }
```
(`reopenForQuality()` returns `false` for software encoders without touching anything — but the flag is only set when `m_avCodecContext` exists; to avoid the software path hitting the `break`, make the base implementation return `true` and do nothing: `virtual bool reopenForQuality() { return true; }`. Use that form.)

- [ ] **Step 4: Build, run, verify**

```bash
~/dev/krdp/scripts/build-kpipewire.sh 2>&1 | tail -3
cd /tmp/krdp-q1 && timeout 40 ~/dev/krdp/build/bin/krdpplasmastreamer --monitor 0 --quality 80 --quit-after 12 --wake-after 2,5,9 --quality-at 4:40,8:100 --output q1b.raw > q1b.log 2>&1; echo exit=$?
grep -nE 'Setting video quality|Reopened h264_vaapi|fixed QP|keyframe true|Total frames|Key frames' q1b.log
ffprobe -hide_banner -f h264 -i q1b.raw 2>&1 | grep Stream
```
Expected (GREEN): three `fixed QP` lines — `18 / 18`, then `29 / 29` (quality 40 → QP 28.8 → 29) after the +4 s change, then `12 / 12` after +8 s; a `Reopened h264_vaapi at quality 40 (QP 29)` line and one for 100; a `keyframe true` frame right after each reopen (the +5 s and +9 s wakes provide damage); `Key frames: 3`; valid `h264 (Main)`.

- [ ] **Step 5: Commit**

```bash
cd ~/dev/kpipewire && git add src/encoder_p.h src/encoder.cpp src/h264vaapiencoder.cpp src/h264vaapiencoder_p.h
git commit -m "h264vaapi: reopen the codec when the quality changes

FFmpeg's VA-API encoder derives its fixed QPs when the codec is opened,
so setQuality() on a running stream never changed a picture. The codec
context is now recreated on the passthrough thread (DRM device and filter
graph are kept); the reopened encoder starts with an IDR at the new QP.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
cd ~/dev/krdp && git add examples/plasmastreamer/main.cpp
git commit -m "examples: plasmastreamer --quality-at to exercise live quality changes

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: KRDP — goodput measurement in NetworkDetection

**Files:**
- Modify: `~/dev/krdp/src/NetworkDetection.h`
- Modify: `~/dev/krdp/src/NetworkDetection.cpp`

**Interfaces:**
- Consumes: FreeRDP `rdpAutoDetect::BandwidthMeasureResults` callback (already wired at `NetworkDetection.cpp:102`, currently discarding its `timeDelta`/`byteCount`).
- Produces: `quint32 NetworkDetection::bandwidth() const` (kbit/s, exponentially smoothed, 0 until the first result) and `Q_SIGNAL void bandwidthChanged()`; `bool onBandwidthMeasureResults(uint32_t timeDelta, uint32_t byteCount)`. Task 3 consumes both.

- [ ] **Step 1: Port upstream's measurement**

Reference: `git -C ~/dev/krdp show origin/master:src/NetworkDetection.cpp` (upstream KDE krdp) lines 34-36 (constants), 47-50 (callback signature passing `timeDelta, byteCount`), 73-88 (private members), 110-112 (`bandwidth()`), 194-215 (`onBandwidthMeasureResults`). Port these into the fork, keeping the fork's state machine (`State::None/PendingStop/PendingResults`) and its `rttMeasurements` code untouched:

`NetworkDetection.h` — add to the public section:
```cpp
    /**
     * Measured goodput of the connection in kbit/s, exponentially smoothed.
     * 0 until the first bandwidth measurement has completed.
     */
    Q_PROPERTY(quint32 bandwidth READ bandwidth NOTIFY bandwidthChanged)
    quint32 bandwidth() const;
    Q_SIGNAL void bandwidthChanged();
```
and change the private declaration to `bool onBandwidthMeasureResults(uint32_t timeDelta, uint32_t byteCount);`.

`NetworkDetection.cpp` — constants next to the existing ones:
```cpp
constexpr double bandwidthSmoothingWeight = 0.5;
```
callback:
```cpp
BOOL bwMeasureResults(rdpAutoDetect *rdpAutodetect, RDP_TRANSPORT_TYPE, uint16_t, uint16_t, uint32_t timeDelta, uint32_t byteCount)
{
    auto context = reinterpret_cast<RdpConnection::RdpPeerContext *>(rdpAutodetect->context);
    if (context->networkDetection->onBandwidthMeasureResults(timeDelta, byteCount)) {
        return TRUE;
    }
    return FALSE;
}
```
(keep whatever the fork's existing body does around the call — only the arguments change). Private members:
```cpp
    double smoothedBandwidthBps = 0.0;
    bool hasSmoothedBandwidth = false;
    std::atomic<uint32_t> averageBandwidthBps{0};
```
accessor and results:
```cpp
quint32 NetworkDetection::bandwidth() const
{
    return d->averageBandwidthBps.load() * 8 / 1000;
}

bool NetworkDetection::onBandwidthMeasureResults(uint32_t timeDelta, uint32_t byteCount)
{
    if (d->state != State::PendingResults) {
        return false;
    }
    d->state = State::None;

    if (timeDelta == 0 || byteCount == 0) {
        return true;
    }

    const auto bytesPerSecond = static_cast<uint32_t>((static_cast<uint64_t>(byteCount) * 1000ULL) / static_cast<uint64_t>(timeDelta));
    if (!d->hasSmoothedBandwidth) {
        d->hasSmoothedBandwidth = true;
        d->smoothedBandwidthBps = bytesPerSecond;
    } else {
        d->smoothedBandwidthBps = (1.0 - bandwidthSmoothingWeight) * d->smoothedBandwidthBps + bandwidthSmoothingWeight * bytesPerSecond;
    }
    d->averageBandwidthBps.store(static_cast<uint32_t>(d->smoothedBandwidthBps));
    Q_EMIT bandwidthChanged();
    return true;
}
```
Keep the fork's existing `if (d->state != State::PendingResults) return false; d->state = State::None;` semantics exactly (read the fork's current function first and merge rather than overwrite).

- [ ] **Step 2: Build and prove the callback carries data**

`cmake --build ~/dev/krdp/build -j16 2>&1 | tail -2`. There is no FreeRDP client on this machine, so the runtime proof is deferred to Task 5 (journal shows `Adaptive quality -> … goodput N kbit/s` during Steve's session). Add a one-line `qCDebug(KRDP) << "Bandwidth measurement:" << byteCount << "bytes in" << timeDelta << "ms ->" << bandwidth() << "kbit/s";` at the end of `onBandwidthMeasureResults` so the journal will show it.

- [ ] **Step 3: Commit**

```bash
cd ~/dev/krdp && git add src/NetworkDetection.h src/NetworkDetection.cpp
git commit -m "network: measure connection goodput from FreeRDP bandwidth results

Ports upstream's smoothed bandwidth estimate (kbit/s) so the video
stream can steer quality; the callback previously discarded timeDelta
and byteCount.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: KRDP — pure adaptive-quality step function with unit test

**Files:**
- Create: `~/dev/krdp/src/AdaptiveQuality.h`
- Create: `~/dev/krdp/autotests/AdaptiveQualityTest.cpp`
- Modify: `~/dev/krdp/autotests/CMakeLists.txt`, `~/dev/krdp/CMakeLists.txt` (enable `add_subdirectory(autotests)` under `BUILD_TESTING`, if not already)

**Interfaces:**
- Produces:
```cpp
namespace KRdp::AdaptiveQuality {
constexpr int MinQuality = 10;
constexpr int StepUp = 5;
constexpr int StepDown = 10;
// "Quality 100" bitrate targets by resolution (RustDesk base_bitrate anchors).
double fullQualityKbit(double pixels);
struct Input { int current; int cap; quint32 goodputKbit; double pixels; std::chrono::milliseconds averageRtt; std::chrono::milliseconds minimumRtt; };
struct Result { int next; int target; bool congested; };
Result step(const Input &in);
}
```
Task 4 calls `step()` from `VideoStream`.

- [ ] **Step 1: Write the failing test**

`autotests/AdaptiveQualityTest.cpp`:
```cpp
#include <QTest>
#include "AdaptiveQuality.h"

using namespace KRdp::AdaptiveQuality;
using namespace std::chrono_literals;

class AdaptiveQualityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void fullQualityAnchorsScaleLinearly()
    {
        QCOMPARE(fullQualityKbit(2560.0 * 1440.0), 4500.0);
        QCOMPARE(fullQualityKbit(1920.0 * 1080.0), 3110.0);
        // Halfway between anchors: nearest anchor, scaled by pixel ratio.
        QVERIFY(fullQualityKbit(2560.0 * 1080.0) > 3110.0);
        QVERIFY(fullQualityKbit(2560.0 * 1080.0) < 4500.0);
    }
    void stepsUpSlowlyTowardsTarget()
    {
        const auto r = step({.current = 50, .cap = 100, .goodputKbit = 9000, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms});
        QCOMPARE(r.target, 100);
        QCOMPARE(r.next, 55);
        QVERIFY(!r.congested);
    }
    void stepsDownFasterTowardsTarget()
    {
        const auto r = step({.current = 80, .cap = 100, .goodputKbit = 900, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms});
        QCOMPARE(r.target, 20); // 900/4500*100
        QCOMPARE(r.next, 70);
    }
    void neverExceedsCapOrDropsBelowMinimum()
    {
        QCOMPARE(step({.current = 80, .cap = 80, .goodputKbit = 90000, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms}).next, 80);
        QCOMPARE(step({.current = 12, .cap = 100, .goodputKbit = 1, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms}).next, MinQuality);
    }
    void congestionForcesAStepDownAndBlocksStepUp()
    {
        const auto r = step({.current = 60, .cap = 100, .goodputKbit = 9000, .pixels = 2560.0 * 1440.0, .averageRtt = 60ms, .minimumRtt = 20ms});
        QVERIFY(r.congested);
        QCOMPARE(r.next, 50);
    }
    void zeroGoodputLeavesQualityAlone()
    {
        const auto r = step({.current = 60, .cap = 100, .goodputKbit = 0, .pixels = 2560.0 * 1440.0, .averageRtt = 10ms, .minimumRtt = 10ms});
        QCOMPARE(r.next, 60);
    }
};

QTEST_GUILESS_MAIN(AdaptiveQualityTest)
#include "AdaptiveQualityTest.moc"
```
`autotests/CMakeLists.txt`:
```cmake
add_executable(AdaptiveQualityTest AdaptiveQualityTest.cpp)
target_include_directories(AdaptiveQualityTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(AdaptiveQualityTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME AdaptiveQualityTest COMMAND AdaptiveQualityTest)
```
In the top-level `CMakeLists.txt` make sure `find_package(Qt6 … Test)` and `if(BUILD_TESTING) add_subdirectory(autotests) endif()` exist (add them if not; the fork's `autotests/CMakeLists.txt` currently exists but is empty).

- [ ] **Step 2: Run it to verify it fails**

`cmake -S ~/dev/krdp -B ~/dev/krdp/build -DBUILD_TESTING=ON >/dev/null && cmake --build ~/dev/krdp/build -j16 --target AdaptiveQualityTest 2>&1 | tail -3`
Expected: FAIL to compile — `AdaptiveQuality.h` not found.

- [ ] **Step 3: Implement the header**

`src/AdaptiveQuality.h`:
```cpp
// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#include <QtGlobal>

namespace KRdp::AdaptiveQuality
{

constexpr int MinQuality = 10;
constexpr int StepUp = 5;
constexpr int StepDown = 10;

struct BitrateAnchor {
    double pixels;
    double kbit;
};

// "Quality 100" bitrate targets by resolution, no fps term (RustDesk's base_bitrate()).
constexpr std::array<BitrateAnchor, 4> FullQualityBitrateAnchors = {{
    {921'600.0, 1500.0}, // 1280x720
    {2'073'600.0, 3110.0}, // 1920x1080
    {3'686'400.0, 4500.0}, // 2560x1440
    {8'294'400.0, 7500.0}, // 3840x2160
}};

inline double fullQualityKbit(double pixels)
{
    const auto *nearest = std::min_element(FullQualityBitrateAnchors.begin(), FullQualityBitrateAnchors.end(), [pixels](const auto &a, const auto &b) {
        return std::abs(a.pixels - pixels) < std::abs(b.pixels - pixels);
    });
    return nearest->kbit * (pixels / nearest->pixels);
}

struct Input {
    int current;
    int cap;
    quint32 goodputKbit;
    double pixels;
    std::chrono::milliseconds averageRtt;
    std::chrono::milliseconds minimumRtt;
};

struct Result {
    int next;
    int target;
    bool congested;
};

// One control step: move `current` towards the quality the measured goodput can
// carry at this resolution, slowly upwards and quickly downwards; a rising RTT
// (average > 1.5x minimum) counts as congestion and forces a step down.
inline Result step(const Input &in)
{
    if (in.goodputKbit == 0 || in.pixels <= 0.0) {
        return {in.current, in.current, false};
    }
    const int hi = std::max(in.cap, MinQuality);
    int target = std::clamp(int(std::lround(in.goodputKbit / fullQualityKbit(in.pixels) * 100.0)), MinQuality, hi);

    const bool congested = in.minimumRtt.count() > 0 && in.averageRtt.count() > in.minimumRtt.count() * 3 / 2;
    if (congested) {
        target = std::clamp(in.current - StepDown, MinQuality, target);
    }

    int next = in.current;
    if (target < next) {
        next = std::max(target, next - StepDown);
    } else if (target > next && !congested) {
        next = std::min(target, next + StepUp);
    }
    return {next, target, congested};
}

}
```

- [ ] **Step 4: Run the test to verify it passes**

`cmake --build ~/dev/krdp/build -j16 --target AdaptiveQualityTest 2>&1 | tail -2 && ~/dev/krdp/build/bin/AdaptiveQualityTest` (or `ctest --test-dir ~/dev/krdp/build -R AdaptiveQualityTest --output-on-failure`)
Expected: `Totals: 6 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
cd ~/dev/krdp && git add src/AdaptiveQuality.h autotests/AdaptiveQualityTest.cpp autotests/CMakeLists.txt CMakeLists.txt
git commit -m "video: pure adaptive-quality step function with unit tests

Ported from upstream krdp's updateAdaptiveQuality(): resolution-based
full-quality bitrate anchors, +5/-10 steps, RTT-based congestion gate.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: KRDP — drive the session's quality from VideoStream

**Files:**
- Modify: `~/dev/krdp/src/VideoStream.h` (`Q_SIGNAL void requestedQualityChanged(quint8 quality);`, `void setQualityCap(quint8 cap);`, `void setAdaptiveQuality(bool enabled);`)
- Modify: `~/dev/krdp/src/VideoStream.cpp` (quality state in `Private`; `updateAdaptiveQuality()` slot connected to `networkDetection()->bandwidthChanged`; metablock `qp` from current quality; initial emit)
- Modify: `~/dev/krdp/server/SessionController.cpp` (connect `requestedQualityChanged` → `session->setVideoQuality`, `Qt::QueuedConnection`; pass `Quality` as the cap; pass `AdaptiveQuality`)
- Modify: `~/dev/krdp/src/kcm/krdpserversettings.kcfg` (`AdaptiveQuality` Bool default true), `~/dev/krdp/server/main.cpp` (apply it at startup and in `applyRuntimeConfig`, add `adaptive=` to the startup summary)

**Interfaces:**
- Consumes: `KRdp::AdaptiveQuality::step()` (Task 3), `NetworkDetection::bandwidth()/bandwidthChanged()/averageRTT()/minimumRTT()` (Task 2), `AbstractSession::setVideoQuality(quint8)` (existing).
- Produces: `VideoStream::requestedQualityChanged(quint8)`; config key `AdaptiveQuality`.

- [ ] **Step 1: VideoStream state and slot**

In `VideoStream::Private` add:
```cpp
    quint8 quality = 100; // current adaptive value
    quint8 qualityCap = 100; // configured Quality
    bool adaptiveQuality = true;
    std::chrono::system_clock::time_point lastQualityUpdate;
```
and the constant `constexpr auto QualityUpdateInterval = std::chrono::milliseconds(1500);` next to the file's other constants. In `VideoStream.h` (public):
```cpp
    void setQualityCap(quint8 cap);
    void setAdaptiveQuality(bool enabled);
    Q_SIGNAL void requestedQualityChanged(quint8 quality);
```
In `VideoStream.cpp`:
```cpp
void VideoStream::setQualityCap(quint8 cap)
{
    d->qualityCap = cap;
    const quint8 next = d->adaptiveQuality ? std::min(d->quality, d->qualityCap) : d->qualityCap;
    if (next != d->quality) {
        d->quality = next;
        Q_EMIT requestedQualityChanged(d->quality);
    }
}

void VideoStream::setAdaptiveQuality(bool enabled)
{
    if (d->adaptiveQuality == enabled) {
        return;
    }
    d->adaptiveQuality = enabled;
    if (!enabled && d->quality != d->qualityCap) {
        d->quality = d->qualityCap;
        Q_EMIT requestedQualityChanged(d->quality);
    }
}

void VideoStream::updateAdaptiveQuality()
{
    if (!d->adaptiveQuality) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    if (now - d->lastQualityUpdate < QualityUpdateInterval) {
        return;
    }
    auto *network = d->session->networkDetection();
    const auto result = AdaptiveQuality::step({
        .current = d->quality,
        .cap = d->qualityCap,
        .goodputKbit = network->bandwidth(),
        .pixels = double(d->surfaceSize.width()) * double(d->surfaceSize.height()),
        .averageRtt = std::chrono::duration_cast<std::chrono::milliseconds>(network->averageRTT()),
        .minimumRtt = std::chrono::duration_cast<std::chrono::milliseconds>(network->minimumRTT()),
    });
    if (result.next == d->quality) {
        return;
    }
    d->lastQualityUpdate = now;
    d->quality = quint8(result.next);
    qCDebug(KRDP) << "Adaptive quality ->" << d->quality << "(target" << result.target << "cap" << d->qualityCap << "goodput" << network->bandwidth() << "kbit/s" << (result.congested ? ", congested" : "") << ")";
    Q_EMIT requestedQualityChanged(d->quality);
}
```
`d->surfaceSize` is whatever `Private` already stores as the current RDPGFX surface size (find the member `performReset()` writes; use that name). Connect in the constructor/initialize path, next to the existing `QoeFrameAcknowledge` wiring: `connect(d->session->networkDetection(), &NetworkDetection::bandwidthChanged, this, &VideoStream::updateAdaptiveQuality);` (`updateAdaptiveQuality` is a private slot; `#include "AdaptiveQuality.h"`). Thread note: `bandwidthChanged` is emitted from the FreeRDP peer thread where `VideoStream` also runs — direct connection is fine; the signal to the session crosses threads and is queued in `SessionController`.

- [ ] **Step 2: Metablock QP follows the actual quality**

Replace `RDPGFX_H264_QUANT_QUALITY quality = {22, 0, 100};` (≈ `VideoStream.cpp:622`) with:
```cpp
    // Informational for the client (MS-RDPEGFX 2.2.4.4.1), but keep it honest:
    // the same map the private KPipeWire uses (quality 100 -> QP 12, 0 -> QP 40).
    const quint8 qp = quint8(std::lround(40.0 - 0.28 * d->quality));
    RDPGFX_H264_QUANT_QUALITY quality = {qp, 0, d->quality};
```

- [ ] **Step 3: SessionController and config**

`server/SessionController.cpp`, next to the `keyFrameRequested` connection (≈ line 48):
```cpp
        connect(connection->videoStream(), &KRdp::VideoStream::requestedQualityChanged, this, &SessionWrapper::onRequestedQualityChanged, Qt::QueuedConnection);
```
with
```cpp
void SessionWrapper::onRequestedQualityChanged(quint8 quality)
{
    session->setVideoQuality(quality);
}
```
Where the controller currently applies `m_quality` to the session at connect (≈ lines 187/223), also call `connection->videoStream()->setQualityCap(quint8(m_quality.value()))` and `connection->videoStream()->setAdaptiveQuality(m_adaptiveQuality)`; add `void SessionController::setAdaptiveQuality(bool)` storing `m_adaptiveQuality` (default true) and applying to live wrappers. `krdpserversettings.kcfg`: `<entry name="AdaptiveQuality" type="Bool"><default>true</default></entry>` in the General group next to `Quality`. `server/main.cpp`: read it like `WakeDisplayOnConnect` (startup + `applyRuntimeConfig`), call `controller.setAdaptiveQuality(...)`, and append `adaptive=%N` to the startup summary.

- [ ] **Step 4: Build; unit tests still pass; harness unaffected**

```bash
cmake --build ~/dev/krdp/build -j16 2>&1 | tail -2 && ~/dev/krdp/build/bin/AdaptiveQualityTest | tail -1
cd /tmp/krdp-q4 && timeout 40 ~/dev/krdp/build/bin/krdpplasmastreamer --monitor 0 --quality 80 --quit-after 8 --wake-after 2 --output q4.raw > q4.log 2>&1; echo exit=$?; grep -E 'Total frames|fixed QP' q4.log
```
Expected: build OK, `6 passed`, harness exit 0 with `fixed QP = 18 / 18` (the harness has no RDP connection, so no adaptation happens — that is expected).

- [ ] **Step 5: Commit**

```bash
cd ~/dev/krdp && git add src/VideoStream.h src/VideoStream.cpp server/SessionController.cpp server/SessionController.h src/kcm/krdpserversettings.kcfg server/main.cpp
git commit -m "video: steer session quality from measured goodput and RTT

Quality is now the cap; with AdaptiveQuality=true (default) the video
stream moves the session's quality towards what the measured goodput can
carry at the surface resolution, stepping down quickly on congestion. The
RDPGFX metablock QP now reports the QP the encoder actually uses.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Deploy, live validation, docs, patch export

**Files:**
- Modify: `~/dev/krdp/research.md`, `~/dev/krdp/README.md`, `~/dev/rdp/CLAUDE.md`
- Create: re-export `~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/*.patch`

- [ ] **Step 1: Restart the service (only if no RDP client is connected) and verify**

```bash
ss -tnp | grep ':3389' | grep ESTAB || echo "no client"
systemctl --user restart app-org.kde.krdpserver.service; sleep 3; systemctl --user status app-org.kde.krdpserver.service --no-pager | head -6
journalctl --user -u app-org.kde.krdpserver --no-pager -n 20 | grep -E 'startup summary|Listening'
```
Expected: `… quality=80 … adaptive=1 …`.

- [ ] **Step 2: Live validation (Steve, from Windows)** — after a few minutes of use, `journalctl --user -u app-org.kde.krdpserver --no-pager --since -10min | grep -E 'Bandwidth measurement|Adaptive quality|Reopened h264_vaapi'` should show goodput samples, quality moves within [10, 80], and one `Reopened h264_vaapi` per move. If the loop oscillates (alternating up/down every 1.5 s), raise `QualityUpdateInterval` to 3 s and widen the congestion gate to 2× — record the observation in research.md either way.

- [ ] **Step 3: Patches and docs**

`git -C ~/dev/kpipewire format-patch v6.6.4..HEAD -o ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/` (five files now). `research.md`: `OPT-016 DONE 2026-09-16` with the mechanism and the live-validation numbers; `README.md` runtime settings: `AdaptiveQuality` (default true; `Quality` is the cap); `~/dev/rdp/CLAUDE.md`: mention `AdaptiveQuality` next to `Quality`.

- [ ] **Step 4: Commit**

```bash
cd ~/dev/krdp && git add research.md README.md && git commit -m "docs: OPT-016 adaptive quality status

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Self-review

- Spec coverage: OPT-016 "route quality into the encoder" → Task 1 (encoder actually changes) + Task 4 (steering); "consume QoE acks" — deferred: the fork's `gfxQoEFrameAcknowledge` stub stays; goodput+RTT is the first loop (research §5.6 accepts either); metablock honesty (review §2.3) → Task 4 step 2; §13's "test that `setQuality()` takes effect" → Task 1 step 1/4.
- Placeholder scan: `d->surfaceSize` is named "whatever Private stores" — acceptable because the implementer is told exactly how to find it (the member `performReset()` writes); everything else is concrete.
- Type consistency: `bandwidth()` returns `quint32` kbit/s everywhere; `Input.goodputKbit` is `quint32`; `requestedQualityChanged(quint8)` matches `setVideoQuality(quint8)`; `step()`/`Result` names identical in test and header.
