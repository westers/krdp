# Console "Replace": map N host screens onto M client monitors (design, 2026-10-06, OPT-060 follow-up)

Status: APPROVED by Steve 2026-10-06 (see "Decisions" below; where a decision differs from the text, the decision wins and
the text has been corrected). Amends
`2026-10-05-console-replace-screens-design.md` (S0, §8). Console route (:3391) only; two routes stay, no :3389.

## Verdict and model (read this first)

- **Steve's model is buildable, and the old route proves it.** The old per-user server did exactly this for him
  on 2026-09-29 (mechanism B, KRDPCTL `layout apply` "Fit"): one stand-in per Hal screen, each at the size of the
  Buzz monitor it was fitted to, placed at that Hal screen's own position (`CreateStandIn DP-1 1920x1080 at 0,0`,
  `HDMI-A-1 1920x1080 at 2560,0`; planner `src/LayoutControl.cpp:704-737`). The client still has the matching
  per-connection model (`Mapping::HostSetting.fitTo`, `core/Mapping.h:63-70`). Console never got it.
- **Correction to the brief:** `VirtualMonitorLayout=physical` was NOT "one output per host screen sized for its
  client monitor". It mirrors the host's own sizes and scales (archive research S5, `ConsoleVirtualOutputPlan.h:73-93`
  uses `output.size`/`output.scale`; KCM help `brokerpreferences.cpp:208` "copies the computer's own screens,
  including their resolution and scale"). The client-sized-per-host-screen behaviour was mechanism B. Notes and code
  agree with each other; only the brief was wrong, so this is not a stop condition.
- **What Console Replace creates today:** one output per CLIENT monitor (`Layout::Client`), or one output for a
  one-monitor client (D0 request). Hal's N screens are switched off and their windows land wherever KWin puts
  them. There is no per-host-screen identity, so "toggle between Hal's two screens on one Buzz monitor" is
  impossible today: Hal's two screens collapse into one 1920x1080 output.
- **Model (confirmed, with three precisions):**
  1. Each enabled host screen h (N of them) gets one owned virtual output `Virtual-krdp-h<i>-WxH`, sized and scaled
     like the client monitor it is mapped to. Host screens are off during the connection and restored after
     (existing guard, journal, S3 lock guard, fail-open). Unchanged consent (D1-D5).
  2. Each host screen maps to exactly one client monitor; a client monitor shows 0..k host screens, one at a time.
     No tiling. The user cycles with **host key + PgUp / PgDown** (default host key Right Ctrl,
     `Shortcuts.cpp:18,41-42`), per window (`SessionView.qml:279-289`). Cycling must become "within this monitor's
     group" (today it wraps over all remote monitors, `SessionModel.cpp:709-718`).
  3. Default mapping: host screens ordered primary first then left-to-right, client monitors the same;
     `host[i] -> client[min(i, M-1)]`. Host primary always on client primary, never cycled away; extras share the
     last monitor. User-editable per connection in the existing Monitors panel, keyed by host connector name.
- **Encode policy:** stream only what some client displays (broker filter first, wire 13; worker pause later,
  wire 15 or later: wire 14 is taken by M-2), **hard cap 4 replaced host screens** (no 8; 16-output creation bound). Cycle switch target
  p95 <= 250 ms with encoders running. All numbers marked "measure" are unproven.

---

## Decisions (Steve, 2026-10-06)

| # | Decision |
|---|---|
| Q1 | Default mapping `host[i] -> client[min(i, M-1)]`, host primary on client primary first (§4.2). |
| Q2 | Hidden host screens stop streaming by default; an Advanced "keep streaming" switch exists (M-4; the planner and wire do not preclude it). |
| Q3 / Q5 | **Hard cap of 4 replaced host screens**, the default AND the maximum (the text used 4 default / 8 max). Above 4 the planner refuses and the caller shows the screens as they are. No host setting. |
| Q4 (client hot-plug) | Notice + Reconnect first; live re-map later (M-9). |
| Q4 (spec) | A client monitor with no host screen mapped shows a **"No host screen mapped" panel on the CLIENT**, not a host concern. The planner only reports which client monitors are unmapped. |
| Q6 | Host output scale follows the client monitor's scale (per-monitor scale in the planner and policy). |
| Q7 | Host monitor plugged/unplugged while replaced: restore and continue as extend (M-8). |
| Q8 | Stock `/multimon` clients keep one output per client monitor (`Layout::Client`, unchanged). |

M-1/M-2 findings: the planner treats an unknown host name in the mapping as ignored-and-reported (§7), an unknown client
monitor id as a refusal; wire 14 carries the mapping; the configured-output creators DO each run an encoder (see §4.3 check).

## 1. Today's building blocks vs the N->M model

Status words: **shipped** = in installed packages (`e9231e8` fleet, `a6626e6` on Sol), reachable; **dormant** =
shipped, off by default; **source-only** = committed after the installed package (`54841663`, client `135f46c`,
not installed); **unreachable** = shipped only in the retired :3389 server; **absent** = not written.

