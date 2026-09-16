# OPT-015: Private KPipeWire + VA-API Encoder Configuration + Keyframe-on-Demand — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the encoder-side latency and quality defects KRDP inherits from stock KPipeWire 6.6.4 (`async_depth=2` held frame, libx264-only options, QP-1 quality map, 100-frame periodic IDR, no keyframe-on-demand) by building a private, patched KPipeWire that KRDP links against, and making KRDP request keyframes through a real API instead of restarting the encoder.

**Architecture:** KPipeWire v6.6.4 is cloned at `~/dev/kpipewire` and built into a private prefix `~/dev/krdp/.deps/kpipewire`; KRDP's build resolves `find_package(KPipeWire)` there and its build-tree RUNPATH makes `krdpserver` load the private libraries — the system `libkpipewire6` is untouched. KPipeWire gains `PipeWireBaseEncodedStream::requestKeyFrame()` (stream → produce thread → `Encoder` atomic → `AVFrame::pict_type = AV_PICTURE_TYPE_I` at `avcodec_send_frame`, which FFmpeg's hw_base_encode and libx264 turn into an IDR). The `h264_vaapi` encoder is reconfigured (async_depth 1, explicit CQP, QP map 40→12, long GOP, IDR at P-frame QP, no libx264 options, frame-repeat off). KRDP keeps compiling against stock KPipeWire via the fork's existing `if constexpr (requires …)` pattern.

**Tech Stack:** C++20/Qt 6.10/KF6 6.24, CMake ≥ 3.16 + ECM, FFmpeg 8.0.1 (`h264_vaapi`, hw_base_encode), Mesa 26.0.8 radeonsi VA-API on the Radeon 780M, PipeWire 1.6.2, KWin 6.6.6 `zkde_screencast_unstable_v1`.

**Spec:** `~/dev/rdp/RDP_QUALITY_RESEARCH.md` §5.1 (OPT-015), §5.3, §5.6, §7 row OPT-015/OPT-023; `~/dev/rdp/FORK_PERFORMANCE_REVIEW.md` §10 rows OPT-015/OPT-023/OPT-035, §12, §13 ("'no periodic IDR' must not land before keyframe-on-demand"). Both documents travel with this plan.

## Global Constraints

- No `sudo`, no package installs, nothing installed under `/usr` or `/usr/local`; the private prefix is `$HOME/dev/krdp/.deps/kpipewire`.
- KRDP must still configure and compile against stock KPipeWire 6.6.4 (`/usr/lib/x86_64-linux-gnu/cmake/KPipeWire`); every use of a private-only API goes through `if constexpr (requires …)` like `setFullColorRangeIfSupported()` in `src/PlasmaScreencastV1Session.cpp:156-165`.
- KPipeWire changes are commits on branch `westers/opt-015` in `~/dev/kpipewire` (off tag `v6.6.4`), exported as patches into `~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/`.
- Before `systemctl --user restart app-org.kde.krdpserver.service` or `kscreen-doctor --dpms off`: `ss -tnp | grep ':3389' | grep ESTAB` must be empty. If a client is connected, skip that step, finish everything else, and report.
- Every commit message ends with the trailer line `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. No amends, no rebases, no pushes, no `git stash`.
- Harness environment for every run: `export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 QT_QPA_PLATFORM=wayland LIBVA_DRIVER_NAME=radeonsi QT_LOGGING_RULES="org.kde.krdp.debug=true;kpipewire*.debug=true"`. Harness binary: `~/dev/krdp/build/bin/krdpplasmastreamer` (KWin grants it the screencast protocol via `~/.local/share/applications/org.kde.krdpplasmastreamer.desktop`; the `Exec=` there must equal the binary path). Run it from a scratch directory; it writes `--output` there.
- Nothing in KRDP's `src/VideoStream.cpp` queue/caps logic (commits 4b6c7bf, 0ed92d5) is changed by this plan.

---

### Task 1: Private KPipeWire build and KRDP relink

**Files:**
- Create: `~/dev/krdp/scripts/build-kpipewire.sh`
- Create: `~/dev/krdp/scripts/check-kpipewire-link.sh`
- Modify: `~/dev/krdp/.gitignore` (add `.deps/`)
- Modify: `~/dev/krdp/README.md` (new subsection "Private KPipeWire" under the build/setup section)
- Modify: `~/dev/krdp/build/` (reconfigure only; not a tracked file)

**Interfaces:**
- Consumes: nothing.
- Produces: `~/dev/krdp/.deps/kpipewire/{include/KPipeWire,lib*/cmake/KPipeWire,lib*/libKPipeWire*.so.6}`; `scripts/build-kpipewire.sh` (idempotent: configure+build+install KPipeWire, then reconfigure+build KRDP); `scripts/check-kpipewire-link.sh` (exit 0 iff `build/bin/krdpserver` resolves all `libKPipeWire*` to the private prefix). Later tasks call `scripts/build-kpipewire.sh` after every KPipeWire edit.

- [ ] **Step 1: Create the KPipeWire branch**

```bash
cd ~/dev/kpipewire && git checkout -b westers/opt-015 v6.6.4 && git log --oneline -1
```
Expected: `HEAD` at the v6.6.4 commit on branch `westers/opt-015`.

- [ ] **Step 2: Write the failing link check**

`~/dev/krdp/scripts/check-kpipewire-link.sh`:
```bash
#!/usr/bin/env bash
# Exit 0 iff krdpserver (and the harness) resolve every libKPipeWire* to the private prefix.
set -euo pipefail
prefix="${KRDP_KPIPEWIRE_PREFIX:-$HOME/dev/krdp/.deps/kpipewire}"
status=0
for bin in "$HOME/dev/krdp/build/bin/krdpserver" "$HOME/dev/krdp/build/bin/krdpplasmastreamer"; do
    [ -x "$bin" ] || { echo "missing: $bin"; status=1; continue; }
    while read -r line; do
        case "$line" in
            *"$prefix"*) ;;
            *) echo "NOT PRIVATE: $bin -> $line"; status=1 ;;
        esac
    done < <(ldd "$bin" | grep -E 'libKPipeWire(Record|DmaBuf)?\.so' || true)
