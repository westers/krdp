# KRdp Research: OPT-### tracker

## Goal
Reduce encoded bandwidth and latency of the KRdp fork (compositor damage, RDPGFX codecs, per-monitor and virtual outputs), and track every optimisation/feature as a stable `OPT-###` item.

## Tracking rule
- One line per item: status, one-sentence summary, pointers. Detail goes to reports/ledgers, not here.
- Status is one of `DONE`, `FROZEN` (code kept, work paused on purpose), `OPEN` (work outstanding, not active), `WIP` (being implemented now), `ABANDONED` (removed, superseded or deleted).
- Never renumber; a new item takes the next free id. On a status change, edit the line and add the date.
- Full history up to 2026-09-27 (all detailed evidence, commit lists, acceptance runs): `~/dev/rdp/archive/research-2026-09-27-full.md`. Ledgers: `.superpowers/sdd/<date>-<item>/`.

## Items
- `OPT-001` ABANDONED 2026-09-15 — damage metadata through the encoded stream: removed, no consumer (KPipeWire does not pair damage 1:1 with packets); empty-damage skipping lives in OPT-048.
- `OPT-002` ABANDONED 2026-09-15 — damage-first send with rectangle coalescing: removed, the encoder produces full-frame pictures (full-surface AVC420 per upstream).
- `OPT-003` ABANDONED 2026-09-15 — packet/metadata FIFO pairing: removed, it mis-paired metadata to packets.
- `OPT-004` ABANDONED 2026-09-15 — tile activity classification / per-region quality bias: removed, never reached the encoder.
- `OPT-005` ABANDONED 2026-09-15 — progressive refinement after motion: removed, it only relabelled normal frames.
- `OPT-006` ABANDONED 2026-09-15 — RTT congestion adaptation (frame rate + QP bias): superseded by upstream `978f1cb` and by OPT-016.
- `OPT-007` DONE — H.264 Main preference with profile fallback (fork value over upstream).
- `OPT-008` ABANDONED 2026-09-15 — first AVC444 negotiation scaffold: removed; AVC444 redone as OPT-045.
- `OPT-009` DONE 2026-09-16 — multi-monitor protocol layout: per-monitor `ResetGraphics` layout, and one RDPGFX surface per monitor since OPT-018 (`MonitorMode=multi`).
- `OPT-010` ABANDONED 2026-09-15 — first AVC444 wire path (`LC=1/2`, broken): deleted; redone as OPT-045.
- `OPT-011` DONE — automatic VA-API encode-device selection at startup and on config change; the libx264 fallback/stall watchdog was removed 2026-09-15.
- `OPT-012` ABANDONED 2026-09-15 — RDPGFX tile/content cache: removed, inert with full-frame encode (FreeRDP also crashed on SurfaceToCache).
- `OPT-013` DONE 2026-02-20 — `VaapiDriverMode` (`auto|off|radeonsi|iHD`) in config and KCM.
- `OPT-014` DONE 2026-02-20 — startup summary log line and smoke-test encoder assertions.
- `OPT-015` DONE 2026-09-16 — private KPipeWire (`~/dev/kpipewire` `westers/opt-015`): h264_vaapi CQP, async_depth=1, quality->QP 40..12, gop 600; patch series in `~/dev/rdp/kpipewire-vaapi-fix/`.
- `OPT-016` DONE 2026-09-20 — adaptive quality on pressure only (RTT inflation or ack backlog; `Quality` is a cap, `AdaptiveQuality` key), 20 s backlog grace for fresh AVC444 connections (`f5fbe8d`); QoE acks remain a stub.
- `OPT-018` DONE 2026-09-16 — `MonitorMode=multi`: one session, encoder and RDPGFX surface per monitor with workspace-global input (`544f340..11c94ab`); `/multimon` for stock clients is OPT-040; ledger `.superpowers/sdd/2026-09-16-opt-018-per-monitor-surfaces/`.
- `OPT-023` OPEN — frame pacing: frame-repeat disabled for h264_vaapi (done 2026-09-16); QoE-driven pacing (`gfxQoEFrameAcknowledge`, still a stub) outstanding.
- `OPT-026` DONE — the fork's own Linux RDP client with multi-monitor support (`~/dev/krdp-client`, version 0.5.0); no Windows client (Steve, 2026-09-16).
- `OPT-035` DONE 2026-09-21 — IDR on surface (re)create via `PipeWireBaseEncodedStream::requestKeyFrame()` (restart fallback for stock KPipeWire); idle-console cached-picture fix KPipeWire `01c6f99`.
- `OPT-037` OPEN — `krdp-monitors` visibility record: client names the monitors it shows, server disables hidden monitor sessions and forces an IDR on re-enable; not started.
- `OPT-039` OPEN — adaptive quality: a step-down reopens the encoder (IDR per surface) and compounds a no-ack stall; defer step-downs while no acks arrive; not started.
- `OPT-040` OPEN — client-layout mapping for stock `/multimon` clients in `MonitorMode=multi` (client layout in `ResetGraphics` + `MapSurfaceToScaledOutput`, caps >= 10.3); superseded for `virtual` by OPT-041.
- `OPT-041` DONE 2026-09-18 — `MonitorMode=virtual`: client-sized KWin virtual outputs mirroring the client (or physical) layout, `replace`/`extend` policy with `PhysicalOutputGuard` and desk takeover (`8497595..046126d`, S1-S5); follow-ups: OPT-041c live resize, OPT-041d `desktopScaleFactor` -> KWin output scale, Plasma orphan containments.
- `OPT-042` OPEN — pointer position: verify whether the client cursor is drawn at the wrong place in `virtual` before deciding on a fix; not started.
- `OPT-043` OPEN — fork KCM and packaging: WS-K (2026-09-27, on master) put every `krdpserverrc` key on the page from one kcfg (`server/krdpserversettings.kcfg`, `c6768c6..c866c00`); the `krdp` system package (`aud-pkg`, not merged or installed, `~/dev/rdp/PACKAGING.md`) replaces and conflicts with stock KRDP, so the coexistence requirement below is still unmet.
  - 2026-09-21 requirement (Steve; restored 2026-09-27 from `~/dev/rdp/archive/2026-09-27-maintenance-guard-wip.patch`): a distinct KDE settings page that cannot overwrite/shadow the stock KCM or touch stock config/services; cover every supported user-facing server setting (adaptive quality, codec/chroma, audio priority, camera bridge, virtual-monitor policy, console/persistent-desktop/GPU policy as they land); label per-user vs system-host scope, server default vs client override, live vs restart-required; unsupported features must not look functional; migrate existing settings explicitly without modifying stock KRDP; acceptance = both packages installed together, each page affecting only its own server.
  - Ongoing delivery requirement (restored 2026-09-27): every added/changed user-facing server setting updates this KCM, validation/help/defaults, tests and the matching server package in the same delivery; audit config-to-UI coverage so the page never falls behind.