| # | Building block | Where | Status | What it gives the N->M model |
|---|---|---|---|---|
| B1 | Standard monitor block -> Replace, `Layout::Client`: one output per client monitor at client positions, anchor (0,0) | `ConsoleVirtualOutputPlan.h:103-108`; gate `ConsoleVirtualOutputPolicy.h:213-220`; broker `ConsoleHostController.cpp:1000-1028` | shipped (S2), accepted S7 two simulated monitors | Wrong cardinality when N != M; no host identity |
| B2 | One-monitor request `console-screens-request` v1 -> one output of the client size | `ConsoleHostController.cpp:1449-1478`; `effectiveRequest` `ConsoleVirtualOutputPolicy.h:202-210`; client `ConsoleScreens.h:131-152` | source-only (D0); live run = S7b, in progress | Trigger path and timing reused; layout must change |
| B3 | `Layout::Physical`: one output per host screen at HOST size/scale | `ConsoleVirtualOutputPlan.h:73-93` | dormant (user preference `VirtualMonitorLayout=physical`) | The per-host-screen loop and order (priority sort, primary = index 0) to copy; sizing differs |
| B4 | `Layout::Single`, fallback 1920x1080, `forceSingle` after a multi failure | plan `:95-102`; worker `failConsoleVirtual` `consoleworker.cpp:660-668` | dormant | Fallback ladder |
| B5 | Serial creation, 400 ms settle, mode repair, full-tuple recheck, then one `applyReplace(placements, primary)` batch | `consoleworker.cpp:671-721, 900-997` | shipped, S7 20/20 | Unchanged; N outputs = N x settle |
| B6 | Guard, journal, restore, desk reclaim (continue as extend), one attempt per connection, S3 lock guard | `PhysicalOutputGuard*`, `OutputRestoreJournal*`, `consoleworker.cpp:839-897, 3153-3183` | shipped; S7 rows 2-8 PASS | Unchanged |
| B7 | Per-output capture: one `PlasmaScreencastV1Session` per output, RDP surface index = worker output order | `consoleworker.cpp:1577-1607`; `ConsoleFrameLayout.h:16-19` | shipped (Hal runs 2 HEVC encoders today) | N streams, N surfaces |
| B8 | Broker forwards every frame to every admitted client; keyframe request has no output selector (all outputs) | `ConsoleHostController.cpp:291-314`; `consoleworker.cpp:3355-3359` | shipped | Per-client visible filter is a broker-only change |
| B9 | Owned resize/Fit of configured outputs (resize one owned output, exact readback) | `aec0fbbd`, `d5206b30`, `RemoteTopologyFit.h` | dormant; real-RDP Fit on Sol failed (CUDA OOM, `2026-10-01-t06-owned-rdp`) | Live re-map of one host screen to another client monitor without physical churn |
| B10 | Stand-in per real monitor at a requested size, at the real monitor's position (mechanism B) | `src/LayoutControl.cpp:704-737`, `server/HostLayoutExecutor.cpp` | unreachable (:3389 retired 2026-10-03) | The exact semantics wanted; positions rule to copy |
| B11 | Host inventory to the client before Replace: v1 `layout` record of the captured outputs (ids = connectors, kind real, size, position, scale, primary) and `topology-query` | `ConsoleHostController.cpp:1960-1991, 1419-1440`; `capabilities.layoutQuery=true` `:1936` | shipped | Client learns host screen names; during Replace the record lists the virtual outputs instead |
| B12 | Client scale in the request | `VideoMonitor` has no scale (`src/SurfaceLayout.h:22-30`); request has no scale field (contract (h)) | absent | Needed for 125% monitors |
| B13 | N->M sizing, mapping, naming, arrangement, visible filter, group cycling | - | **absent** | This design |

**Answer to Q1.** (a) A standard block creates one output per client monitor (B1). (b) The one-monitor request
creates one output at the client monitor's size (B2). (c) One output per host screen sized for its mapped client
monitor exists only in the retired :3389 server (B10). In the T06 Console plan it is absent; `Layout::Physical`
(B3) has the right loop with the wrong sizes.

## 2. What the client already does (Q2)

| Topic | Today | Gap for N->M |
|---|---|---|
| Mapping model | `Mapping { privateMode, hosts[{host, lit, fitTo, scale, virtualSize}], edges[{host, screen, sizing, presentation}] }` per connection (`core/Mapping.h:52-95`, stored in `Connection::mapping`, `Connection.h:138-147`, with `cachedHostLayout` and `screenIdentities` to find screens again) | `fitTo` = "size this host screen for that client screen" is exactly the mapping; on Console it is disabled ("host-side controls disabled on direct Console", `MonitorsPanel.qml:19-23`) |
| Mapping editor | Monitors panel: host boxes over client boxes, click host then client to link, edge pill (Scaled/1:1, Windowed/Full screen, Fit, remove), in Edit PC and Session > Monitors… (host key+O) (`MonitorsPanel.qml:1-23`, `MonitorsDialog.qml:1-12`) | Needs a "Replace" mode: one assignment per host screen (fitTo + full-screen edge together), default shown |
| Console path | Flow `Absent` (layout `apply` refused): edges only re-map windows locally and are remembered as the 2b per-monitor-set layout, never as a mapping (`AppLayout.cpp:487-516, 425-431`) | Mapping must be sent (request v2) and persisted for Console Replace |
| Views | A View = one window showing one remote monitor (or all); remembered per monitor-set fingerprint; no saved set = one window per remote monitor, windowed (`Views.h:283-330`, `AppLayout.cpp:343-399`) | N=10 would open 10 windows. Needs "one full-screen view per client monitor that has a group" |
| Cycling | host key+PgUp/PgDown = previous/next remote monitor in THIS window; wraps over all N remote monitors (`SessionModel.cpp:709-718`); switcher host key+M (`MonitorSwitcher.qml`) | Not group-scoped: window on monitor 1 can cycle onto a host screen also shown on monitor 2. No indicator |
| Scaled view | `viewMode` scaled/native per view | Used when a host screen is shown on a monitor smaller than its output (unplug, >4096 clamp) |
| Replace UI | Edit PC choice "Use this computer's monitors and turn the host's screens off" (`ConnectionForm.qml:542-556`, stored `requestClientMonitors`); banner + restore (`ConsoleScreensBanner.qml`); Fit refused while active (`AppLayout.cpp:526-533`) | Banner must name the host screens and the cycle key |
| Request | v1 sent once after `capabilities`, only Console + Replace + exactly ONE monitor (`ConsoleScreens.h:131-137`, commit `135f46c`) | v2 for any M, with mapping and scale |