done
[ $status -eq 0 ] && echo "OK: all KPipeWire libraries resolve to $prefix"
exit $status
```
`chmod +x ~/dev/krdp/scripts/check-kpipewire-link.sh`

- [ ] **Step 3: Run it to verify it fails**

Run: `~/dev/krdp/scripts/check-kpipewire-link.sh`
Expected: lines like `NOT PRIVATE: … libKPipeWire.so.6 => /usr/lib/x86_64-linux-gnu/libKPipeWire.so.6 …`, exit 1.

- [ ] **Step 4: Write the build script**

`~/dev/krdp/scripts/build-kpipewire.sh`:
```bash
#!/usr/bin/env bash
# Build the patched KPipeWire (~/dev/kpipewire, branch westers/opt-015) into a private prefix
# and relink KRDP against it. System libkpipewire6 is never touched.
set -euo pipefail
src="${KPIPEWIRE_SRC:-$HOME/dev/kpipewire}"
prefix="${KRDP_KPIPEWIRE_PREFIX:-$HOME/dev/krdp/.deps/kpipewire}"
krdp="${KRDP_SRC:-$HOME/dev/krdp}"
jobs="${JOBS:-16}"

cmake -S "$src" -B "$src/build" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF \
    -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF
cmake --build "$src/build" -j"$jobs"
cmake --install "$src/build"

config_dir="$(dirname "$(find "$prefix" -name KPipeWireConfig.cmake | head -1)")"
[ -n "$config_dir" ] || { echo "KPipeWireConfig.cmake not found under $prefix"; exit 1; }

cmake -S "$krdp" -B "$krdp/build" \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DKPipeWire_DIR="$config_dir"
cmake --build "$krdp/build" -j"$jobs"
"$krdp/scripts/check-kpipewire-link.sh"
```
`chmod +x ~/dev/krdp/scripts/build-kpipewire.sh`. Add a line `.deps/` to `~/dev/krdp/.gitignore`.

- [ ] **Step 5: Run the build script**

Run: `~/dev/krdp/scripts/build-kpipewire.sh 2>&1 | tail -25`
Expected: KPipeWire configures (all deps found — they are installed), builds, installs into the prefix (note the lib dir name it chose: `lib` or `lib/x86_64-linux-gnu`); KRDP reconfigures with `KPipeWire_DIR` under the prefix and builds; last line `OK: all KPipeWire libraries resolve to …/.deps/kpipewire`.
If `cmake --install` fails on the QML plugin path, confirm `-DKDE_INSTALL_USE_QT_SYS_PATHS=OFF` took effect (`grep KDE_INSTALL_USE_QT_SYS_PATHS ~/dev/kpipewire/build/CMakeCache.txt`); if the KRDP configure keeps `/usr/lib/x86_64-linux-gnu/cmake/KPipeWire`, delete `KPipeWire_DIR` from `~/dev/krdp/build/CMakeCache.txt` and re-run.

- [ ] **Step 6: Verify the RUNPATH and the behaviour is unchanged**

```bash
readelf -d ~/dev/krdp/build/bin/krdpserver | grep -E 'RUNPATH|RPATH'
mkdir -p /tmp/krdp-t1 && cd /tmp/krdp-t1 && timeout 40 ~/dev/krdp/build/bin/krdpplasmastreamer --monitor 0 --quality 80 --quit-after 8 --wake-after 2 --output t1.raw > t1.log 2>&1; echo exit=$?
grep -E 'Total frames|Key frames|Mesa Gallium driver|RC mode|async' t1.log
```
Expected: RUNPATH contains the private lib dir; exit 0; frames > 0 after the +2 s wake; log still shows `radeonsi … renderD128` and (unpatched) `RC mode: CQP / fixed QP = 17` for quality 80 (`max(1, 87 − 0.87·80)`). This is the baseline for Task 3's comparison — keep `t1.log`.

- [ ] **Step 7: Document and commit (KRDP)**

Add to `~/dev/krdp/README.md`, next to the existing build instructions:
```markdown
### Private KPipeWire