- `OPT-044` DONE 2026-09-19 — KRDPCTL private channel and layout control (owner/viewers, Fit stand-ins, Private, live apply as a diff, desk takeover), now protocol v2 (`~/dev/rdp/KRDPCTL-V2-CONTRACT.md`); ledger `.superpowers/sdd/2026-09-19-slice-2c-server-layout-control/`.
- `OPT-044` console topology FROZEN 2026-09-27 — console/multi topology experiments behind `KRDP_EXPERIMENTAL_CONSOLE_TOPOLOGY`, `KRDP_EXPERIMENTAL_CONSOLE_VIRTUAL` and `KRDP_EXPERIMENTAL_MULTI_{RESIZE,PRIMARY,MIXED}` (`ConsoleHostController`, `VirtualSessionTransport`, `RetainedMulti*Plan.h`): code stays off by default, no further work (Steve); 2026-09-22..24 notes in the archive patch above.
- `OPT-044` retained virtual desktops OPEN — per-user retained virtual desktops on the virtual host (selected-layout create `28a71fc`, Add/preview `6e06fbc`/`67487f9`); isolated Sol/Buzz acceptance done, native Gate 2 is next, then Gate 3; Hal untouched.
- `OPT-045` DONE 2026-09-19 — AVC444/AVC444v2 over RDPGFX (`Codec=auto|avc420|avc444`, two encoder contexts one stream, chroma shed as the first adaptive rung); follow-ups: skip the aux split while chroma is shed; spec `docs/superpowers/specs/2026-09-19-opt-045-avc444-design.md`.
- `OPT-045b` DONE 2026-09-20 — tunable AVC444 chroma policy (`Avc444MotionGapMs/RestMs/MaxGapMs`, KRDPCTL `chroma` override) and the pairer fix (KPipeWire `0019`); open: `auxMaxGap` in the cost line.
- `OPT-046` DONE 2026-09-20 — private HEVC/AV1 stream for the own client (vendor RDPGFX `0x8001/0x8002`, ordered allow-list, slow latency-driven codec step `18a5f94`); open follow-ups: NVENC on the 4090 with GPU admission by PCI id/VRAM headroom, zero-copy luma-only AVC444.
- `OPT-047` OPEN — KWin `DrmGpu::pageFlipHandler` crash during virtual-output creation after churn: mitigation 1 (serial creation, 400 ms gap) implemented; mitigation 2 (fewer physical off/on cycles) open.
- `OPT-048` DONE 2026-09-19 — empty-damage frames skipped before encode (KPipeWire `a181fa2`, patch `0011`); open: the idle-desk saving was not measured, upstream KWin MR not filed.
- `OPT-049` OPEN — on Hal, a layout release that coincides with the lock screen appearing kills the greeter and the lock is not re-armed; first item after the audit; until fixed, check `LockedHint` after output-changing runs.
- `OPT-050` OPEN — conferencing redirection for the own client (RDPSND playback, AUDIN mic -> per-session PipeWire source, RDPECAM camera -> PipeWire/V4L2); browser WebRTC and console-mic acceptance done, device on/off now per WS-D below; open: an external bidirectional call, console camera; evidence `~/dev/rdp/evidence/console-audio-client.yI3kMD/`.
- `S1` DONE `29277e8` — clipboard bridge on the Plasma session.
- `S2` DONE `dd416c8` — clipboard echo suppression, one request per format list, CRLF/LF normalisation.
- `S3` DONE `046126d` — `multi` ignores `Virtual-*` screens.
- `S4` DONE — `ClientDisplay::sanitize()` bounds the client union by the 8192-px RDP limit, not 4096.
- `S5` DONE — `VirtualMonitorLayout=physical` mirrors the host's own layout.
- `S6` DONE 2026-09-19 — `Encoder::finish()` without a frame no longer SIGSEGVs (KPipeWire `d722ed8`/`7d51a92`, patches `0009`/`0010`).
- Maintenance guard ABANDONED 2026-09-27 — virtual-session maintenance guard (`VirtualSessionMaintenance*`, `PackageLease`, unattended-upgrade guard, `krdp-virtual-maintenance`) deleted from master (AUD-C1 `8e13f63`); source at tag `archive/maintenance-guard`, WIP in `~/dev/rdp/archive/2026-09-27-maintenance-guard-wip.patch`.
- AUD-2026-09-27 audit DONE 2026-09-28 — security/pre-auth (WS-S), physical session (WS-P), console broker/worker (WS-C), KCM/settings (WS-K), KRDPCTL v2 (WS-L), devices (WS-D), software HEVC/AV1 fallback (WS-E) and the live-test fixes AUD-FIX…AUD-FIX11 (F1–F5, U1–U6, C1, N1, D1–D5, R1, R4–R6); final: server `7148bb5`, client `v0.5.5`, FreeRDP `+h264.2`, all held on all six hosts; plan and outcome `~/dev/rdp/AUDIT-FIX-PLAN.md`, evidence `~/dev/rdp/evidence/2026-09-28-release-7148bb5/`; open backlog there.
- WS-D devices DONE 2026-09-27 — playback, microphone and camera switch on/off during a session (KRDPCTL `device` record, `StandardClientMedia` for stock clients, console/virtual broker mapping): server on master (`3f8a18c..72cdffe`), client 0.5.0 Devices menu; open: mstsc, a real camera and live Sol/Buzz toggling; design `~/dev/rdp/DEVICES-DESIGN.md`.
- AUD-FIX2 client-acceptance fixes DONE 2026-09-27 (deployed with `7148bb5`) — F1 codec: per-codec hardware/software encoder probe (`src/EncoderSupport*`), `SoftwareEncoding=auto|never|prefer` policy with slow-link/CPU-guard/anti-flap (`src/CodecPolicy.h`), `codec` answers only what the host can encode and the codec id always matches the encoder, `capabilities.video`; F2 a KRDPCTL v2 client on :3395 is never auto-bound (`codec` answered AVC); virtual device revocations use code `detached`; takeover sends `session-end` `opened-elsewhere` before 0x5 and explicit `attach` takes over the user's other connection; F5 KPipeWire `DmaBufHandler` EGL/GBM leak (sync_file per software session) fixed in KPipeWire `aud-fix2-dmabuf-egl` `e31f7e8`; contract `~/dev/rdp/KRDPCTL-V2-CONTRACT.md` (e)/(f); evidence `~/dev/rdp/evidence/2026-09-27-audit-phase3-client/`.
- Stock clients on the virtual host DONE 2026-09-27 — a client without KRDPCTL attaches the user's most recent virtual desktop or creates one from its monitor data; `VirtualStockClientPolicy` (KCM combo, `refuse` -> ERRINFO 0x7), opt-in MS-RDPEDISP resize (`54de0d2`); open: live sdl-freerdp3 test on Sol/Buzz.