## 3. Protocol (Q3)

### 3.1 Inventory before choosing

The client does not need a new inventory record before the first connect:
- **Saved mapping** is keyed by host connector name (`DP-1`), learned from earlier sessions (`cachedHostLayout`).
  It can be sent right after `capabilities`, with no round trip.
- **First connect, nothing saved:** the client omits `mapping`; the broker applies the default (§4.2) and reports
  the result in `console-screens` (`screens[]`, below). The client shows it in the panel; it is persisted only
  when the user changes something (so the default follows monitor changes).
- **Editing before connecting:** the Edit PC panel uses `cachedHostLayout` (B11 record captured "as they are").
  During Replace the v1 `layout` record lists the virtual outputs, so the host screen list comes from
  `console-screens.screens[]`.

### 3.2 Records and capability (KRDPCTL protocol stays 2; all additions optional)

| Record / field | Direction | Shape | Default / missing | Compatibility |
|---|---|---|---|---|
| `capabilities.console.screens.mapped` | S->C | bool | false | Old client ignores. Sent only with the rest of the group (permission Ask) |
| `capabilities.console.screens.maxScreens` | S->C | int 1..4 (always 4 today) | absent = 1 output per client monitor (old) | Client refuses to send a mapping larger than this; shows note |
| `capabilities.console.screens.view` | S->C | bool | false = broker forwards all outputs (today) | Old client never sends `console-screens-view` |
| `console-screens-request` **v2** | C->S | `{"type":"console-screens-request","v":2,"requestId":"..","replace":true,"layout":"mapped","monitors":[{"id":"eDP-1","x":0,"y":0,"width":1920,"height":1080,"scale":1.25,"primary":true}],"mapping":[{"host":"DP-1","monitor":"eDP-1"}]}` | `mapping` optional (missing or partial = default for the host screens not named); `scale` optional (1.0) | Sent only when `mapped` is true. v1 parser is strict, so an old server answers `ok:false`; the client never sends v2 there. v1 stays for one-monitor clients of `request`-only servers |
| v2 validation | | `monitors` 1..16, ids unique 1..64 chars, sizes 640..4096 (larger: see §7), scale 1..4 in 0.05 steps, one primary; `mapping[].host` unique connector names; `monitor` must be a listed id | Unknown host names are ignored and reported; bad shape = `ok:false` | Reply unchanged from v1 (`ok`, `message`, `requestId` echo) |
| v2 with a standard block | | Allowed for 2+ monitors: the block stays the standard request and consent; v2 adds layout and mapping. Its `monitors` rectangles must equal the block's (same set); else `ok:false` and the block alone applies (`Layout::Client`) | | Standards first: a stock client's block behaves exactly as today |
| `console-screens` v1 + `layout`, `screens` | S->C | adds `"layout":"mapped"` and `"screens":[{"host":"DP-1","output":"Virtual-krdp-h0-1920x1080","monitor":"eDP-1","width":1920,"height":1080,"scale":1.25,"primary":true,"default":true}]` with `active:true` | missing = old layout | Client ignores unknown fields (`stateFromJson` checks only the two flags) |
| `console-screens-view` | C->S | `{"type":"console-screens-view","v":1,"requestId":"..","visible":["Virtual-krdp-h0-1920x1080"]}` (0..16 output names this client shows now) | never sent = all visible | Reply `{ok, message?, requestId}`. Any admitted client, viewer too; affects only its own stream |
| `console-screens` reason `hostScreensChanged` | S->C | `active:false` | client must tolerate unknown reasons (contract (h)) | New reserved reason for host hot-plug (§7) |

No pre-auth records: all of these flow after `capabilities`, which is sent only after authentication (contract (b)).

### 3.3 The arming race for 2+ monitors

With a block the broker can arm Replace at worker Ready (`armConfiguredConsoleOutputs`, `ConsoleHostController.cpp:2061`)
before the v2 request arrives, and the one-attempt latch forbids a second plan. Rule: a connection that opened
KRDPCTL and was advertised `mapped` defers block arming until its v2 request is taken or **2 s after `capabilities`**
(measure; S7 shows capture-normal-first is already the norm). A stock client (no KRDPCTL) never waits. An old own
client (0.6.9, KRDPCTL but no v2) waits the grace once and gets `Layout::Client`.

### 3.4 Stock clients (document)

A stock `/multimon` client sends only the block: `Layout::Client` exactly as today (one output per client monitor in
the client's arrangement; Hal's windows land where KWin puts them). N==M looks one-to-one only if KWin's evacuation
happens to match. Mapped behaviour needs the own client. Optional later: a per-user `VirtualMonitorLayout=mapped`
that applies the default mapping to stock clients when N == M (decision Q8).