KRDP links against a patched KPipeWire built into `.deps/kpipewire` (source: `~/dev/kpipewire`,
branch `westers/opt-015`; patches exported to `~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/`).
`scripts/build-kpipewire.sh` builds/installs it and relinks KRDP; `scripts/check-kpipewire-link.sh`
verifies `build/bin/krdpserver` resolves `libKPipeWire*` there. Re-run the build script after any
KPipeWire edit or after an apt upgrade of Qt/KF6/FFmpeg/PipeWire. To go back to the system
library, reconfigure with `-UKPipeWire_DIR -DCMAKE_PREFIX_PATH=`.
```
```bash
cd ~/dev/krdp && git add scripts/build-kpipewire.sh scripts/check-kpipewire-link.sh .gitignore README.md
git commit -m "build: link against a private KPipeWire prefix (.deps/kpipewire)

KPipeWire 6.6.4 from ~/dev/kpipewire (branch westers/opt-015) is built into
.deps/kpipewire and KRDP is configured with KPipeWire_DIR pointing there, so
encoder changes can be made without touching the system libkpipewire6. The
build-tree RUNPATH makes build/bin/krdpserver load the private libraries;
scripts/check-kpipewire-link.sh asserts that.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: KPipeWire — `requestKeyFrame()` API

**Files:**
- Modify: `~/dev/kpipewire/src/pipewirebaseencodedstream.h` (public section, after `void setColorRange(ColorRange colorRange);` ≈ line 76 of the class body)
- Modify: `~/dev/kpipewire/src/pipewirebaseencodedstream.cpp` (after `PipeWireBaseEncodedStream::setQuality`)
- Modify: `~/dev/kpipewire/src/pipewireproduce_p.h` (after `void setQuality(const std::optional<quint8> &quality);`)
- Modify: `~/dev/kpipewire/src/pipewireproduce.cpp` (after `PipeWireProduce::setQuality`, ≈ line 277)
- Modify: `~/dev/kpipewire/src/encoder_p.h` (public: after `void setQuality(std::optional<quint8> quality);`; protected: after `std::optional<quint8> m_quality;`)
- Modify: `~/dev/kpipewire/src/encoder.cpp` (`Encoder::encodeFrame`, before `avcodec_send_frame`, ≈ line 100; new `Encoder::requestKeyFrame` after `Encoder::setQuality`, ≈ line 175)

**Interfaces:**
- Consumes: nothing new.
- Produces: `void PipeWireBaseEncodedStream::requestKeyFrame()` (public, `Q_INVOKABLE`, thread-safe to call from any thread; no-op when the stream is not running); `void PipeWireProduce::requestKeyFrame()`; `void Encoder::requestKeyFrame()` + `std::atomic_bool Encoder::m_keyFrameRequested`. Task 4's KRDP code detects the method with `requires(Stream *s) { s->requestKeyFrame(); }`.

- [ ] **Step 1: Write the failing symbol test**

```bash
cat > /tmp/krdp-t2-check.sh <<'EOF'
#!/usr/bin/env bash
lib=$(find "$HOME/dev/krdp/.deps/kpipewire" -name 'libKPipeWire.so.6' | head -1)
nm -D --defined-only "$lib" | c++filt | grep -q 'PipeWireBaseEncodedStream::requestKeyFrame()' && echo "OK: requestKeyFrame exported" || { echo "MISSING: requestKeyFrame"; exit 1; }
EOF
chmod +x /tmp/krdp-t2-check.sh && /tmp/krdp-t2-check.sh
```
Expected: `MISSING: requestKeyFrame`, exit 1.

- [ ] **Step 2: Add the API to the stream**

