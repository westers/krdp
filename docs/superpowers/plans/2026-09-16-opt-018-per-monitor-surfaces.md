# OPT-018: Per-Monitor Capture, Encode and RDPGFX Surfaces — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `MonitorMode=multi`, in which each enabled physical monitor is captured, encoded and sent as its own RDPGFX surface mapped at its own origin — so mstsc with `/multimon` shows each monitor as a separate remote monitor (and without `/multimon` shows the union as one desktop) — while every existing mode (`workspace`, `primary`, `specific`, virtual monitor) keeps working unchanged.

**Architecture:** One `PlasmaScreencastV1Session` per monitor (each already knows how to capture one output, encode it on the 780M under the 4096-px VA-API limit, recover from output churn, and request keyframes), all feeding one `VideoStream` per connection that now owns a vector of surfaces. `SessionController` computes the monitor layout once (KScreen order, primary normalised to `(0,0)`), hands each session its index and global geometry, and fans frames/cursor/keyframe/quality signals between the N sessions and the one stream. Pointer input arrives in the RDP desktop (union) space and is injected as KWin-global coordinates through one session's fake-input object. Hot-plug in v1 rebuilds the whole session set. This is the design gnome-remote-desktop's layout manager and xrdp 0.10 use; the fork's `VideoStream.cpp` is already modelled on g-r-d's graphics pipeline.

**Tech Stack:** C++20/Qt 6.10/KF6 6.24, FreeRDP 3.31 RDPGFX server API (`ResetGraphics` monitorDefArray, `CreateSurface`, `MapSurfaceToOutput`, `SurfaceCommand`, frame acks), KWin `zkde_screencast_unstable_v1` `stream_output`, `org_kde_kwin_fake_input` (global coordinates), private KPipeWire (one `PipeWireEncodedStream` per session), radeonsi VA-API.

**Spec:** `~/dev/rdp/RDP_QUALITY_RESEARCH.md` §4 (4.1 requirements, 4.2 mstsc negotiation, 4.3 recommended design, 4.4 rejected slicing, 4.5 encoder budget) and §7 row OPT-018; `~/dev/rdp/FORK_PERFORMANCE_REVIEW.md` §2.7 (multi-monitor ResetGraphics KEEP, unverified with `/multimon`), §10 row OPT-018. Prerequisites: Plans 1 and 2 complete (keyframe-on-demand, adaptive quality) — both deployed 2026-09-16.

## Global Constraints

- All existing modes keep their current behaviour byte-for-byte on the wire when `MonitorMode` is not `multi`: a single session, a single surface, `VideoFrame::monitorIndex == 0`.
- Coordinates: the RDP desktop space is the union of the monitors with the primary monitor's top-left at `(0,0)` (mstsc requires a monitor containing the origin). KWin-global coordinates are the `QScreen::geometry()` values. Conversion is a constant translation `rdp = kwin − primary.topLeft()`; keep it in exactly one helper.
- Each surface is `CreateSurface`d at its monitor's pixel size and `MapSurfaceToOutput`ed at its RDP-space origin; `ResetGraphics` advertises `monitorCount = N` with `monitorDefArray[i] = {left, top, right, bottom, flags = primary ? MONITOR_PRIMARY : 0}` in RDP space.
- Frame IDs, `pendingFrames`, acks and the adaptive-quality loop stay per connection; the quality applied to every session is the same value; `AdaptiveQuality::step()` uses the SUM of surface pixels.
- VA-API limit: never create a surface/stream wider or taller than 4096 px; a monitor that exceeds it makes `multi` fall back to `specific` for the primary with a warning (log + startup summary), never to the workspace stream.
- No `sudo`, no package installs; before `systemctl --user restart app-org.kde.krdpserver.service` or `kscreen-doctor --dpms off`, `ss -tnp | grep ':3389' | grep ESTAB` must be empty.
- Every commit message ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. No amends, rebases, pushes, or `git stash`. Commit in `~/dev/krdp` only (no KPipeWire change is expected; if one becomes necessary, stop and report).
- Harness environment: `export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 QT_QPA_PLATFORM=wayland LIBVA_DRIVER_NAME=radeonsi QT_LOGGING_RULES="org.kde.krdp.debug=true;kpipewire*.debug=true;kpipewire*.info=true"`; binary `~/dev/krdp/build/bin/krdpplasmastreamer` from a scratch directory. Monitors on this box: index 0 = DP-1 2560x1440 @ (0,0) primary; index 1 = HDMI-A-1 2560x1440 @ (2560,0).

---

### Task 1: Feasibility gate — two concurrent VA-API encoders (harness `--multi`)