## 4. Server behaviour (Q4)

### 4.1 Plan: `Layout::Mapped` (new value in `ConsoleVirtualOutputPolicy::Layout`)

| Input | Source |
|---|---|
| Host screens H | `m_consoleOutputGuard.physicalOutputs()` (fresh snapshot), enabled only, ordered: priority 1 (primary) first, then by logical top-left (x, then y) |
| Client monitors C | v2 `monitors` (or block rectangles + v2 ids), ordered primary first, then (x, y) |
| Mapping | v2 `mapping`; unnamed host screens get the default |

| Output field | Rule |
|---|---|
| Count | `|H|` (not `|C|`). Refuse (fail open, plain capture) if `|H| > 4` (hard cap) or `|inventory| + |H| > 16` |
| Name | `Virtual-krdp-h<i>-<W>x<H>`, i = index in H order. New prefix so KWin's per-output-set store never replays a `Layout::Client` position (`krdp-m`) onto a mapped output |
| Pixels | target monitor's physical pixels; even; if a side > 4096, scale both down to fit 4096 (aspect kept, >= 640) and mark `clamped` |
| Scale | target monitor's `scale` (v2) or 1.0 (block only). Same logical size as the client's own desktop |
| Position (try A) | host screen's own logical top-left (mechanism B rule). Valid only if no two outputs overlap in logical space and the union fits 8192 per side |
| Position (else B) | pack: rows = host screens whose vertical ranges overlap, in H's spatial order; outputs edge to edge, top aligned, rows stacked; union <= 8192 else refuse |
| Primary | the output of the host primary |
| Fallback | create failure -> existing ladder: restore, then `forceSingle` once (one output at the client primary's size), then plain capture; never a second Replace |

Why position A first: `applyReplace` disables the physical screens and places the stand-ins in one KScreen batch
(`consoleworker.cpp:976-981`), so KWin evacuates each window from a removed output by geometry; a stand-in at the
same top-left keeps it on "its" host screen. Mechanism B did this on Hal (2026-09-29). **Measure** that windows land
on the matching output for A and B (§10 M-6).

### 4.2 Default mapping

`target(H[i]) = C[min(i, M-1)]`.

| N host | M client | Result | Cycling |
|---|---|---|---|
| 1 | 1 or 2 | H0 -> C0 | none; C1 shows nothing |
| 2 | 1 | H0, H1 -> C0 | C0 cycles 2 |
| 2 | 2 | H0 -> C0, H1 -> C1 | none |
| 3 | 2 | H0 -> C0; H1, H2 -> C1 | C1 cycles 2 |
| 5 | 2 | refused above the hard cap of 4 (screens shown as they are) | - |
| 4 | 2 | H0 -> C0; H1..H3 -> C1 | C1 cycles 3 |

Justification: the host primary carries the panel, notifications and most windows; keeping it alone on the client
primary means cycling never hides it. Round-robin (`i mod M`) balances groups but moves H2 onto the primary
monitor; offered as decision Q1.

### 4.3 Streams and the "encode only what is displayed" policy

| Phase | Mechanism | Wire | Saves |
|---|---|---|---|
| P1 (default when `view` is offered) | Broker keeps a visible set per client (`console-screens-view`; until the first one: the outputs that are group heads). Frames of other outputs are not forwarded to that client. On a set change: `RequestKeyFrame` (all outputs, existing empty payload), forward the newly visible output from its next keyframe only (same rule as `wireLayout`, `ConsoleHostController.cpp:308-310`) | 13 | Bandwidth, client decode. Not GPU |
| P2 (after measurement) | Worker pauses encoding of outputs no admitted client shows (stream kept, encoder idle, or `setStreamingEnabled(false)`); per-output keyframe request payload | 15 | GPU/encoder sessions |

- RDPGFX: one ResetGraphics with all N monitors when Replace becomes active; cycling never resets graphics.
  Hidden surfaces keep their last picture on the client.
- Viewers: each client has its own visible set; the worker (P2) encodes the union over admitted clients.
  Only the controller's request shapes the outputs (unchanged control model).
- Keyframes: a healthy KPipeWire encoder answers a keyframe request at once even on a still picture by re-encoding
  the last one (`src/EncoderWatchdog.h:27-31`); a wedged one is retried at 250 ms and restarted at 450 ms.
- **Expected cycle latency (to measure, M-3):** P1 = keyframe request + hardware encode + network + decode,
  about 50-150 ms on the LAN (target p95 <= 250 ms; VPN: + one RTT). P2 resume = a fresh stream's first frame,
  0.2-0.6 s (D2 probe `consoleFreshWorkerFirstFrame`), worst case about 1 s with the watchdog (target p95 <= 800 ms).
  Today (all streams forwarded) cycling is instant because every surface is already decoded; P1 trades that for
  bandwidth, so the client gets an Advanced switch "Keep hidden host screens streaming" (decision Q2).
- Cap: **hard 4** (default and maximum; Steve Q3/Q5). Reasons: the plan counts existing outputs in its 16 bound while creating
  (`ConsoleVirtualOutputPlan.h:112`; physical outputs are disabled only after creation, and disabling first would
  pass through KWin's no-output state, OPT-047), RDPGFX 16 monitors, creation time about N x (KWin create + 400 ms
  settle) (S7: 2 outputs, first frame at +4.7-5.1 s), unmeasured encoder load for N streams, and the OPT-047 KWin
  crash seen when creating three outputs. Raising the cap needs the creation bound changed and a KWin
  test with 2N outputs; not in this plan.
- Check during M-2: the configured-output creators are `PlasmaScreencastV1Session`s with streaming enabled
  (`consoleworker.cpp:934-952`) next to the capture sessions (`:1577-1607`); confirm they do not run a second
  encoder per output (2N encoders).
  **Finding (M-2, code reading, not measured):** they do. Each creator is a full `PlasmaScreencastV1Session` whose
  `setStreamingEnabled(true)` starts its own `PipeWireEncodedStream` and encoder (`AbstractSession.cpp:410-425`); the
  worker never connects `frameReceived` on creators (`consoleworker.cpp:934-952`), so those packets are dropped, but
  the encoder is created and fed. The creator's stream must stay active to keep the KWin output alive and resolved
  (`streamActive()` gate at `consoleworker.cpp:926`), so the encoder cannot simply be skipped without a KPipeWire
  "no-encode" stream mode or stopping the stream (which would retire the output). Not changed in M-2. Consequence:
  N mapped outputs cost 2N encoders (same as `Layout::Client` today with M outputs). Measure in M-2m; a
  creator-without-encoder mode belongs with M-4/M-11.

### 4.4 Input, cursor, lock, restore

| Topic | Rule |
|---|---|
| Input | Unchanged: absolute RDP coordinates in the monitor's atlas rectangle map to that output (existing per-output mapping). A view shows one output, so the pointer stays inside it; moving windows between host screens is a host shortcut (or cycling while dragging is not supported). Keyboard focus stays on the host's focused window after a cycle; no automatic focus change |
| Cursor | Unchanged: per-output vote on shape; the client draws its own pointer |
| Lock / greeter | Lock screen draws on all virtual outputs. S3 guard unchanged (S7 row 8 10/10). Greeter: never Replace. Measure release time against N (more creators to retire before the lock check) |
| Restore | Unchanged guarantees: journal of the physical layout, restore and verify at disconnect / worker exit / desk input / restore request, fail-open to plain capture, one attempt per connection, worker exits after release (D3, ~5.7 s window) |

## 5. Client behaviour (Q5)

| Topic | Design |
|---|---|
| Where | Edit PC (Console): next to "When I connect", a "Screen mapping…" button opens the Monitors panel in **Replace mode**. Live: Session > Monitors… (host key+O) |
| Replace-mode panel | Host screens as cards (from `cachedHostLayout` or `console-screens.screens[]`), client monitors as cards. Each host card has one target (click host, click monitor; or drag). One assignment = `fitTo` + full-screen edge on that screen. Default targets shown as "(default)". "Reset to default" per host and global |
| Stored | `Connection::mapping`, per host connector name -> client screen name, matched again via `screenIdentities`. Only user choices are stored. A saved target screen that is absent falls back to the default for that host screen |
| Views on connect | One full-screen view per client monitor that has a group, showing the group's first host screen (H order). No window on a monitor with an empty group (it stays the client's own desktop; decision Q4) |
| Cycling | host key+PgUp/PgDown cycles within the view's group (wrap). Group = host screens mapped to the view's client monitor. With one member: no-op, brief "Only one host screen on this monitor". host key+M switcher lists the group first, then "other monitors' screens" (picking one moves it into this group for the session, not saved) |
| Indicator | On each cycle and for 2 s after a Replace starts: overlay "Hal screen 2 of 3: HDMI-A-1" (bottom centre, fades). Same text in the window title tooltip |
| Visible set | After every view change the client sends `console-screens-view` with the output names currently shown in any of its views (debounced 50 ms). It keeps showing the previous picture until the new output's first keyframe is decoded (1 s timeout, then the stale picture with a small "updating" mark) |
| Scale | Request carries each monitor's scale (`ScreenLayout::Monitor::scalePercent`). Host output scale = client scale, so text has the same size as the client's own desktop |
| Clamped output | Shown Scaled on its monitor |
| Hot-plug, client monitor added | First delivery: nothing changes on the host; notice "A monitor was connected. Reconnect to use it for Hal's screens" with a Reconnect action. Later slice (M-9): live re-map via owned resize (B9) of the host screens whose default or saved target is the new monitor, then move their views |
| Hot-plug, client monitor removed | Local only: its group joins the client primary's group (session only), views of those outputs go Scaled; host output sizes unchanged; notice |