`pipewirebaseencodedstream.h`, public section, directly after `void setColorRange(ColorRange colorRange);`:
```cpp
    /**
     * Ask the encoder to make the next encoded picture a keyframe (IDR).
     *
     * Safe to call from any thread. It is a no-op while no encoder is running;
     * a freshly started encoder always begins with a keyframe anyway.
     */
    Q_INVOKABLE void requestKeyFrame();
```
`pipewirebaseencodedstream.cpp`, after `PipeWireBaseEncodedStream::setQuality`:
```cpp
void PipeWireBaseEncodedStream::requestKeyFrame()
{
    if (!d->m_produce) {
        return;
    }
    // m_produce lives on the produce thread; hop there instead of touching its encoder here.
    QMetaObject::invokeMethod(d->m_produce.get(), &PipeWireProduce::requestKeyFrame, Qt::QueuedConnection);
}
```

- [ ] **Step 3: Thread it through PipeWireProduce**

`pipewireproduce_p.h`, after `void setQuality(const std::optional<quint8> &quality);`:
```cpp
    void requestKeyFrame();
```
`pipewireproduce.cpp`, after `PipeWireProduce::setQuality`:
```cpp
void PipeWireProduce::requestKeyFrame()
{
    if (m_encoder) {
        m_encoder->requestKeyFrame();
    }
}
```

- [ ] **Step 4: Implement it in Encoder**

`encoder_p.h`: add `#include <atomic>` next to `#include <mutex>`; public, after `void setQuality(std::optional<quint8> quality);`:
```cpp
    /**
     * Make the next picture sent to libavcodec a keyframe. Thread-safe.
     */
    void requestKeyFrame();
```
protected, after `std::optional<quint8> m_quality;`:
```cpp
    std::atomic_bool m_keyFrameRequested = false;
```
`encoder.cpp`, after `Encoder::setQuality`:
```cpp
void Encoder::requestKeyFrame()
{
    m_keyFrameRequested = true;
}
```
`encoder.cpp`, in `Encoder::encodeFrame`, replace
```cpp
        if (queued + 1 < maximumFrames) {
            auto ret = -1;
            {
                std::lock_guard guard(m_avCodecMutex);
                ret = avcodec_send_frame(m_avCodecContext, frame);
            }
```
with
```cpp
        if (queued + 1 < maximumFrames) {
            if (m_keyFrameRequested.exchange(false)) {
                // libavcodec's hardware encoders (hw_base_encode) and libx264 turn an
                // I-typed input picture into an IDR.
                frame->pict_type = AV_PICTURE_TYPE_I;
#ifdef AV_FRAME_FLAG_KEY
                frame->flags |= AV_FRAME_FLAG_KEY;
#endif
            }
            auto ret = -1;
            {
                std::lock_guard guard(m_avCodecMutex);
                ret = avcodec_send_frame(m_avCodecContext, frame);
            }
```

- [ ] **Step 5: Build, install, run the symbol test**

Run: `~/dev/krdp/scripts/build-kpipewire.sh 2>&1 | tail -5 && /tmp/krdp-t2-check.sh`
Expected: build OK, link check OK, `OK: requestKeyFrame exported`.

- [ ] **Step 6: Commit (KPipeWire)**

```bash
cd ~/dev/kpipewire && git add src/pipewirebaseencodedstream.h src/pipewirebaseencodedstream.cpp src/pipewireproduce_p.h src/pipewireproduce.cpp src/encoder_p.h src/encoder.cpp
git commit -m "Add PipeWireBaseEncodedStream::requestKeyFrame()

Remote-desktop consumers re-create their client-side surface on
reconfiguration and need an IDR immediately rather than after the next GOP.
The request hops to the produce thread and sets an atomic the encoder
consumes right before avcodec_send_frame(), marking the picture
AV_PICTURE_TYPE_I, which hw_base_encode and libx264 promote to an IDR.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: KPipeWire — OPT-015 `h264_vaapi` configuration

**Files:**
- Modify: `~/dev/kpipewire/src/h264vaapiencoder.cpp` (`initialize()` lines with `gop_size`/`global_quality` ≈ 134-142; `percentageToAbsoluteQuality` ≈ 169-178; `buildEncodingOptions` ≈ 180-190)
- Modify: `~/dev/kpipewire/src/pipewireproduce.cpp` (`setupStream()`, after `m_encoder = makeEncoder(); if (!m_encoder) {…}` ≈ line 150-154)

**Interfaces:**
- Consumes: `Encoder::requestKeyFrame()` from Task 2 (the long GOP depends on it).
- Produces: quality→QP mapping `QP = round(40 − 0.28·quality)` (100→12, 80→18, 50→26, 0→40); `async_depth=1`; `rc_mode=CQP`; `gop_size=600`; IDR QP = P QP; no frame-repeat for `h264_vaapi`. Task 5 documents these numbers.

- [ ] **Step 1: Record the failing observation**

Run (from `/tmp/krdp-t1`, Task 1's baseline log): `grep -E 'RC mode|fixed QP|async_depth|preset|tune' t1.log | head`
Expected (stock behaviour): `RC mode: CQP / fixed QP = 17` at quality 80, and the option log line lists `preset`/`tune` entries that `h264_vaapi` ignores. Also: `grep -c 'Frame ' t1.log` vs the wake time shows the first frame ~100 ms after the wake; note the number for comparison.

- [ ] **Step 2: Reconfigure the codec context**

In `h264vaapiencoder.cpp` `initialize()`, replace
```cpp
    m_avCodecContext->max_b_frames = 0;
    m_avCodecContext->gop_size = 100;