## Runtime settings (quick reference)
- Every `krdpserverrc` key, its default and help text live in `server/krdpserversettings.kcfg` (single source, shown on the KCM page since WS-K); the archive copy has the older annotated list.

### Runtime Environment Variables
- `KRDP_FORCE_VAAPI_DRIVER=<driver>`: force VAAPI driver selection in KRDP startup/device probing.
- `KRDP_AUTO_VAAPI_DRIVER=0`: disable KRDP automatic VAAPI driver selection.
- `KPIPEWIRE_FORCE_ENCODER=libx264`: honoured by KPipeWire itself if the user sets it; KRDP sets it only for `KRDP_FORCE_SOFTWARE_ENCODING` (below) and never overrides a value the user set.
- `KRDP_FORCE_SOFTWARE_ENCODING=1` (AUD-FIX2): no hardware encoder is used or advertised; KPipeWire H.264 forced to libx264 (or libopenh264).
- `KRDP_ENCODERS=avc=hw+sw,hevc=hw,av1=none` (AUD-FIX2, tests/diagnosis): replaces the encoder probe's answer per codec (`hw`, `sw`, `hw+sw`, `none`).
- `KPIPEWIRE_DMABUF_RENDER_NODE=/dev/dri/renderD128` (private KPipeWire, tests): DmaBufHandler uses its own GBM display on that node instead of the application's EGL display.
- (removed 2026-09-15) `KRDP_EXPERIMENTAL_AVC444*` / `KRDP_EXPERIMENTAL_TRUE_AVC444` / `KRDP_ENABLE_TILE_CACHE` / `KRDP_ENABLE_STALL_WATCHDOG`: the features behind these flags were deleted.

### Current Display-Change Recovery Behavior (2026-09-15)
- Display geometry/topology changes trigger a screencast re-create that goes through the deferred `attachEncodedStream()` path (it waits for KPipeWire's produce thread to tear down — `nodeId()==0` — before calling `start()` again).
- Config reloads (e.g. a quality-slider write) are decoupled from display-refresh: a quality change never touches the stream.
- The software-fallback / hardware-retry / stall-watchdog policy was removed: it could not restart a KPipeWire 6.6 stream and leaked `KPIPEWIRE_FORCE_ENCODER` process-wide. KPipeWire's own internal VAAPI→libx264 fallback still applies at encoder init.

The February research notes (capture path, PipeWire damage findings, private-prefix build guide, ICA/Thinwire patterns, references) are in the archive copy.