## 6. Settings and consent (Q6)

Unchanged: D1 (per-connection choice = consent), D2 (`ConsoleScreensPermission` default "When the connection asks"),
D3 (block from stock clients), D4 (desk input restores and keeps the session), banner + restore action. No new host
setting (Q5: the cap is a fixed 4, not configurable).
Banner text (mapped): "Hal's screens DP-1 and HDMI-A-1 are off while you are connected. Switch screens with
Right Ctrl+PgUp / PgDown." (host key name from settings). Notices also name the screen count when the cap refused
Replace: "Hal has 5 screens; replacing works for up to 4. Showing them as they are."

## 7. Edge cases and failure behaviour (Q7)

| Case | Behaviour |
|---|---|
| N < M | Extra client monitors get no group and no window |
| N = 1 | One output at its target's size; same as v1 one-monitor request when M = 1 |
| Mapping names a host screen that does not exist | Ignored; listed in the reply `message`; the panel marks it "not on the host now" and keeps it |
| Mapping names a client monitor not in `monitors` | `ok:false` (malformed); client never sends it (it filters by its current screens) |
| Host screen added mid-connection (Hal plugs a monitor) | Absent today. Rule: the worker sees a new enabled non-virtual output -> restore, continue as extend (desk-reclaim path), Replace over for the connection, `console-screens active:false reason:hostScreensChanged` |
| Host screen removed while disabled | Absent today: restore verification requires every journaled output back (`consoleworker.cpp:600-604`), so an unplugged connector would leave restore "unverified" and the journal kept (fail-open capture, next Replace refused). Needs a rule: a connector absent from the inventory counts as restored-unavailable and is dropped from the entry, logged. Slice M-8 |
| Mixed scales | Per output from its target; positions in logical space (B3's overlap check uses `RemoteMonitorGeometry::logicalRect`) |
| Client monitor > 4096 | Output clamped to fit 4096 keeping aspect; view Scaled |
| N > 4 (e.g. 5) | Request answered `ok:true` but Replace refused by the plan: plain capture (as they are), `console-screens active:false reason:failed` and a message. Never partial replacement |
| Union > 8192 after packing | Same as N > cap |
| Two clients | Controller's request shapes outputs; viewers see the same N outputs, keep their own grouping (local) and visible sets. A viewer's v2 request is stored and used only if it gets control on a fresh connection (existing rule: control change releases Replace) |
| Steve at the desk | Desk input restores the screens; remote continues on the virtual outputs as extend (S7 row 7 semantics); mapping irrelevant afterwards |
| Lock during cycling | No interaction; lock screen shows on every output |
| Encoder session limit hit (NVENC consumer cap, VAAPI open failure) | Existing per-output failure ends capture: restore + plain capture (fail-open); cap prevents it in practice. Measure on Hal (AMD VCN) and Sol (NVENC) |
| Grace timeout passes before the v2 request | Block arms `Layout::Client`; the late v2 is answered "came too late" (existing message) |

## 8. Sequences

**Connect (own client, Buzz 1 monitor, Hal 2 screens, permission Ask)**
1. TLS + PAM; worker captures Hal's 2 screens as they are; broker sends `capabilities` with
   `screens {replace, restore, request, mapped, maxScreens:4, view}`.
2. Client sends v2 `{layout:"mapped", monitors:[eDP-1 1920x1080 s1.25], mapping:[saved or none]}`; broker validates,
   stores, `updateClientDisplayPolicy` (Layout::Mapped), `armConfiguredConsoleOutputs`; reply `ok:true`.
3. Worker: guard hold, journal, plan {h0 = DP-1 -> 1920x1080@1.25 at 0,0; h1 = HDMI-A-1 -> 1920x1080@1.25 at
   2560,0 (A) or 1536,0 (B)}, create h0, settle, create h1, settle, recheck, `applyReplace` (physical off, placements,
   primary h0), multi-capture of 2 outputs.
4. Broker: readback, ResetGraphics 2 monitors, `console-screens {active:true, reason:connect, layout:"mapped",
   screens:[...]}`; banner.
5. Client: one full-screen view on eDP-1 showing h0, indicator "Hal screen 1 of 2: DP-1"; sends
   `console-screens-view {visible:[h0]}`; broker stops forwarding h1.

**Cycle** host key+PgDown in the eDP-1 view -> view shows h1's surface (last picture) -> client sends
`view {visible:[h1]}` -> broker `RequestKeyFrame`, forwards h1 from its next keyframe, drops h0 -> client swaps to
the decoded keyframe; indicator "2 of 2: HDMI-A-1". Target p95 <= 250 ms (P1).

**Client hot-plug (portable monitor added)** first delivery: notice + Reconnect. Reconnect = new connection: old one
restores (physical back about +1 s, worker exit, new worker about +5.7 s), new one gets its own attempt with M = 2:
h0 -> eDP-1, h1 -> portable. Later (M-9): owned resize of h1 to the portable's size, view moves; no physical churn.

**Host hot-plug** -> worker restore, extend, `active:false reason:hostScreensChanged`, banner gone, remote keeps the
virtual outputs as extend until disconnect; no new Replace on this connection.

**Restore** (disconnect, kill, cut, worker kill, broker restart, restore request, desk input): unchanged from S0/S7.
Release retires N creators; S3 lock check; worker exits; broker respawns it.

## 9. Slices (Q8)

Dependencies: "now" = can start before S7b reports; "S7b" = needs the S7b run (D0 one-monitor request live, D2
row 5 re-run) to pass, because the mapped request reuses the D0 arming path and the D2 surface reset.

| Slice | Content | Done when | Tests (existing -> new) | Start | Risk |
|---|---|---|---|---|---|
| M-0 | Steve signs this spec and answers Q1-Q8 | Decisions recorded in this file | - | now | Low |
| M-1 | Pure planner `Layout::Mapped`: order, default mapping, sizes, clamp, scale, names `krdp-h`, positions A then B, caps | `ConsoleVirtualOutputPlanTest` new rows pass: N=1..4 x M=1..3, Hal 2x2560 + Buzz 1920@1.25, overlap -> pack, union > 8192 refuse, unknown host name, N > 4 refuse, mixed scales, >4096 clamp, `forceSingle` | ConsoleVirtualOutputPlanTest (16 rows today) | now | Low |
| M-2 | Policy + worker wire 14 (taken by M-2; M-11 pause would be 15): `Layout::Mapped`, mapping, per-monitor scale in the serialized policy (`ConsoleWorkerWire.h:1476-1522`); creator encoder check (§4.3) | ConsoleWorkerWireTest round trip + version-mismatch rows; broker/worker still ship together | ConsoleWorkerWireTest, ConsoleHostLifecycleTest | now | Medium (paired wire bump) |
| M-3 | KRDPCTL v2 request, capabilities `mapped/maxScreens/view`, `console-screens.screens[]`, 2 s grace for block arming | LayoutControlTest malformed/valid v2 rows; ConsoleHostControllerTest: v2 alone, v2 + block equal, v2 + block different, grace expiry, late v2, viewer v2, permission Off, greeter | LayoutControlTest (71), ConsoleHostControllerTest (57) | S7b | Medium |
| M-4 | Broker visible filter P1 + `console-screens-view` | ConsoleHostControllerTest: per-client filter, keyframe gating on switch, viewer independence, no record = forward all | ConsoleHostControllerTest | now (pure) | Low-Medium |
| M-5 | Client: v2 request, Replace-mode panel, group views, group cycling, indicator, view record, banner text, notices; version 0.7.0 | MappingTest (default, saved, absent screen), ViewsTest (group views), SessionModelTest (cycle within group, wrap, k=1), ConsoleScreensTest (v2 JSON, screens[] parse), MonitorsDialogTest offscreen screenshots | MappingTest, ViewsTest, SessionModelTest, ConsoleScreensTest, MonitorsDialogTest | now (model), S7b (send) | Medium |
| M-6 | Sol private fixture (`WorkerEndToEndTest consoleConfiguredOutputs`): 2 private outputs mapped onto 1 and onto 2 client monitors, mixed scale; windows-landing check A vs B; release + lock rows | Fixture rows pass; window marker (Konsole title) on the matching output in decoded frames for A; decision A/B recorded | WorkerEndToEndTest (Sol only) | after M-1/M-2 | Medium |
| M-7 | Sol installed, one real panel, Buzz client: N=1 x M=1,2 (simulated second monitor), cycling latency, 20 cycles, S7 rows 2-8 on mapped | All S7 rows pass on mapped; M-3 latency p95 recorded; kscreen-doctor before/after identical | Native matrix | after M-3..M-5, S7b | Medium |
| M-8 | Host hot-plug rules (added -> restore; removed -> restored-unavailable) | Pure journal rows + Sol private fixture with an output added/removed while replaced | OutputRestoreJournalTest, WorkerEndToEndTest | now (pure) | Medium |
| M-9 | Live re-map via owned resize (client hot-plug, panel change during Replace) | Fixture + Sol: one output resized, view moved, no physical change; the 2026-10-01 CUDA OOM Fit failure explained or fixed first | RemoteTopologyFitTest, client RemoteTopologyClientStateTest | later | Medium-High |
| M-10 | Two real panels as host: Buzz as Console server (eDP + portable), Sol's client with 1 and 2 monitors | N=2 x M=1,2: both panels restored every row; cycling; window landing | Native | after M-7; Steve plugs the monitor | Medium |
| M-11 | P2 worker pause (wire 15), only if M-7 shows GPU/encoder need | Resume p95 <= 800 ms; GPU drop measured | WorkerEndToEndTest | later | Medium |
| M-12 | Supervised Hal run at the desk, LAN: Buzz 1 monitor then 2; then VPN | S0 §5 step 5 pass criteria + both Hal screens on Buzz's one monitor with cycling; Steve's sign-off | Native | last | Medium-High |
| M-13 | Docs: contract (h) v2, research OPT-060, HANDOFF Log, `~/dev/rdp/CLAUDE.md` Fixed, settings-ui notes | Written | - | with each slice | Low |

Versions: worker wire 13 -> 14 (M-2; broker and worker paired as always; no other component). KRDPCTL protocol stays
2 (optional fields). Client 0.6.9 -> 0.7.0. Old pairs: new server + 0.6.9 client = today's behaviour (block or v1);
0.7.0 client + old server = no `mapped`, client uses v1/block and says "this host shows one screen per monitor".

## 10. What must be measured (unproven today)

| ID | Measurement | Where | Pass |
|---|---|---|---|
| M-1m | Connect-to-first-frame vs N (1, 2, 4, 8 outputs) | Sol fixture, Sol installed | Recorded; N=2 <= 6 s |
| M-2m | GPU/encoder load and encoder session count vs N, idle and one moving output; creators encoding or not | Sol (NVENC), Buzz/Hal-class AMD later | No session failure at the cap |
| M-3m | Cycle latency P1 (keyframe), P2 (resume), LAN and VPN | Sol + Buzz | p95 <= 250 ms (P1), <= 800 ms (P2) |
| M-4m | Bandwidth of a hidden moving output, P1 on vs off | Sol + Buzz | Hidden output costs ~0 with P1 |
| M-5m | Release time vs N, with the S3 lock check | Sol | Restore verified, lock kept, release < 5 s at N=4 |
| M-6m | Windows land on the matching output (A) / packed (B) | Sol fixture, then two panels | Window marker on the right output |
| M-7m | KWin stability creating N outputs after a release (OPT-047) | Sol fixture | 20 cycles at N=4, no KWin crash |
| M-8m | Grace window: time from `capabilities` to v2 arrival | Sol + Buzz logs | p99 < 2 s |

## 11. Decisions for Steve

| # | Question | Recommended answer |
|---|---|---|
| Q1 | Default mapping: extras on the last monitor (`min(i, M-1)`, host primary alone on your primary) or round-robin (`i mod M`)? | `min(i, M-1)` |
| Q2 | Hidden host screens: stop streaming them (about 0.15 s switch, less bandwidth) or keep all streaming (instant switch)? | Stop streaming by default, with an Advanced "keep streaming" switch |
| Q3 | You plug a second monitor into Buzz mid-session: notice + one-click Reconnect now, live re-map later? | Yes |
| Q4 | A Buzz monitor with no Hal screen mapped: leave it as Buzz's own desktop, or a black full-screen window? | Leave it as Buzz's desktop |
| Q5 | Cap on replaced host screens: 4; above it, show the screens as they are (no partial Replace)? | Yes; no host setting |
| Q6 | Host output scale follows the client monitor's scale (Buzz 125% -> Hal text the same size as on Buzz)? | Yes |
| Q7 | Hal plugs/unplugs a monitor while replaced: restore Hal's screens and continue as extend for that connection? | Yes |
| Q8 | Stock `/multimon` clients: keep one output per client monitor (today), or the default mapping when N == M? | Keep today's behaviour |

## 12. Contradictions and unproven items

- Brief vs notes/code: `physical` layout semantics (see Verdict). Code and notes agree.
- Contract (h) says the v1 request "only for a client with ONE monitor" and the block "wins"; this design allows a v2
  request alongside a block (it adds the mapping, the block stays the consent). Contract text must change with M-3.
- Unproven: window landing (M-6m), encoder load and creators (M-2m), cycle latency (M-3m), KWin with more than 3
  created outputs (M-7m), the D0 path live (S7b in progress: evidence directory has logs, no SUMMARY yet; not read
  further, not touched), owned resize on real RDP (failed 2026-10-01, CUDA OOM).
- S0 "Fit in Replace resizes owned outputs" stays; in mapped mode Fit on a view = re-map that host screen to this
  monitor (M-9).

## 13. Evidence index

- Specs: `docs/superpowers/specs/2026-10-05-console-replace-screens-design.md` (§2 gate, §8 D0-D3);
  `docs/issues/2026-10-05-console-replace-screens-analysis.md` (§1 mechanism A/B, Hal journal 2026-09-29 18:34:57
  `CreateStandIn`); `docs/issues/2026-10-05-console-encoder-limit-two-monitors.md` (Hal 2 HEVC encoders, 4096/8192).
- Contract: `~/dev/rdp/KRDPCTL-V2-CONTRACT.md` (b) capabilities lines 48-92, (h) lines 669-722.
- Evidence: `~/dev/rdp/evidence/2026-10-06-replace-screens-s7/SUMMARY.md` (rows 2-12, first frame +4.7-5.1 s,
  restore +0.0-1.0 s, journal +2.4-3.4 s, row 10 2x4096x4096); `2026-10-06-replace-screens-d0-d3/SUMMARY.md`
  (D0 request, D2 first frame 0.0-0.6 s, D3 ~5.7 s); `2026-10-06-replace-screens-s7b/` (in progress, listing only);
  `~/dev/rdp/archive/research-2026-09-27-full.md:85` (S5 `physical` = host sizes, 2560x1440 x2 on Hal);
  `~/dev/rdp/archive/CLAUDE-2026-10-04-full.md:443,842`.
- Server code: `server/ConsoleVirtualOutputPlan.h:33-120`; `server/ConsoleVirtualOutputPolicy.h:134-239`;
  `server/MonitorCapturePolicy.h`; `server/consoleworker.cpp:600-721, 900-997, 1577-1640, 3355-3359`;
  `server/ConsoleHostController.cpp:291-363, 1000-1028, 1419-1490, 1928-1991, 2061`; `server/ConsoleFrameLayout.h:16-40`;
  `server/ConsoleWorkerWire.h:62, 1476-1528`; `server/BrokerUserSettings.cpp:81`; `src/kcm/brokerpreferences.cpp:208`;
  `src/ClientDisplayInfo.h:30-46, 80-82`; `src/SurfaceLayout.h:22-30`; `src/LayoutControl.h:47-118, 335-360`;
  `src/LayoutControl.cpp:704-760`; `src/EncoderWatchdog.h:18-31`.
- Client code (`~/dev/krdp-client`, head `135f46c`): `src/core/Mapping.h:20-95, 813-838`; `src/core/Views.h:18-330`;
  `src/core/ConsoleScreens.h:26-175`; `src/core/Connection.h:78-163`; `src/ui/AppLayout.cpp:343-399, 473-533, 560-574`;
  `src/ui/SessionModel.cpp:709-718`; `src/ui/Shortcuts.cpp:17-42`; `src/qml/SessionView.qml:279-289`;
  `src/qml/MonitorsPanel.qml:1-23`; `src/qml/MonitorsDialog.qml:1-12`; `src/qml/ConsoleScreensBanner.qml`;
  `src/qml/ConnectionForm.qml:542-556`; `src/rdp/SessionEngine.cpp:655-668, 845-855`; `src/core/ScreenLayout.h:55-61`.
- Commits: server `9893a7f6` (D0), `54841663` (D2), `a6626e6a`, `aec0fbbd`/`d5206b30` (owned resize), `03d50aaa`
  (stand-ins, OPT-047 settle); client `135f46c`, `700fd43`, `8a39d8e`.