```
with
```cpp
    m_avCodecContext->max_b_frames = 0;
    // Keyframes are requested on demand (Encoder::requestKeyFrame); the periodic
    // IDR only remains as a safety net for a lost one (10 s at 60 fps).
    m_avCodecContext->gop_size = 600;
    // Keep IDR pictures at the same QP as P pictures: libavcodec's default
    // i_quant_factor (1.25) makes every keyframe visibly softer than its neighbours,
    // which on a static desktop is the picture the user looks at the longest.
    m_avCodecContext->i_quant_factor = 1.0;
    m_avCodecContext->i_quant_offset = 0.0;
```
and replace
```cpp
    if (m_quality) {
        m_avCodecContext->global_quality = percentageToAbsoluteQuality(m_quality);
    } else {
        m_avCodecContext->global_quality = 35;
    }
```
with
```cpp
    // Constant QP; rc_mode=CQP is set explicitly in buildEncodingOptions().
    m_avCodecContext->global_quality = percentageToAbsoluteQuality(m_quality.value_or(70));
```

- [ ] **Step 3: Replace the quality map**

Replace `H264VAAPIEncoder::percentageToAbsoluteQuality` with:
```cpp
int H264VAAPIEncoder::percentageToAbsoluteQuality(const std::optional<quint8> &quality)
{
    if (!quality) {
        return -1;
    }
    // Map 0..100 linearly onto CQP 40..12. QP 12 is visually lossless for
    // desktop content; at QP 40 small text stops being readable. The previous
    // map reached QP 1 at 100 (a near-lossless bitstream) and QP 44 at 50.
    constexpr int MaxQp = 40;
    constexpr int MinQp = 12;
    const double q = std::clamp<int>(quality.value(), 0, 100) / 100.0;
    return int(std::lround(MaxQp - q * (MaxQp - MinQp)));
}
```
Add `#include <algorithm>` and `#include <cmath>` to the includes at the top of the file.

- [ ] **Step 4: Replace the encoding options**

Replace `H264VAAPIEncoder::buildEncodingOptions` with:
```cpp
AVDictionary *H264VAAPIEncoder::buildEncodingOptions()
{
    // Deliberately not calling Encoder::buildEncodingOptions(): "preset", "tune"
    // and "threads" are libx264 options that h264_vaapi ignores, and the old
    // "+mv4"/"+loop" flags only mean something to mpeg4/h263.
    AVDictionary *options = nullptr;
    // Emit each picture as soon as it is encoded instead of holding one back for
    // pipelining: on a damage-driven desktop stream the held picture is a full
    // frame period of latency on every isolated update.
    av_dict_set_int(&options, "async_depth", 1, 0);
    // Constant QP from global_quality (see percentageToAbsoluteQuality).
    av_dict_set(&options, "rc_mode", "CQP", 0);
    return options;
}
```

- [ ] **Step 5: Switch frame-repeat off for h264_vaapi**

In `pipewireproduce.cpp` `setupStream()`, directly after
```cpp
    m_encoder = makeEncoder();
    if (!m_encoder) {
        qCWarning(PIPEWIRERECORD_LOGGING) << "No encoder could be created";
        return;
    }
```
add
```cpp
    // h264_vaapi runs with async_depth=1 (see H264VAAPIEncoder), so it never holds a
    // picture back; re-sending the last frame after a quiet period would only cost
    // bandwidth. The repeat timer stays on for the software encoders.
    if (m_encoderType == PipeWireBaseEncodedStream::H264Baseline || m_encoderType == PipeWireBaseEncodedStream::H264Main) {
        if (auto *codecContext = m_encoder->avCodecContext(); codecContext && codecContext->codec && qstrcmp(codecContext->codec->name, "h264_vaapi") == 0) {
            m_enableFrameRepeat = false;
        }
    }
```
Check where `m_encoder->initialize(size)` is called (it is inside `setupFormat()` → confirm with `grep -n 'initialize(' pipewireproduce.cpp`); `avCodecContext()` is only valid after that, so if `initialize()` runs later than this point, move the block to directly after the `setupFormat()` success check instead. State which placement you used.

- [ ] **Step 6: Build, install, relink, and measure**

