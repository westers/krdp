# KRdp Research: Wayland Damage + ICA/Thinwire-Inspired Optimization

## Goal
Reduce encoded bandwidth by leveraging compositor damage metadata and protocol-side region optimization, inspired by ICA/Thinwire and RDPGFX region/caching behavior.

## Status Snapshot (2026-02-20; clean-up 2026-09-15)
- `OPT-001` Damage metadata plumbed through encoded stream and consumed in KRDP: `REMOVED` (2026-09-15 — no consumer; KPipeWire does not pair damage 1:1 with encoded packets).
- `OPT-002` Damage-first send path with rectangle coalescing: `REMOVED` (2026-09-15 — encoder produces full-frame pictures, so full-surface AVC420 region restored per upstream).
- `OPT-003` Packet/metadata pairing with fallback and resync behavior: `REMOVED` (2026-09-15 — FIFO pairing mis-paired metadata to packets; deleted from both sessions).
- `OPT-004` Tile activity classification with per-region quality bias: `REMOVED` (2026-09-15 — fed only the informational RDPGFX metablock; never reached the encoder).
- `OPT-005` Progressive refinement after motion settles (with cooldown): `REMOVED` (2026-09-15 — no extra data encoded; a relabelled normal frame).
- `OPT-006` Congestion adaptation (frame rate + QP bias): `SUPERSEDED-BY-UPSTREAM` (2026-09-15 — RTT source-rate heuristic removed per upstream `978f1cb`; QP bias only touched the informational metablock; requested rate now fixed at the client value).
- `OPT-007` H264 Main preference and fallback profile handling: `DONE` (kept — pure fork value over upstream).
- `OPT-008` AVC444 negotiation scaffold with AVC420 fallback and intent bias: `REMOVED` (2026-09-15 — AVC420-only negotiation restored to match upstream `origin/master`).
- `OPT-009` True multi-monitor protocol layout (server advertises multiple monitors/surfaces): `PARTIAL` (kept — per-monitor `ResetGraphics` layout retained; transport is still a single encoded surface).
- `OPT-010` True AVC444 transport path end-to-end: `REMOVED` (2026-09-15 — the `LC=1`/`LC=2` wire path was AVC420-in-an-envelope / chroma-only-broken; deleted with `VideoCodecSupport.h`).
- `OPT-011` Automatic GPU encode-device selection (avoid decode-only VAAPI backends): `PARTIAL` (kept — auto-selection moved to startup and re-runs on config change; the libx264 software-fallback/stall-watchdog was `REMOVED` on 2026-09-15 because `start()` from the Idle handler is a no-op against KPipeWire 6.6 and it leaked `KPIPEWIRE_FORCE_ENCODER` process-wide).
- `OPT-012` Explicit tile/content cache reuse strategy: `REMOVED` (2026-09-15 — inert with full-frame encode, `cacheSlot` was 0-based (invalid), disabled by default; no path to a benefit without a damage-cropping encoder).
- `OPT-013` Persisted VAAPI mode controls in KCM/server config (`auto|off|radeonsi|iHD`): `DONE` (kept).
- `OPT-014` Startup observability and smoke-test encoder assertions: `DONE` (kept; startup summary line updated to drop the AVC444 fields).

## Tracking Rule
- Every optimization item must have a stable ID in the form `OPT-###`.
- Every ID must carry a status: `TODO`, `WIP`, `BLOCKED`, `PARTIAL`, or `DONE`.
- When status changes, update this file with the date and short reason.