**Files:**
- Modify: `~/dev/krdp/examples/plasmastreamer/main.cpp`

**Interfaces:**
- Produces: harness option `--multi` (capture every `QGuiApplication::screens()` output with one `PlasmaScreencastV1Session` each, writing `<output>.<index>.raw`, printing per-session frame/keyframe totals and each session's average frames per second over the run). Task 5 reuses it.

- [ ] **Step 1: Add the option**

In the options block: `{u"multi"_s, u"Capture every screen with its own session and encoder; writes <output>.<index>.raw"_s},`. After parsing, when `--multi` is set, replace the single `session` usage with a vector: for each `screen` in `qGuiApp->screens()` (index `i`), create `auto s = std::make_unique<KRdp::PlasmaScreencastV1Session>(); s->setActiveStream(i); if (quality) s->setVideoQuality(q); s->requestStreamingEnable(&application);` and connect `frameReceived` to a per-index `QFile` (`output + u"." + QString::number(i) + u".raw"`) plus per-index counters; connect each `started()` so the quit timer starts when ALL sessions have started; on quit, `requestStreamingDisable` each, then print `Session i: frames N keyframes K bytes B avg_fps F`. The existing single-session path must be untouched when `--multi` is absent (keep the code paths separate; a small `struct SessionRun { std::unique_ptr<KRdp::PlasmaScreencastV1Session> session; QFile file; int frames = 0; int keyframes = 0; qint64 bytes = 0; }` is enough).

- [ ] **Step 2: Build and run the gate**

```bash
cmake --build ~/dev/krdp/build -j16 --target krdpplasmastreamer 2>&1 | tail -2
mkdir -p /tmp/krdp-m1 && cd /tmp/krdp-m1 && timeout 60 ~/dev/krdp/build/bin/krdpplasmastreamer --multi --quality 80 --quit-after 20 --wake-after 2,6,10,14 --output m1.raw > m1.log 2>&1; echo exit=$?
grep -E 'Session [0-9]+:|Mesa Gallium driver|fixed QP|Failed|error|Error' m1.log | sort | uniq -c | sort -rn | head -20
for f in m1.raw.*.raw; do ffprobe -hide_banner -f h264 -i "$f" 2>&1 | grep Stream; done
```
Expected: two `Mesa Gallium driver … renderD128` lines (two encoders, both on the 780M), both sessions `Using output stream index N screen "…"`, both streams valid `h264 (Main), yuv420p, 2560x1440`, no VA-API errors, and — with the mouse moving on DP-1 only — session 0 at a real frame rate and session 1 mostly idle. Record `avg_fps` for both. GATE: if the second encoder fails to initialise, or either stream is invalid, STOP the plan and report (the design then needs a KPipeWire change).

- [ ] **Step 3: Commit**

```bash
cd ~/dev/krdp && git add examples/plasmastreamer/main.cpp
git commit -m "examples: plasmastreamer --multi runs one session per screen

Feasibility gate for per-monitor surfaces: two concurrent h264_vaapi
encoders on the Radeon 780M.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Session plumbing — monitor index, global geometry, global input

**Files:**
- Modify: `~/dev/krdp/src/VideoStream.h` (`VideoFrame`: add `int monitorIndex = 0;`)
- Modify: `~/dev/krdp/src/AbstractSession.h`, `~/dev/krdp/src/AbstractSession.cpp`
- Modify: `~/dev/krdp/src/PlasmaScreencastV1Session.h`, `~/dev/krdp/src/PlasmaScreencastV1Session.cpp`

**Interfaces:**
- Produces on `AbstractSession`: `void setMonitorIndex(int index)` / `int monitorIndex() const` (default 0; stamped into every emitted `VideoFrame::monitorIndex`); `virtual void sendGlobalEvent(const std::shared_ptr<QEvent> &event)` (pointer positions already in KWin-global coordinates; default implementation calls `sendEvent`); `QRect outputGeometry() const` (KWin-global geometry of the captured output, `d->logicalRect` for output streams).
- Consumes: nothing new.

- [ ] **Step 1: `VideoFrame::monitorIndex`**

In `src/VideoStream.h` add to `struct VideoFrame` after `monitors`: `/** Index of the surface this frame belongs to (0 unless MonitorMode=multi). */ int monitorIndex = 0;`.

- [ ] **Step 2: AbstractSession index + geometry**

`AbstractSession.h` (public): `void setMonitorIndex(int index); int monitorIndex() const; QRect outputGeometry() const; virtual void sendGlobalEvent(const std::shared_ptr<QEvent> &event);` with doc comments; `Private` gains `int monitorIndex = 0;`. `AbstractSession.cpp`: trivial setter/getter; `outputGeometry()` returns `d->logicalRect`-equivalent — the fork keeps the logical rect in `PlasmaScreencastV1Session::Private`, so implement `outputGeometry()` as `virtual` with the Plasma override returning `d->logicalRect` and the base returning `QRect(QPoint(0, 0), logicalSize())`; default `sendGlobalEvent` = `sendEvent(event)`.

- [ ] **Step 3: Stamp frames and implement global input in the Plasma session**

In `PlasmaScreencastV1Session::onPacketReceived()` (where the `VideoFrame` is built before `Q_EMIT frameReceived(frame)`), set `frame.monitorIndex = monitorIndex();`. Add `void sendGlobalEvent(const std::shared_ptr<QEvent> &event) override;` whose body is `sendEvent()`'s switch with ONE difference: for `QEvent::MouseMove` it skips the normalisation and calls `d->remoteInterface->pointer_motion_absolute(wl_fixed_from_double(position.x()), wl_fixed_from_double(position.y()))` directly (positions are KWin-global). Factor the shared button/wheel/key handling into a private `injectNonMotionEvent()` so the two functions do not duplicate it.

- [ ] **Step 4: Build; harness unchanged**

`cmake --build ~/dev/krdp/build -j16 2>&1 | tail -2`; run the harness once (`--monitor 0 --quality 80 --quit-after 8 --wake-after 2`) → `Total frames` > 0, `fixed QP = 18 / 18`. Add a line to the harness's per-frame log printing `frame.monitorIndex` and confirm it is 0.

- [ ] **Step 5: Commit**

```bash
cd ~/dev/krdp && git add src/VideoStream.h src/AbstractSession.h src/AbstractSession.cpp src/PlasmaScreencastV1Session.h src/PlasmaScreencastV1Session.cpp examples/plasmastreamer/main.cpp
git commit -m "session: monitor index on frames, output geometry, global-coordinate input

Plumbing for per-monitor surfaces; single-session behaviour unchanged
(monitorIndex defaults to 0).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: VideoStream — N surfaces, one per monitor

**Files:**
- Modify: `~/dev/krdp/src/VideoStream.h`, `~/dev/krdp/src/VideoStream.cpp`

**Interfaces:**
- Produces: `void VideoStream::setMonitorLayout(const QVector<VideoMonitor> &layout)` (RDP-space rects, index = surface index, exactly one `primary`; empty = derive from frames as today); `Q_SIGNAL void keyFrameRequested(int monitorIndex)` (replaces the parameterless signal; existing single-surface emits pass 0).
- Consumes: `VideoFrame::monitorIndex` (Task 2).

- [ ] **Step 1: Surface vector**

In `VideoStream::Private` replace `Surface surface;` with `QVector<Surface> surfaces;` (each `Surface{uint16_t id; QSize size; QPoint origin;}` — add `origin`), keep `std::atomic<qint64> surfacePixels` as the SUM over surfaces, and add `QVector<VideoMonitor> configuredLayout;` set by `setMonitorLayout()`. Every existing `d->surface` use becomes `surfaceFor(frame.monitorIndex)`; add `Surface *surfaceFor(int index)` returning `nullptr` when out of range (then log once and drop the frame).

- [ ] **Step 2: Reset creates all surfaces**

`performReset(const QVector<VideoMonitor> &layout)`: `ResetGraphics` with `monitorCount = layout.size()` and each monitor's RDP-space rect and primary flag (the fork's existing `monitorLayoutForReset()` / `monitorLayoutSummary()` helpers already produce this for the single-surface case — extend them to take the configured layout when set); then for each monitor `i`: `CreateSurface{surfaceId = nextSurfaceId++, width/height = rect size, pixelFormat = PIXEL_FORMAT_XRGB_8888}` and `MapSurfaceToOutput{surfaceId, outputOriginX = rect.x(), outputOriginY = rect.y()}`; push `Surface{id, size, origin}`. Reset condition in `sendFrame()` becomes: `d->pendingReset || layoutChanged || !surfaceFor(frame.monitorIndex) || surfaceFor(frame.monitorIndex)->size != frame.size` where `layoutChanged` compares the configured layout (or, when unset, the frame's `monitors` as today).

- [ ] **Step 3: Per-surface send and keyframe requests**

`sendFrame()`: `surfaceCommand.surfaceId = surface->id;` `left/top = 0`, `right/bottom = surface size` (each surface is a whole monitor); the `RDPGFX_START_FRAME`/`END_FRAME` bracket stays per frame. Where the reset path emits `keyFrameRequested()` on a non-keyframe, emit `keyFrameRequested(frame.monitorIndex)`; keep the 2 s rate limit PER surface (a `QVector<time_point> lastKeyFrameRequest` sized with the surfaces). `updateAdaptiveQuality()` unchanged (it reads the summed `surfacePixels`).

- [ ] **Step 4: Unit test for the layout helper**

Extract the pure part — "given a configured layout or a frame's monitor list, produce the `RDPGFX_RESET_GRAPHICS` monitor defs and the per-surface `(size, origin)` list" — into `src/SurfaceLayout.h` (`namespace KRdp::SurfaceLayout { struct Entry { QSize size; QPoint origin; bool primary; }; QVector<Entry> fromMonitors(const QVector<VideoMonitor> &monitors); }`) and add `autotests/SurfaceLayoutTest.cpp` with: one monitor at (0,0) → one entry; two monitors DP-1 (0,0,2560x1440 primary) + HDMI-A-1 (2560,0,2560x1440) → two entries with origins (0,0) and (2560,0); a layout whose primary is not at the origin (e.g. primary at (1920,0)) → entries translated so the primary origin is (0,0) and the other becomes negative-x (mstsc accepts negative coordinates as long as the primary contains (0,0)). Register it in `autotests/CMakeLists.txt` like `AdaptiveQualityTest`. RED first (header missing), then GREEN.

- [ ] **Step 5: Build, tests, harness**

`cmake --build ~/dev/krdp/build -j16`; `build/bin/AdaptiveQualityTest` and `build/bin/SurfaceLayoutTest` pass; harness single-monitor run unchanged (`fixed QP = 18 / 18`, frames flowing). `server/SessionController.cpp` must be updated in this task only as far as needed to compile against the new `keyFrameRequested(int)` signature (forward the index to `session->requestKeyFrame()` ignoring it for now — Task 4 routes it).

- [ ] **Step 6: Commit**

```bash
cd ~/dev/krdp && git add src/VideoStream.h src/VideoStream.cpp src/SurfaceLayout.h autotests/SurfaceLayoutTest.cpp autotests/CMakeLists.txt server/SessionController.cpp
git commit -m "video: one RDPGFX surface per monitor

VideoStream owns a vector of surfaces mapped at their RDP-space origins;
ResetGraphics advertises every monitor; keyframe requests carry the surface
index. Single-surface behaviour is unchanged when no layout is configured.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: SessionController — `MonitorMode=multi`

**Files:**
- Modify: `~/dev/krdp/server/SessionController.h`, `~/dev/krdp/server/SessionController.cpp`, `~/dev/krdp/server/main.cpp`
- Modify: `~/dev/krdp/src/kcm/krdpserversettings.kcfg` (document `multi` as a valid `MonitorMode` value if the kcfg enumerates them; it is a String today, so likely only the KCM list changes), `~/dev/krdp/src/kcm/ui/main.qml` (add `{text: i18nc("@item:inlistbox", "Each monitor separately"), value: "multi"}` to `monitorModeCombo`)

**Interfaces:**
- Consumes: Task 2 (`setMonitorIndex`, `sendGlobalEvent`, `outputGeometry`), Task 3 (`setMonitorLayout`, `keyFrameRequested(int)`).
- Produces: `MonitorMode=multi`; startup summary `stream=multi:N`; per-connection `SessionWrapper` holding `QVector<std::unique_ptr<KRdp::AbstractSession>> sessions` (size 1 in every other mode).

- [ ] **Step 1: Layout computation**

`normalizedMonitorMode()` in `main.cpp` accepts `multi`. Add to `SessionController` a `static QVector<KRdp::VideoMonitor> computeMultiLayout(QVector<QScreen *> &orderedScreens)`: take `qGuiApp->screens()` ordered as KRDP's monitor indices already are (the existing `kwinPrimaryOutputName()` / index logic in `main.cpp` identifies the primary — reuse it), drop screens whose geometry exceeds 4096 in either dimension (warn), translate all geometries by `−primary.topLeft()`, mark the primary. If fewer than 2 screens remain, fall back to `specific` with the primary index and log `MonitorMode=multi needs two usable monitors; using specific`.

- [ ] **Step 2: N sessions per connection**

In `SessionWrapper` (multi mode): create one `makeSession()` per screen with `setActiveStream(i)` and `setMonitorIndex(i)`; connect every session's `frameReceived` → `videoStream()->queueFrame`, `cursorUpdate` → `onCursorUpdate` (translate the cursor position by the session's `outputGeometry().topLeft() − primary.topLeft()` before forwarding), `started()`/`error()` as today; call `videoStream()->setMonitorLayout(layout)` before enabling; route `keyFrameRequested(int i)` → `sessions[i]->requestKeyFrame()`, `requestedQualityChanged` and `onRequestedFrameRateChanged` → every session; `requestStreamingEnable/Disable` → every session; the display-wake guard acquire/release once per wrapper as today. Input: `RdpConnection`'s input events currently go to `session->sendEvent`; in multi mode translate pointer positions from RDP space to KWin-global (`+ primary.topLeft()`) and call `sessions[0]->sendGlobalEvent(event)` (fake input is global; one session suffices). Keep the single-session code path literally as it is for the other modes (`sessions.size() == 1`, index 0).

- [ ] **Step 3: Hot-plug (v1)**

On `qGuiApp->screenAdded/screenRemoved` in multi mode, `main.cpp`'s existing debounced topology handler calls a new `SessionController::rebuildMultiSessions()` that, for each live wrapper, recomputes the layout, tears the sessions down and recreates them (`videoStream()->setMonitorLayout(newLayout)` first so the next frame performs a full reset). Log `Rebuilt N monitor sessions after topology change`.

- [ ] **Step 4: Startup summary + KCM**

`stream=multi:N` (N usable monitors) in the startup summary; the KCM combo gains the "Each monitor separately" entry; README runtime-settings table gains `MonitorMode=multi`.

- [ ] **Step 5: Build and harness**

Build; `AdaptiveQualityTest` + `SurfaceLayoutTest` pass; single-monitor harness unchanged. The server-side path can only be exercised by an RDP client (Task 5).

- [ ] **Step 6: Commit**

```bash
cd ~/dev/krdp && git add server/SessionController.h server/SessionController.cpp server/main.cpp src/kcm/krdpserversettings.kcfg src/kcm/ui/main.qml README.md
git commit -m "server: MonitorMode=multi — one session and surface per monitor

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Deploy, mstsc `/multimon` validation, docs

**Files:**
- Modify: `~/dev/krdp/research.md`, `~/dev/krdp/README.md`, `~/dev/rdp/CLAUDE.md`

- [ ] **Step 1: Switch the config and restart (no client connected)**

`kwriteconfig6 --file krdpserverrc --group General --key MonitorMode multi`; `systemctl --user restart …` after the `ss` check; journal shows `stream=multi:2`.

- [ ] **Step 2: Steve's tests (from Windows)**

(a) `mstsc /v:hal9000 /multimon` — expect two remote monitors matching DP-1 and HDMI-A-1, pointer lands where clicked on both, window drag across the seam works, keyframes on both after connect. (b) plain `mstsc` — expect one 5120x1440 desktop. (c) `journalctl --user -u app-org.kde.krdpserver --no-pager --since -10min | grep -E 'Reset graphics|Adaptive quality|Rebuilt'`. (d) DPMS-off → connect: both surfaces recover. Record results in `research.md` under OPT-018; if (a) fails on layout, capture mstsc's `RDPGFX_CAPS`/monitor negotiation lines and stop for analysis.

- [ ] **Step 3: Docs and commit**

`research.md`: `OPT-018 DONE 2026-xx-xx` with the measured per-monitor fps from Task 1 and Steve's results; `README.md`/`~/dev/rdp/CLAUDE.md`: `MonitorMode=multi` documented next to the others (note the 4096-px rule and the `/multimon` client flag). Commit `docs: OPT-018 per-monitor surfaces status` with the trailer.

---

## Self-review

- Spec coverage: §4.3 items 1 (capture per output) → Task 4 via N sessions; 2 (encode per monitor) → inherent in per-session encoders, gated by Task 1; 3 (package per surface) → Task 3; 4 (input in unified space) → Task 2 + Task 4; 5 (cursor) → Task 4; 6 (hot-plug diffing) → Task 4 step 3 (v1 rebuild, as §4.3 allows); 7 (client-driven layout / OPT-017) → out of scope, noted. §4.5 encoder budget → Task 1 measures it. "Giant screen as an option" → plain mstsc against `multi` shows the union; `workspace` mode also remains.
- Placeholder scan: Task 3 step 2 references the existing helpers by name (`monitorLayoutForReset`, `monitorLayoutSummary`) and gives the exact PDU fields; Task 4's cursor translation and input translation are specified as formulas. No TBDs.
- Type consistency: `VideoFrame::monitorIndex` (int) is what `surfaceFor(int)`, `keyFrameRequested(int)` and `setMonitorIndex(int)` use; `VideoMonitor{QRect geometry; bool primary}` is the existing struct reused for `setMonitorLayout`.