```bash
~/dev/krdp/scripts/build-kpipewire.sh 2>&1 | tail -5
mkdir -p /tmp/krdp-t3 && cd /tmp/krdp-t3 && timeout 40 ~/dev/krdp/build/bin/krdpplasmastreamer --monitor 0 --quality 80 --quit-after 8 --wake-after 2 --output t3.raw > t3.log 2>&1; echo exit=$?
grep -E 'RC mode|fixed QP|async_depth|preset|tune|Total frames|Key frames' t3.log
grep -E 'Injecting mouse move|^Frame 0 ' t3.log
ffprobe -hide_banner -f h264 -i t3.raw 2>&1 | grep Stream
```
Expected: `RC mode: CQP / fixed QP = 18` (quality 80 → QP 18), no `preset`/`tune` in the option log, frames > 0, the first frame's offset minus the wake offset ≤ the Task 1 baseline (no held-frame period), valid `h264 (Main), yuv420p, 2560x1440`. Run the same command with `--quality 100` and `--quality 50` once each and confirm `fixed QP = 12` and `= 26`.

- [ ] **Step 7: Commit (KPipeWire)**

```bash
cd ~/dev/kpipewire && git add src/h264vaapiencoder.cpp src/pipewireproduce.cpp
git commit -m "h264vaapi: low-latency configuration for desktop streaming

- async_depth=1: emit each picture immediately instead of holding one back
- explicit rc_mode=CQP with a quality map of 0..100 -> QP 40..12 (the old
  map produced QP 1 at 100 and QP 44 at 50)
- IDR pictures at the same QP as P pictures (i_quant_factor 1.0)
- gop_size 100 -> 600 now that keyframes can be requested on demand
- drop preset/tune/threads/+mv4/+loop, which h264_vaapi ignores
- no frame-repeat timer for h264_vaapi (nothing is held back any more)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: KRDP — request keyframes through the API; harness option

**Files:**
- Modify: `~/dev/krdp/src/PlasmaScreencastV1Session.cpp` (template helpers next to `setFullColorRangeIfSupported` ≈ line 156; `PlasmaScreencastV1Session::requestKeyFrame()` ≈ line 555)
- Modify: `~/dev/krdp/src/AbstractSession.h` (doc comment of `requestKeyFrame` ≈ line 53-61)
- Modify: `~/dev/krdp/examples/plasmastreamer/main.cpp` (options block ≈ line 31-38; timers after the `wake-after` block ≈ line 118-128)

**Interfaces:**
- Consumes: `PipeWireBaseEncodedStream::requestKeyFrame()` (Task 2) when present.
- Produces: `PlasmaScreencastV1Session::requestKeyFrame()` uses the API when the headers have it and falls back to the encoder restart otherwise; harness option `--keyframe-at <s[,s…]>`.

- [ ] **Step 1: Add the harness option (the failing test)**

In `examples/plasmastreamer/main.cpp` options block add:
```cpp
        {u"keyframe-at"_s, u"Call requestKeyFrame() on the session at these offsets (seconds, comma separated) after the stream started"_s, u"seconds"_s},
```
After the `wake-after` handling block add:
```cpp
    if (parser.isSet(u"keyframe-at"_s)) {
        const auto offsets = parser.value(u"keyframe-at"_s).split(u',', Qt::SkipEmptyParts);
        QObject::connect(&session, &KRdp::AbstractSession::started, &application, [&session, &sinceStarted, offsets]() {
            for (const auto &offset : offsets) {
                QTimer::singleShot(offset.toInt() * 1000, &session, [&session, &sinceStarted]() {
                    qInfo() << "Requesting keyframe at +" << sinceStarted.elapsed() << "ms";
                    session.requestKeyFrame();
                });
            }
        });
    }