## Status Updates
- OPT-018 DONE 2026-09-16 — per-monitor RDP surfaces (`MonitorMode=multi`). One `PlasmaScreencastV1Session` and one RDPGFX surface per monitor: N sessions -> N `h264_vaapi` encoders -> N `CreateSurface` + `MapSurfaceToOutput` at each monitor's own origin, under a single `ResetGraphics` carrying the full monitor list and the union desktop size. Input stays workspace-global: `SessionController::onInputEvent()` translates RDP desktop coordinates back to KWin-global logical ones (`SurfaceLayout::originOf()`, the exact inverse of the translation `VideoStream::setMonitorLayout()` applies) and injects through `sendGlobalEvent()` on the first session, because `org_kde_kwin_fake_input` addresses the whole workspace and `sendEvent()`'s per-session clamping would pin the pointer to one output.
  - **Commits (`544f340..11c94ab`):** `544f340` examples: plasmastreamer --multi runs one session per screen · `c3a28c0` session: monitor index on frames, output geometry, global-coordinate input · `0458b02` video: anchor RDP surface space at the monitor union, not the primary · `c8eecf0` video: one RDPGFX surface per monitor · `3d603a8` video: drop mismatched frames and abort a failed surface reset · `fec98b8` server: MonitorMode=multi — one session and surface per monitor · `964fc45` server: keep the connection when one monitor session fails · `04589bc` server: pure multi-layout selection with unit test · `0851d21` server: apply monitor index before multi-mode toggle · `fc93f25`/`c741cb0` docs: wire validation captured / re-captured · `30e8133` video: keyframes supersede only their own monitor's queued frames · `32fb984` video: clear stale frames on layout change; inclusive monitor edges; pendingReset under lock · `fc710ab` server: correct surface size from the started session; input via an active session; rebuild after a drop; refuse multi for portal · `c86a737` docs: hedge /multimon claim · `11c94ab` server: name the port in runtime-update log lines. Ledger and reports: `.superpowers/sdd/2026-09-16-opt-018-per-monitor-surfaces/`.
  - **Semantics.** `multi` creates one surface per monitor. A client that negotiates multi-monitor (e.g. `sdl-freerdp3 /multimon`) gets them as separate remote monitors; a client that does not still sees the union as one 5120x1440 desktop — that is the "giant screen" option, obtained for free rather than as a separate mode. The 4096-px VA-API surface limit applies **per monitor**, not to the union, which is exactly why `multi` is the way to use both 2560x1440 outputs at once (`workspace` mode cannot: 5120 > 4096). Hot-plug is handled as a v1 rebuild — the whole session set is torn down and rebuilt on a monitor change, not diffed. If one monitor's session fails to start, that session is dropped and the connection is kept with the remaining monitors rather than failing the whole connection. The client's own declared layout and `/size:` are ignored; the server always streams its own monitors.
  - **Switching (live reload, no restart):** `kwriteconfig6 --file krdpserverrc --group General --key MonitorMode multi --notify`. The live service keeps `MonitorMode=specific` as its default.
  - **Encoder budget (Task 1, `krdpplasmastreamer --multi`, clean run, no RDP client connected, 2026-09-16 16:5x).** Two concurrent `h264_vaapi` contexts on the Radeon 780M work. `--quality 80 --quit-after 20 --wake-after 2,6,10,14` with mouse activity on DP-1 only: session 0 (DP-1) 26 frames / 1 keyframe / avg_fps 1.11; session 1 (HDMI-A-1, idle) 1 frame / 1 keyframe / avg_fps 0.04. Both raw streams `ffprobe` as `h264 (Main), yuv420p, 2560x1440`. Frame rate follows damage, as KWin is damage-driven; neither session starved. No KPipeWire change was needed.
  - **Wire validation (Task 5 Step A, 2026-09-16 18:42).** A second `krdpserver --plasma --port 3390` instance with its own `XDG_CONFIG_HOME=/tmp/krdp-t5/config` (`MonitorMode=multi`, `SystemUserEnabled=false`, `-u krdptest`), leaving the live service on 3389 untouched. Startup summary: `KRDP startup summary: session=plasma stream=multi:2 port=3390 quality=80 vaapiMode=radeonsi … wakeDisplay=1 adaptive=1`. A 40 s `wlfreerdp3 /v:… /gfx:AVC420 /cert:tofu` session from `buzz.local` (FreeRDP **3.22**, before that machine's upgrade — see the regression note below) gave, server-side:
    - `Using output stream index 0 screen "DP-1" logical rect QRect(0,0 2560x1440)` and `Using output stream index 1 screen "HDMI-A-1" logical rect QRect(2560,0 2560x1440)` — both monitor sessions started, each with its own VA-API context on `renderD128`.
    - one graphics reset carrying both monitors, then surface 1 created at `QPoint(0,0)` and surface 2 at `QPoint(2560,0)`, both `QSize(2560, 1440)`. (This run predates the reworded reset log line; see the re-capture below for its current text.)
    - **2111 frames sent: 1587 on `monitorIndex 0` and 524 on `monitorIndex 1`, with 22 keyframes on each surface.** Frames continued on both to the end of the run.
    - no `mismatch`, `unknown surface`, `CreateSurface failed` or `Dropping frames` warnings; the only VA-API error is the expected `radeonsi` probe failing against the NVIDIA render node (`10de:2684`) before succeeding on the AMD one, once per session.
    - The adaptive loop (OPT-016) stepped 80 -> 10 over the first 20 s against a backlog of 56-107 unacknowledged frames, then climbed back to 75 once the backlog cleared. Two 2560x1440 streams are roughly twice the bitrate of one and the Wi-Fi client, not the encoder, was the bottleneck. Working as designed, but it is the first evidence that `multi` costs real bandwidth.
  - **Wire validation re-captured on the current build (Task 5 fix round 1, 2026-09-16 19:20).** The run above predates the reworded `Reset graphics` line, so a second 20 s `wlfreerdp3` session was taken against the same second-instance recipe, after buzz was reverted to the H.264-capable FreeRDP 3.22 (`WITH_GFX_H264=ON`, `WITH_FFMPEG=ON`). The reset line now executes and reads, verbatim:
    ```
    org.kde.krdp: Reset graphics desktop QSize(5120, 1440) with 2 monitor(s): "0,0 2560x1440 primary; 2560,0 2560x1440"
    ```
    Both monitor sessions again: `Using output stream index 0 screen "DP-1" logical rect QRect(0,0 2560x1440)` and `index 1 screen "HDMI-A-1" logical rect QRect(2560,0 2560x1440)`. Counts from this run: 794 frames on `monitorIndex 0` (2 keyframes) and **1 frame on `monitorIndex 1` (1 keyframe)** — HDMI-A-1 was idle for the whole 20 s, and KWin is damage-driven, so an idle output yields only its initial keyframe. That is the same pattern Task 1's clean harness run recorded (session 1: 1 frame, avg_fps 0.04), not a regression; the 18:42 run above, where the monitor saw activity, is the evidence that frames flow continuously on both. `grep -ciE "mismatch|unknown surface|CreateSurface"` over the whole server log returns 0. Zero `Adaptive quality` lines in this run: quality sat at the cap with a clear link, so the controller never stepped — consistent with the OPT-016 acceptance note.
    Client-flag note worth keeping: **bare `/gfx` is not enough even on an H.264-capable FreeRDP.** With `/gfx` the 3.22 client advertises `AVC: true YUV420: false` on `RDPGFX_CAPVERSION_101` and `AVC: false YUV420: true` on 104-107, never both on one caps set, so KRDP rejects it with `Client does not support H.264 in YUV420 mode!`. `/gfx:AVC420` is required, and it selects `RDPGFX_CAPVERSION_81`.
    - Evidence source: `VideoStream::sendFrame()` now carries one debug-gated `Sending frame N monitorIndex M surface S at ORIGIN SIZE keyframe|delta B bytes` line (category `org.kde.krdp`, off by default). `PlasmaScreencastV1Session::sendGlobalEvent()` gained a matching `Global pointer motion to …` debug line, and the `Reset graphics` line now names the desktop size and monitor count. Added in Task 5 and **kept** — in multi mode these are the only places the per-monitor wire behaviour can be checked.
  - **Not validated on the wire at this point: `/multimon`, `/size:`, and the input path.** See the FreeRDP regression note below. Steps A.4 (input), A.5 (`/size:1920x1080`) and B (`sdl-freerdp /multimon`) reached authentication and both monitor sessions but no frames, so per-monitor *rendering* on a genuinely multi-monitor-aware client, pointer landing across the seam, and window drag across the seam were all still unverified as of this run. Superseded below: the final fix round, the post-fix re-test, and Steve's acceptance close the input-path and giant-desktop questions; `/multimon` (separate client monitors) remains open.
- 2026-09-16 **FreeRDP client H.264 regression (blocks further wire tests; affects Steve's daily use).** `buzz.local` was upgraded at 18:43 from `libfreerdp3-3` **3.22.0+dfsg-1** to **3.31.0+dfsg-0ubuntu0.26.04.1** (the `apt-get install freerdp-sdl …` that added `sdl-freerdp`). The 3.31 Ubuntu packages are built with `WITH_GFX_H264=OFF`, `WITH_OPENH264=OFF`, `WITH_FFMPEG=OFF` (`sdl-freerdp /buildconfig`), so the client parser now rejects `/gfx:AVC420` outright (only bare `/gfx` and `/gfx:mask:<v>` parse) and advertises `RDPGFX_CAPS_FLAG_AVC_DISABLED` in every caps set that carries YUV420. KRDP requires `avcSupported && yuv420Supported` in the *same* caps set (`VideoStream::onCapsAdvertise()`), so it logs `Client does not support H.264 in YUV420 mode!` and closes the connection with `ERRINFO_GRAPHICS_SUBSYSTEM_FAILED` — there is no RFX or planar fallback in this fork. **Every FreeRDP-based client on buzz, Remmina included, therefore cannot connect to KRDP at all right now**, in any `MonitorMode`. hal9000's own FreeRDP was already 3.31 (upgraded 2026-09-01) but only the *server* libraries matter there, so the KRDP server side is unaffected. Recovering H.264 on the client needs a FreeRDP built with `WITH_GFX_H264=ON` (self-built, or a non-dfsg package); adding a non-H.264 fallback codec path to KRDP would be the alternative and is not currently planned.
- OPT-018 FINAL REVIEW 2026-09-16 — whole-branch review (`740e8e0..c741cb0`) found one Critical and four Important defects before sign-off, all fixed in one dispatch (commits `30e8133`, `32fb984`, `fc710ab`, `c86a737`; full detail in `.superpowers/sdd/2026-09-16-opt-018-per-monitor-surfaces/final-fix-report.md`):
  - **Critical:** a keyframe on one monitor cleared the whole per-connection frame queue, dropping the other monitor's still-queued keyframe. Fixed: `FrameQueuePolicy::dropSupersededFrames()` clears only entries matching the keyframe's own `monitorIndex`; outside multi mode every frame carries index 0, so the non-multi path is byte-for-byte unchanged (`FrameQueuePolicyTest`, 8 cases).
  - **Inclusive `TS_MONITOR_DEF` edges** (MS-RDPBCGR 2.2.1.3.6.1: right/bottom are inclusive, not exclusive) — this was wrong on the pre-existing single-surface path too. For a 2560x1440 monitor at the origin, right/bottom go from 2560/1440 to **2559/1439**; the second monitor at (2560,0) goes to right 5119/bottom 1439. `SurfaceLayout::edgesOf()` is now the one place any `ResetGraphics` monitor def is built (`SurfaceLayoutTest`: `monitorEdgesAreInclusive`, `offsetMonitorEdgesAreInclusive`).
  - Frames already queued under an old layout kept their old `monitorIndex` after a layout change; `setMonitorLayout()` now clears the queue whenever the layout actually changes (identical layouts, including empty-to-empty, clear nothing).
  - Surface size for a fractionally-scaled monitor is now taken from the session once its `started()` fires, not computed in advance as `logicalSize × DPR` (that rounding could land one pixel off the real capture size and permanently drop every frame); only the layout entry's size is corrected, not its origin.
  - Input now goes through the first session with an *active* stream (`SessionWrapper::inputSession()`) rather than always `sessions.front()`, so a pointer event is no longer silently dropped when that particular session's stream isn't up yet.
  - `MonitorMode=multi` against a Portal session (no `--plasma`) now logs once and falls back to `specific` instead of trying to open N portal dialogs.
  - A monitor dropped by `dropSession()` (its session failed to start) is now rebuilt on the next topology change instead of staying dark forever (`refreshMultiLayout()` now treats "fewer live sessions than monitors" as a topology change).
- OPT-018 RE-TEST 2026-09-16 ~20:2x (post-fix build `c86a737`/`4c6a903`, second instance on :3390, `wlfreerdp3` 3.22 from buzz, synthetic activity driven onto both outputs via a KWin script) — **PASS**: run 2 held a client for the full 45 s with **188 frames on monitor 0 (DP-1) and 238 on monitor 1 (HDMI-A-1), steadily interleaved** rather than one-then-the-other, one keyframe per monitor (gop 600 means no periodic IDR is due at these frame counts; keyframe independence across monitors was separately confirmed from a heavier run where all 5 keyframe pairs survived intact), inclusive edges as above, zero adaptive-quality steps, zero `mismatch`/`unknown surface`/`CreateSurface` warnings. A significant but **pre-existing, not multi-specific** concern surfaced in a heavier-load run: client frame acknowledgements can stop entirely under two-stream load (`pending min-after-ack -1`, `goodput 0`), and the adaptive loop's response — a QP step-down that reopens the encoder and costs a fresh IDR — compounds the stall instead of relieving it (tracked as `OPT-039` below). Full detail: `retest-report.md`.
- OPT-018 STEVE'S ACCEPTANCE 2026-09-16 — Remmina 1.4.43 on FreeRDP 3.22 (buzz) against a `MonitorMode=multi` test instance on :3391: one 5120x1440 desktop (the giant-screen presentation, since Remmina doesn't negotiate multi-monitor), the pointer crossed the seam onto the second monitor with no issues, picture stable throughout, 5730/1209 frames on the two monitors. This verifies the giant-desktop presentation and the input translation across the seam end to end with the client Steve actually uses daily. **Still unverified: `/multimon`** (a genuinely multi-monitor-aware client showing the two monitors as separate remote displays) — needs an H.264-capable SDL client run with buzz's second (laptop) monitor attached; `sdl-freerdp3` 3.22 (Debian's `3.22.0+dfsg-1` snapshot build, H.264 on) is now installed on buzz for that, after buzz's Ubuntu 3.31 packages turned out to have `WITH_GFX_H264=OFF` (see the regression note above).
- OPT-018 fix round 2 2026-09-16 — investigated an apparent bug surfaced by the re-test (`Applied runtime quality update: … active sessions: 0` logged while a multi client appeared to be streaming). **Verdict: NO BUG** — runtime quality updates do reach connected multi-mode sessions (reproduced live: cap 80→65 with a keyframe on both surfaces 18 ms later); the re-test's "0" line was logged just after that client's connection had already broken. Commit `11c94ab` names the listening port in the runtime-update log lines so a same-named-file question like this one can be answered by grepping the port instead of re-deriving it from timing. Full trace: `fix2-report.md`.
- OPT-018 gotcha: `kwriteconfig6 --notify` on **any** `krdpserverrc` — including one under a throwaway `XDG_CONFIG_HOME` for a disposable test instance — broadcasts by config file *name* over the session bus, so every running `krdpserver`, the live service included, reloads its own config (a no-op if the live file is unchanged). A plain write with no `--notify` stays in-process. Use `--notify` only when the live service's reload is actually wanted.
- OPT-039 PLANNED — adaptive quality: a step-down reopens the encoder and costs an IDR per surface; under a client that stops acknowledging (pending frames climbing, goodput 0) this compounds the stall; consider deferring step-downs while no acks arrive at all (min-after-ack == -1) or lengthening the hold.
- OPT-037 PLANNED — `krdp-monitors` private DVC. The client sends `{"visible":[i,...]}` naming the monitors it is actually showing; the server calls `requestStreamingDisable()` on the monitor sessions that are hidden and re-enables them on demand, forcing an IDR on restart. Saves an encoder and its bitrate for every monitor the user has collapsed or scrolled away. Client side lands in `~/dev/krdp-client` slice 2.
- OPT-016 ACCEPTED 2026-09-16 17:08 — Steve's journal: zero adaptive steps on idle and during a ~33 Mbit/s burst; windows ~500 ms.
- OPT-016 DONE (code) 2026-09-16 — live validation pending Steve's session: adaptive quality steering end to end. `NetworkDetection` measures goodput in kbit/s from FreeRDP bandwidth results; `VideoStream::updateAdaptiveQuality()` steps quality from goodput + RTT via `KRdp::AdaptiveQuality::step()` (unit-tested, 8 cases, `build/bin/AdaptiveQualityTest`), at most once per `QualityUpdateInterval` (1.5 s). The `Quality` setting is now a **cap**, not a fixed value; a new `AdaptiveQuality` kcfg key (`Bool`, default `true`) enables/disables the loop (`VideoStream::setAdaptiveQuality()`). With the private KPipeWire, each quality step reopens the `h264_vaapi` codec at the new QP (forcing an IDR — see `h264vaapiencoder.cpp`'s `Reopened h264_vaapi` log); the RDPGFX metablock now reports the actual QP in use instead of the static configured value. Harness sanity (`krdpplasmastreamer --quality-at 4:40,8:100`) confirmed two `Reopened h264_vaapi` events with matching `fixed QP` lines (18/18 → 29/29 → 12/12) and `Key frames: 3`. Deployed to the live service 2026-09-16 15:28 (startup summary now reports `adaptive=1`). Consuming RDPGFX QoE acks (`gfxQoEFrameAcknowledge`) remains a stub — goodput+RTT is the first loop; QoE acks are a possible future refinement, not required by this pass.
- 2026-09-16: client roadmap correction (Steve) — no Windows client; OPT-026 becomes a Linux RDP client with multi-monitor support, lower priority, to be brainstormed with Steve before planning (see ~/dev/rdp/RDP_QUALITY_RESEARCH.md §6.4).
  - **Live validation (Steve, from Windows)**: after a few minutes of normal use, run:
    ```bash
    journalctl --user -u app-org.kde.krdpserver --no-pager --since -10min | grep -E 'Bandwidth measurement|Adaptive quality|Reopened h264_vaapi'
    ```
    Expect periodic `Bandwidth measurement: … -> N kbit/s` samples, `Adaptive quality -> N (target … cap 80 goodput … kbit/s …)` lines with quality moving within `[10, 80]`, and exactly one `Reopened h264_vaapi at quality N (QP M)` per quality move (not one per bandwidth sample). If the loop instead oscillates — alternating up/down roughly every 1.5 s — that's a known open tuning risk: fix in a future session by raising `QualityUpdateInterval` (currently a 1.5 s `constexpr` in `VideoStream.cpp`) to 3 s and widening the congestion gate to 2x. Record whatever is actually observed here (steady convergence vs. oscillation) once tested.