```
(`sinceStarted` is the existing `QElapsedTimer` the wake-after block uses; keep the same capture style.)

- [ ] **Step 2: Build and run it to verify it fails (restart path)**

```bash
cmake --build ~/dev/krdp/build -j16 --target krdpplasmastreamer 2>&1 | tail -3
mkdir -p /tmp/krdp-t4 && cd /tmp/krdp-t4 && timeout 40 ~/dev/krdp/build/bin/krdpplasmastreamer --monitor 0 --quality 80 --quit-after 10 --wake-after 2 --keyframe-at 5,7 --output t4a.raw > t4a.log 2>&1; echo exit=$?
grep -E 'Requesting keyframe|Restarting encoded stream|keyframe true|Key frames' t4a.log
```
Expected (before the code change): two `Restarting encoded stream on node … to obtain a keyframe` lines — the old restart hack — and keyframes that arrive only after each restart.

- [ ] **Step 3: Use the API when available**

In `PlasmaScreencastV1Session.cpp`, after `setPreferredH264Encoder` (≈ line 175) add:
```cpp
template<typename Stream>
bool requestKeyFrameIfSupported(Stream *stream)
{
    if constexpr (requires(Stream *s) { s->requestKeyFrame(); }) {
        stream->requestKeyFrame();
        return true;
    } else {
        return false;
    }
}
```
Replace the body of `PlasmaScreencastV1Session::requestKeyFrame()` with:
```cpp
void PlasmaScreencastV1Session::requestKeyFrame()
{
    auto encodedStream = stream();
    const uint nodeId = encodedStream->nodeId();
    if (!d->streamConfigured || nodeId == 0 || !streamingRequested()) {
        return;
    }
    if (requestKeyFrameIfSupported(encodedStream)) {
        qCDebug(KRDP) << "Requested a keyframe from the encoder for the new surface";
        return;
    }
    // Stock KPipeWire 6.6 cannot be asked for an IDR mid-stream, but a restarted
    // encoded stream always opens with one. Re-attach the same PipeWire node
    // through the deferred restart (KWin keeps the screencast source alive;
    // only the KPipeWire consumer/encoder is recreated).
    if (d->streamRestartTimer.isActive()) {
        // A restart is already in flight; it will deliver a keyframe.
        return;
    }
    qCDebug(KRDP) << "Restarting encoded stream on node" << nodeId << "to obtain a keyframe for the new surface";
    restartEncodedStream(nodeId);
}
```
Update the `AbstractSession.h` comment on `requestKeyFrame()` to: "Sessions ask the encoder for a keyframe through `PipeWireBaseEncodedStream::requestKeyFrame()` when the linked KPipeWire has it, and otherwise restart their encoded stream (which always opens with an IDR). The default implementation only logs."

- [ ] **Step 4: Build and run to verify it passes**

```bash
cmake --build ~/dev/krdp/build -j16 2>&1 | tail -3
cd /tmp/krdp-t4 && timeout 40 ~/dev/krdp/build/bin/krdpplasmastreamer --monitor 0 --quality 80 --quit-after 10 --wake-after 2 --keyframe-at 5,7 --output t4b.raw > t4b.log 2>&1; echo exit=$?
grep -E 'Requesting keyframe|Requested a keyframe|Restarting encoded stream|keyframe true|Key frames' t4b.log
```
Expected: two `Requested a keyframe from the encoder` lines, NO `Restarting encoded stream`, a `keyframe true` frame within ~100 ms after each request (the harness needs damage for a frame to exist — the cursor moves from `--wake-after` provide some; if the desktop is fully static and no frame follows a request, re-run with `--wake-after 2` replaced by injecting moves at 2, 5.2 and 7.2 s, i.e. extend `--wake-after` to accept a comma list the same way as `--keyframe-at`), `Key frames: 3`.

- [ ] **Step 5: Prove the stock-KPipeWire build still compiles**

```bash
cmake -S ~/dev/krdp -B /tmp/krdp-stock-build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DKPipeWire_DIR=/usr/lib/x86_64-linux-gnu/cmake/KPipeWire >/dev/null && cmake --build /tmp/krdp-stock-build -j16 --target krdpplasmastreamer 2>&1 | tail -2
ldd /tmp/krdp-stock-build/bin/krdpplasmastreamer | grep libKPipeWire.so
```
Expected: builds; links `/usr/lib/x86_64-linux-gnu/libKPipeWire.so.6` (the `requires` check compiled the restart fallback). Delete `/tmp/krdp-stock-build` afterwards.

- [ ] **Step 6: Commit (KRDP)**

```bash
cd ~/dev/krdp && git add src/PlasmaScreencastV1Session.cpp src/AbstractSession.h examples/plasmastreamer/main.cpp
git commit -m "session: request keyframes through KPipeWire when the API exists

With the private KPipeWire's PipeWireBaseEncodedStream::requestKeyFrame()
the surface-reset path asks the running encoder for an IDR instead of
restarting the encoded stream; stock KPipeWire keeps the restart fallback
via the same if-constexpr detection used for setColorRange. The harness
gains --keyframe-at to exercise it.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Deploy, DPMS-off regression, docs, patch export

**Files:**
- Modify: `~/dev/krdp/research.md` (Status Updates: OPT-015 DONE, OPT-023 PARTIAL (frame-repeat off; QoE-driven pacing still open), OPT-035 DONE-via-API)
- Modify: `~/dev/krdp/README.md` (runtime settings: `Quality` → QP table)
- Modify: `~/dev/rdp/CLAUDE.md` ("What actually runs": KPipeWire is now the private build; rebuild command)
- Create: `~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/0001-*.patch`, `0002-*.patch`

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces: the deployed service on the private KPipeWire; exported patches.

- [ ] **Step 1: Regression — DPMS-off → wake with the new encoder (only if no RDP client is connected)**

```bash
ss -tnp | grep ':3389' | grep ESTAB || echo "no client"
kscreen-doctor --dpms off; sleep 5; kscreen-doctor --dpms show
mkdir -p /tmp/krdp-t5 && cd /tmp/krdp-t5 && timeout 60 ~/dev/krdp/build/bin/krdpplasmastreamer --monitor 0 --quality 80 --quit-after 25 --wake-after 8 --output t5.raw > t5.log 2>&1; echo exit=$?
grep -E 'Total frames|Key frames|attempt [0-9]+ of 24|Using output stream|Using workspace stream|falling back|Restarting encoded|RC mode' t5.log
kscreen-doctor --dpms on
```
Expected: recovery lands on `Using output stream index 0 screen "DP-1" logical rect QRect(0,0 2560x1440)` with no workspace fallback, `Restarting encoded stream after display reconfiguration` (that path still restarts — the node changes), `RC mode: CQP / fixed QP = 18`, frames through the end.

- [ ] **Step 2: Restart the service (only if no RDP client is connected) and verify**

```bash
systemctl --user restart app-org.kde.krdpserver.service; sleep 3
systemctl --user status app-org.kde.krdpserver.service --no-pager | head -8
pid=$(systemctl --user show -p MainPID --value app-org.kde.krdpserver.service); grep -o '/home/westers/dev/krdp/.deps/kpipewire[^ ]*libKPipeWire[^ ]*' /proc/$pid/maps | sort -u
journalctl --user -u app-org.kde.krdpserver --no-pager -n 20 | grep -E 'startup summary|Listening'
```
Expected: active; `/proc/<pid>/maps` lists the private `libKPipeWire*.so.6`; startup summary with `quality=80`.

- [ ] **Step 3: Export the KPipeWire patches**

```bash
mkdir -p ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4 && git -C ~/dev/kpipewire format-patch v6.6.4..HEAD -o ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/ && ls ~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/
```
Expected: two patch files (Task 2, Task 3).

- [ ] **Step 4: Docs**

`~/dev/krdp/research.md` Status Updates — append (keep the file's format):
```
- 2026-09-16 OPT-015 DONE: private KPipeWire (~/dev/kpipewire westers/opt-015) — h264_vaapi async_depth=1, rc_mode=CQP, quality→QP 40..12, gop 600, IDR QP = P QP, libx264-only options dropped, frame-repeat off for h264_vaapi.
- 2026-09-16 OPT-023 PARTIAL: frame-repeat disabled for h264_vaapi; QoE-driven pacing remains (OPT-016).
- 2026-09-16 OPT-035 DONE via PipeWireBaseEncodedStream::requestKeyFrame(); the encoder-restart path remains only as the stock-KPipeWire fallback.
```
`~/dev/krdp/README.md` runtime settings: under `Quality`, add "maps to h264_vaapi CQP QP = 40 − 0.28·Quality (100→12, 80→18, 50→26) with the private KPipeWire; with stock KPipeWire the old map applies (100→QP 1)".
`~/dev/rdp/CLAUDE.md` "What actually runs": change the KPipeWire bullet to say KRDP now links the private build in `~/dev/krdp/.deps/kpipewire` (source `~/dev/kpipewire`, branch `westers/opt-015`; rebuild with `~/dev/krdp/scripts/build-kpipewire.sh`), and that stock `libkpipewire6 6.6.4` stays installed for other apps.

- [ ] **Step 5: Commit (KRDP) and report**

```bash
cd ~/dev/krdp && git add research.md README.md && git commit -m "docs: OPT-015 encoder configuration and keyframe-on-demand status

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```
Report: the QP/async lines from t3/t5 logs, the wake→first-frame delta before (t1) and after (t3), the keyframe-at result (t4b), the `/proc/<pid>/maps` line, and anything skipped because a client was connected. Steve's Windows test then judges perceived latency/quality at `Quality=80`.

---

## Self-review

- Spec coverage: OPT-015 (async_depth, rc_mode, QP map, IDR policy, libx264 options) → Task 3; keyframe-on-demand prerequisite (§13 caveat) → Task 2 before Task 3; OPT-023 frame-repeat → Task 3 step 5; OPT-035 via API → Task 4; private KPipeWire mechanism (research.md "private prefix" section) → Task 1; stock-compat constraint → Task 4 step 5. Not covered here by design: OPT-016 (quality steering into the encoder, needs a reopen strategy — next plan), `sei`/`slices` options (unmeasured benefit, deferred).
- Placeholder scan: none.
- Type consistency: `requestKeyFrame()` is the name on all three KPipeWire layers and in KRDP's template; `m_keyFrameRequested` is `std::atomic_bool`; `m_enableFrameRepeat` and `m_encoderType` are the existing `PipeWireProduce` members.