- 2026-09-16 OPT-016 production finding + fix (Task 6): live validation above **did** oscillate. Steve connected at 15:28 and by 15:40-15:42 the journal showed goodput samples like "10 bytes in 1 ms -> 296 kbit/s" and "416 bytes in 1 ms -> 1704 kbit/s" — the per-frame `startBandwidthMeasure()`/`stopBandwidthMeasure()` bracket in `VideoStream::sendFrame()` gave FreeRDP a ~1 ms window per measurement, far too short to be a real goodput sample, so quality thrashed between 10 and 80 (`journalctl --user -u app-org.kde.krdpserver --since "15:40:00" --until "15:42:30"`). Fix: `NetworkDetection` now runs the measurement on a schedule instead — a 500 ms window opened every 2 s from `NetworkDetection::update()` (ported from upstream krdp's `bandwidthMeasureDuration`/`bandwidthMeasureInterval`), with the per-frame calls in `VideoStream::sendFrame()` removed. `onBandwidthMeasureResults()` also now rejects a sample with `timeDelta < 100 ms` or `byteCount < 4096` before it reaches the smoothing filter, and `VideoStream::updateAdaptiveQuality()` waits for `NetworkDetection::validBandwidthSamples() >= 3` before it starts steering — a fresh connection stays at the `Quality` cap until the estimate is real. Second finding from the same incident: attempting to hot-fix via `kwriteconfig6 --notify --file krdpserverrc --group General --key AdaptiveQuality false` against the *running* service did not stop the adaptation (no `Applied runtime adaptive quality update` line appeared in the journal for that process; the team ended up restarting the service instead, which is why the later startup summary shows `adaptive=0` with `active sessions: 0`). Root cause: `server/main.cpp`'s `applyRuntimeConfig` called `ServerConfig::self()->read()`, and `KCoreConfigSkeleton::read()` explicitly documents that it "assumes the KConfig object was previously loaded... without reloading from disk" — unlike `load()`, which forces a disk re-read. This left the `QFileSystemWatcher`-triggered fallback path (a plain file edit, or a write without `--notify`) reading stale in-memory values by design, and made the `KConfigWatcher`-triggered path (the one used here) rely on both call sites happening to share the identical `KSharedConfig` object during `reparseConfiguration()` — confirmed by a standalone repro (`ServerConfig::self()` + `KSharedConfig::openConfig("krdpserverrc")` + a direct on-disk edit with no `--notify`): `config->read()` did not observe the change, `config->load()` did. Fixed by changing `config->read()` to `config->load()` in `applyRuntimeConfig`; a `Runtime config applied: quality … adaptive … monitorMode … monitorIndex … wakeDisplay … vaapiMode …` line now logs every reload so a future live toggle can be confirmed from the journal instead of inferred.
- 2026-09-16 OPT-016 production finding + redesign (Task 7): live validation of the Task 6 loop (journal of PID 2604187, `AdaptiveQuality=true`, LAN client, mostly idle desktop, 16:30-16:38) showed it **still mis-steering** — quality sawtoothed 55↔80 while nothing was wrong with the link. Three defects. (1) The `link-limited` gate (`maxPendingFramesSinceSample >= 2`) trips on any two frames in flight, which a 60 Hz mouse move with ~20 ms ack latency produces constantly, so every step logged `link-limited`. (2) Idle 4-14 KB bandwidth windows dragged the 0.5-EMA goodput down to 0.4-2 Mbit/s, so the goodput→quality `target` came out 21-29; under a *real* limit the mapping errs the other way, since CQP output at `Quality` 80 reached 15 Mbit/s while scrolling against the anchor table's 4.5 Mbit/s "full quality" figure — a bitrate→quality table cannot steer a CQP encoder in either direction. (3) Decisions were only taken on `bandwidthChanged`, which never fires on an idle desktop (samples < 4 KB are rejected), so a drop was never climbed back. Mitigated live at 16:38:44 with `AdaptiveQuality=false`. **Redesign:** the goodput control law is gone; quality now moves on *pressure* only. RTT inflation (average at least `CongestionRttMargin` = 5 ms above the minimum **and** at least 1.5x it) or a persistent backlog (the client never got within `BacklogFrames` = 4 unacknowledged frames of caught up for a whole interval) steps quality down by `StepDown` = 10; a clear interval steps it up by `StepUp` = 5 towards the cap, but only after `ClimbHoldAfterStepDown` = 5 s has elapsed since the last step down, so a limited link settles instead of sawtoothing. Decisions now run on a main-thread `QTimer` at `QualityUpdateInterval` (1.5 s) started in `VideoStream::initialize()` and stopped in `close()` (both via queued `invokeMethod`, since both run on the connection thread), not on `bandwidthChanged` — so a drop is climbed back on an idle desktop too. Backlog evidence is `minPendingAfterAckSinceDecision`, the smallest pending-frame count observed right after any ack since the last decision, lowered to 0 wherever `pendingFrames` is cleared. `NetworkDetection` is unchanged and `bandwidth()` is now **logged only, never used to steer**. A 3 s `WarmupAfterStreamStart` gate replaces the `validBandwidthSamples() >= 3` gate so the minimum-RTT baseline has time to form. Unit tests: 9 cases in `build/bin/AdaptiveQualityTest`. The journal check command is unchanged; the log line now reads `Adaptive quality -> N ( congested|backlogged|clear rtt avg … min … us, pending min-after-ack … now …, goodput … kbit/s, cap … )`. Also in this task: `RdpConnection::run()`'s `WaitForMultipleObjects` now uses a 100 ms timeout instead of `INFINITE`, so `NetworkDetection::update()` (RTT probe every 70 ms, bandwidth window stop after 500 ms) runs on an idle link too — waiting only on socket activity had stretched idle bandwidth windows to 1.0-1.6 s.
- 2026-09-16 OPT-015 DONE: private KPipeWire (~/dev/kpipewire westers/opt-015) — h264_vaapi async_depth=1, rc_mode=CQP, quality→QP 40..12, gop 600, IDR QP = P QP (FFmpeg's default; no override), libx264-only options dropped, frame-repeat off for h264_vaapi.
- 2026-09-16 OPT-023 PARTIAL: frame-repeat disabled for h264_vaapi; QoE-driven pacing remains (OPT-016).
- 2026-09-16 OPT-035 DONE via PipeWireBaseEncodedStream::requestKeyFrame(); the encoder-restart path remains only as the stock-KPipeWire fallback (compile-time detection).
- 2026-09-15: `OPT-035` (force IDR on surface (re)create) implemented. KPipeWire 6.6.4 exposes no keyframe-request API (`grep -i keyframe /usr/include/KPipeWire/*.h` finds only `Packet::isKeyFrame`), so `VideoStream::sendFrame()` emits a new `keyFrameRequested()` signal when `performReset()` runs and the frame being sent is not a keyframe (rate-limited to once per 2 s). `PlasmaScreencastV1Session::requestKeyFrame()` restarts the encoded stream on the same PipeWire node through the existing deferred re-attach (a restarted stream always opens with an IDR); the `PortalSession`/base implementation logs a no-op. Also closed two silent first-IDR losses: the submission thread no longer discards a frame when caps are not yet confirmed — `sendFrame()` now returns false and the frame is re-queued (head) instead of dropped — and the start-up burst can no longer drop the IDR (Phase 1 clear-only-on-keyframe). Harness: first delivered frame is `keyframe true`; once damage exists the first keyframe lands ~100 ms after it.
- 2026-09-15: Fork performance clean-up (see `~/dev/rdp/FORK_PERFORMANCE_REVIEW.md`). Removed the no-op quality/activity/refinement/congestion-QP logic, the AVC444/AVC444v2 wire path (`VideoCodecSupport.h`), the RDPGFX tile cache, the damage coalescer and the packet/metadata FIFO pairing in both sessions. Restored upstream's full-surface AVC420 send (single region rect, `qp=22`, full `destRect`) and AVC420-only caps negotiation. Ported upstream `d399708` (clear pending-send queue only on a new keyframe; never drop encoded P-frames), `cc67efe` (pendingFrames mutex, atomic `requestedFrameRate`, close-before-context ordering, dequeue-after-caps guard) and `d736d8a` (GFX CapsAdvertise re-advertisement reset). Removed the software-fallback / hardware-retry / stall-watchdog state machine and the display-change→libx264 policy from `AbstractSession`; stream restarts now go only through the deferred `attachEncodedStream()` + nodeId poll. `d3b0651` pre-encode backpressure was **not** ported: KPipeWire 6.6.4 has no `setEncoderPaused`.
- 2026-09-15: `OPT-006` — removed the RTT-based source-rate heuristic (upstream `978f1cb`). The upstream ack-window + goodput adaptive-quality replacement was not ported: the installed KPipeWire 6.6.4 and the fork's `NetworkDetection` lack the required APIs (`NetworkDetection::bandwidth()`, encoder pause). Requested frame rate is pinned to the client-configured value.
- 2026-02-20: `OPT-013` marked `DONE` after wiring `General/VaapiDriverMode` through KCM and server startup environment handling.
- 2026-02-20: `OPT-014` marked `DONE` after adding a startup summary log line and smoke-test encoder path assertions.
- 2026-02-20: `OPT-011` reliability pass added one-shot software fallback (`libx264`) when PipeWire encoder initialization fails.
- 2026-02-26: `OPT-012` marked `PARTIAL`: RDPGFX tile content cache infrastructure implemented (SurfaceToCache/CacheToSurface replay, LRU eviction, batched fill, caps awareness, quality/reset invalidation). Disabled by default — FreeRDP xfreerdp crashes on SurfaceToCache PDUs (error 1359). Enable with `KRDP_ENABLE_TILE_CACHE=1` for clients that support RDPGFX bitmap caching.
- 2026-02-20: `OPT-011` reliability pass extended fallback to runtime startup stalls: if no encoded packets are received shortly after stream activation, KRDP forces `libx264` and retries once (with temporary override restoration so configured VAAPI mode remains in effect afterward). This stall watchdog is now disabled by default and requires `KRDP_ENABLE_STALL_WATCHDOG=1` to activate.
- 2026-02-20: `OPT-009` moved to `PARTIAL` by advertising monitor layout metadata in RDPGFX reset; full multi-surface transport is still pending.
- 2026-02-20: `OPT-010` moved to `PARTIAL` after adding experimental AVC444/AVC444v2 wire transport framing (`RDPGFX_AVC444_BITMAP_STREAM`, LC single-stream mode) under `KRDP_EXPERIMENTAL_TRUE_AVC444`.
- 2026-02-20: Added explicit runtime settings inventory (below) so we have one project-memory reference for KCM/config/env controls and their scope.

## Runtime Settings Inventory (Project Memory)
This section is the canonical quick reference for runtime knobs already implemented.

### KCM / `krdpserverrc` (`[General]`)
- `Quality` (`50..100` in KCM): with `AdaptiveQuality` enabled, this is now a **cap** on the adaptive loop rather than a fixed value; live-applied at runtime to active sessions; does not require service restart.
- `AdaptiveQuality` (`Bool`, default `true`, kcfg key `General/AdaptiveQuality`): steers quality down on RTT inflation or a persistent unacknowledged-frame backlog and back up on clear intervals (see `OPT-016` above; goodput is logged, not used); `false` pins quality to the `Quality` cap as before.
- `MonitorMode` (`workspace|primary|specific|multi`): live-applied stream target selection. `multi` streams every monitor as its own RDPGFX surface (see `OPT-018` above); the default remains `specific`.
- `MonitorIndex` (used when `MonitorMode=specific`): live-applied monitor selection.
- `VaapiDriverMode` (`auto|off|radeonsi|iHD`):
  - `auto`: enables KRDP VAAPI driver auto-selection.
  - `off`: disables KRDP VAAPI auto-selection (`KRDP_AUTO_VAAPI_DRIVER=0`).
  - `radeonsi` / `iHD`: forces the requested VAAPI driver via `KRDP_FORCE_VAAPI_DRIVER`.
  - Important: this is VAAPI driver selection, not a full "force software encoder" session policy.

### Runtime Environment Variables
- `KRDP_FORCE_VAAPI_DRIVER=<driver>`: force VAAPI driver selection in KRDP startup/device probing.
- `KRDP_AUTO_VAAPI_DRIVER=0`: disable KRDP automatic VAAPI driver selection.
- `KPIPEWIRE_FORCE_ENCODER=libx264`: honoured by KPipeWire itself if the user sets it; as of the 2026-09-15 clean-up KRDP no longer reads or writes this variable.
- (removed 2026-09-15) `KRDP_EXPERIMENTAL_AVC444*` / `KRDP_EXPERIMENTAL_TRUE_AVC444` / `KRDP_ENABLE_TILE_CACHE` / `KRDP_ENABLE_STALL_WATCHDOG`: the features behind these flags were deleted.

### Current Display-Change Recovery Behavior (2026-09-15)
- Display geometry/topology changes trigger a screencast re-create that goes through the deferred `attachEncodedStream()` path (it waits for KPipeWire's produce thread to tear down — `nodeId()==0` — before calling `start()` again).
- Config reloads (e.g. a quality-slider write) are decoupled from display-refresh: a quality change never touches the stream.
- The software-fallback / hardware-retry / stall-watchdog policy was removed: it could not restart a KPipeWire 6.6 stream and leaked `KPIPEWIRE_FORCE_ENCODER` process-wide. KPipeWire's own internal VAAPI→libx264 fallback still applies at encoder init.

## Current KRdp Capture Path (Source Evidence)
KRdp already uses PipeWire and encoded streams.

- `src/AbstractSession.cpp` constructs `PipeWireEncodedStream` and configures framerate/quality.
- `src/PlasmaScreencastV1Session.cpp` uses PipeWire encoded streams and sets `H264Baseline` encoder.
- `src/PortalSession.cpp` (not shown here) also configures PipeWire encoded streams.
- Build links to `KPipeWire` and `KPipeWireRecord` in `CMakeLists.txt` and `src/CMakeLists.txt`.

This confirms the current pipeline is PipeWire-based and already receives encoded H.264 packets.
KRDP now consumes encoded frame metadata (damage/size/PTS) where available and falls back safely when metadata is missing or delayed.

## Wayland / PipeWire Damage Metadata
The key opportunity is to get damage rectangles per frame and use them to constrain what we encode or transmit.

- `ext-image-copy-capture-v1` provides damage events for each frame; first frame is full damage, subsequent frames are deltas. It also supports client-provided `damage_buffer` metadata to reduce copies.
- `wlr-screencopy-unstable-v1` provides `copy_with_damage` and a damage event list.
- PipeWire exposes damage via `SPA_META_VideoDamage` metadata on buffers.

If KRdp uses PipeWire frames (raw or encoded), confirm whether `KPipeWire` or `PipeWireEncodedStream` exposes `SPA_META_VideoDamage` and whether we can propagate it into KRdp’s encoder or region logic.

## KPipeWire Findings (How Damage Is Exposed)
The KPipeWire library already parses PipeWire damage metadata, but it is only available on **raw** streams, not on the encoded stream API currently used by KRdp.

- `PipeWireSourceStream` has `setDamageEnabled(bool)` and emits `frameReceived(const PipeWireFrame &frame)` where `PipeWireFrame` includes `std::optional<QRegion> damage`.  
- In `PipeWireSourceStream::handleFrame`, KPipeWire reads `SPA_META_VideoDamage` and converts it into a `QRegion`.  
- The damage meta is only requested if `setDamageEnabled(true)` was set **before** stream parameter negotiation (it toggles `m_withDamage`, which controls whether `SPA_META_VideoDamage` is added to the stream params).  
- `PipeWireEncodedStream` does **not** expose damage; `PipeWireEncodedStream::Packet` only carries encoded bytes + keyframe flag.  
- `PipeWireProduce` (used by `PipeWireEncodedStream`) constructs a `PipeWireSourceStream` but never calls `setDamageEnabled(true)`, so damage metadata is not requested by default.

Implication: to tap into damage information in KRdp, either:
1) Use `PipeWireSourceStream` directly and consume `PipeWireFrame.damage` (then handle encoding yourself or augment KPipeWire’s encode path), or  
2) Extend KPipeWire to plumb damage through `PipeWireEncodedStream` (e.g., enable damage in `PipeWireProduce::initialize()` and add a new signal or metadata on `Packet`).

### ABI-safe patch plan (Option B)
Patch file (private fork): `patches/kpipewire/0001-damage-metadata-encoded-stream.patch`

Changes included:
- Add `PipeWireBaseEncodedStream::setDamageEnabled(bool)` and `damageEnabled()` (must be set before `start()`).
- Enable `SPA_META_VideoDamage` on the underlying `PipeWireSourceStream` when damage is enabled.
- Add `PipeWireEncodedFrameMeta` + new signal `PipeWireEncodedStream::frameMetadata(...)` carrying damage/sequence/pts and size.
- Register the new meta type so cross-thread signal delivery works.

Why it’s safe:
- All changes are additive (no existing symbol changes).
- Damage is opt-in (default off), so existing apps remain unchanged.
- No changes to existing signals or packet layout.

### Private fork strategy (KRdp-only now, upstreamable later)
Recommended approach:
- Keep the patch in `patches/kpipewire/` and apply it to a private KPipeWire fork.
- In KRdp, add a CMake option `KRDP_USE_BUNDLED_KPIPEWIRE`:
  - OFF (default): use system KPipeWire.
  - ON: build and link against a vendored fork in `third_party/kpipewire` (git submodule or subtree).
- Maintain a clean patch series (like the file above) so upstreaming later is straightforward.

Minimal KRdp usage (once patched KPipeWire is in use):
- Before starting the stream: `encodedStream->setDamageEnabled(true);`
- Connect `frameMetadata` and cache the damage region per-frame for transport decisions.

## Build + Run Guide (Private Prefix, No System Interference)
These steps keep the patched KPipeWire isolated from system packages. This is the recommended approach on Kubuntu.

### 1) Build + Install KPipeWire (private prefix)
Private prefix path (recommended):
- `$KRDP_SRC/.deps/kpipewire`

Commands:
```bash
cmake -S /path/to/kpipewire -B /path/to/kpipewire/build \
  -DCMAKE_INSTALL_PREFIX=$KRDP_SRC/.deps/kpipewire \
  -DBUILD_TESTING=OFF
cmake --build /path/to/kpipewire/build
cmake --install /path/to/kpipewire/build
```

### 2) Build KRdp against the private KPipeWire
Point CMake at the private prefix so `find_package(KPipeWire)` resolves there first:
```bash
cmake -S $KRDP_SRC -B $KRDP_SRC/build \
  -DCMAKE_PREFIX_PATH=$KRDP_SRC/.deps/kpipewire
cmake --build $KRDP_SRC/build
```

### 3) Run without touching system packages
At runtime, ensure KRdp loads the private KPipeWire:
```bash
LD_LIBRARY_PATH=$KRDP_SRC/.deps/kpipewire/lib:$LD_LIBRARY_PATH \
  $KRDP_SRC/build/path/to/krdp_binary
```

### 4) Rebuild after system or dependency changes
If Qt/KF6/PipeWire changes (e.g., after `apt upgrade`), rebuild in this order:
1. Rebuild and reinstall KPipeWire in the private prefix.  
2. Reconfigure and rebuild KRdp (to refresh CMake cache and relink).

If builds become inconsistent, delete the build dirs (not the source or prefix) and rebuild:
```bash
rm -rf /path/to/kpipewire/build $KRDP_SRC/build
```

### 5) No interference with system packages (what not to do)
Avoid installing the fork into system paths like `/usr` or `/usr/local`.  
Do **not** replace `libkpipewire6` from apt.  
The private prefix approach keeps your system runtime and desktop components safe.

## ICA/Thinwire and RDPGFX Patterns Worth Borrowing
These are proven techniques to reduce bandwidth while maintaining UI quality.

- ICA/Thinwire uses region classification: transient (video-like) vs. static (text/UI). Transient regions use video codecs; static regions use JPEG/RLE/lossless tiles, with text overlays to keep crispness.
- RDPGFX (AVC) allows region rectangles to crop encoded content (`regionRects`), aligned to 16x16 macroblocks.
- RDPGFX uses surface/tile caches to avoid resending unchanged UI elements.
- RemoteFX has progressive refinement (send low quality first, then improve) to keep responsiveness under congestion.

## Proposed Enhancements For KRdp
These steps are ordered by likely ROI and feasibility given the current PipeWire-based pipeline.

1. Damage-first send decisions
- If we can access damage metadata from PipeWire, only transmit damaged regions. Coalesce nearby rectangles to reduce metadata overhead.
- Align regions to 16x16 blocks if we adopt an RDPGFX-like AVC region mask approach.

2. Region classification (Thinwire-style)
- Track recent damage history for each region/tile.
- Encode transient regions with video (H.264/H.265/AV1), static regions with JPEG/RLE or lossless tile codec.
- Consider text overlay for crisp UI (optional but strong quality gain).

3. Cache / tile reuse
- Add explicit tile cache IDs at protocol level, similar to RDPGFX surface cache operations.
- Maintain cache eviction policy based on usage frequency and age.

4. Progressive refinement under congestion
- Send low-quality deltas first; follow with refinement updates when bandwidth recovers.

## Open Questions To Resolve In Source
- Does `PipeWireEncodedStream::Packet` carry damage metadata, or only encoded frames? If not, do we need to switch to a raw `PipeWireSourceStream` to compute damage locally or to read `SPA_META_VideoDamage`?
- Is the screencast source `Screencasting::Metadata` already forwarding damage? If yes, where is it surfaced in `KPipeWire` APIs?
- Are there existing protocol hooks in KRdp for region rectangles, or would this require extending the protocol and client?

## References (External)
- ext-image-copy-capture-v1: https://wayland.app/protocols/wayland-protocols/440
- wlr-screencopy-unstable-v1: https://hoyon.github.io/wayland-protocol-docs/protocols/wlr_screencopy_unstable_v1.html
- PipeWire video damage metadata (SPA_META_VideoDamage): https://docs.pipewire.org/video-src-fixate_8c-example.html
- Citrix HDX/Thinwire design notes: https://community.citrix.com/tech-zone/design/design-decisions/hdx-graphics/
- RDPGFX AVC region rectangles: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/5f12c20e-2ea1-4ad1-a2a0-019ee3893731
- RDPGFX surface cache PDUs: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/7d40d7c2-5645-46a5-938d-d81c99c04f09
- RemoteFX adaptive graphics overview: https://techcommunity.microsoft.com/blog/microsoft-security-blog/remotefx-adaptive-graphics-in-windows-server-2012-and-windows-8/247454
