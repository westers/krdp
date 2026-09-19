# Slice 2c (server) — `KRDPCTL` Layout Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let Steve's own client control the host's monitor layout per connection over a private static virtual channel — real monitors lit/off, Fit stand-ins, extra virtual monitors, live re-apply, layout ownership, desk takeover — while third-party clients keep the config-file behaviour.

**Architecture:** `src/LayoutControl.{h,cpp}` is a pure, tested layer: the JSON record codec and the planner (current layout + `apply` → actions, sanitised). `RdpConnection` opens the `KRDPCTL` static channel when the client joined it and exposes records as signals. `SessionController` gates the session build on the first record, runs an owner state machine (`server/LayoutOwner.h`), and executes plans through the existing `PhysicalOutputGuard` + virtual-output machinery, rebuilding only the sessions a diff touches and re-describing the RDP layout.

**Tech Stack:** Qt 6 / KF6, FreeRDP 3 server API (WinPR `WTSVirtualChannel*`), private KPipeWire, KWin `zkde_screencast_unstable_v1`, `kscreen-doctor`, QtTest.

**Spec:** `~/dev/krdp-client/docs/superpowers/specs/2026-09-19-slice-2c-layout-control-design.md` (§3 messages, §4 server, §6 safety, §7 tests). Companion client plan: `~/dev/krdp-client/docs/superpowers/plans/2026-09-19-slice-2c-client-layout-control.md`.

## Global Constraints

- Repository `~/dev/krdp`, branch `master`, build `cmake --build ~/dev/krdp/build -j16`, tests `ctest --test-dir ~/dev/krdp/build --output-on-failure` (9 tests today; `autotests/CMakeLists.txt` uses plain `add_executable` + `add_test`, `OutputSnapshotTest` shows a test that needs both `src/` and `server/` include dirs). Zero new warnings.
- Channel name exactly `KRDPCTL` (7 chars, the static-channel limit is 7). Records: 4-byte big-endian length + UTF-8 JSON object; every record carries `"v": 1`; unknown fields ignored; unknown record `type` → `error {code:"unsupported"}`; records > 64 KiB → connection closed with a log line.
- Message set, field names and semantics exactly as spec §3 (`query`, `apply`, `layout`, `error`, `takeover`, `ping`, `pong`; `HostMonitor{id,name,kind,size{w,h},position{x,y},scale,primary,lit,standIn,owner}`; `Layout{version:1,monitors,owner,you,caps{maxOutputPx:4096,maxUnionPx:8192,cursorMetadata}}`; `apply{private,takeoverLayout?,monitors:[{id|new,lit?,size?,scale?}]}`; `error{code: not-owner|invalid|unsupported, message}`).
- Build gate: a connection that joined `KRDPCTL` gets no session until its first record; `query` builds nothing; 3 s timeout → config-file behaviour with an info log line; connections without the channel behave exactly as today.
- Sanitise every planned layout with the existing rules (per output ≤ 4096 px, union ≤ 8192 px, disjoint, one primary) — refuse with `invalid`, never clamp silently.
- Every physical change goes through `PhysicalOutputGuard` (snapshot before the owner's first change; restore on release/disconnect/takeover/shutdown); virtual outputs use the OPT-041 naming `Virtual-krdp-…`, fence and park rules; the `.stale` set-aside, `--restore-outputs` and the state file keep working.
- No config-file writes by the server; the channel needs the same authenticated connection as the stream; logs never contain credentials.
- Hardware rules (binding): never restart the live service (3389) while a client is connected — `ss -tnp | grep -E ':(3389|3391|3392) ' | grep ESTAB` empty first; never `kwriteconfig6 --notify`; test instances use their own `XDG_CONFIG_HOME` and port (3390 via the client repo's `scripts/test-server.sh`, which gains `MONITOR_MODE=…` unchanged); after any run that changed outputs: `kscreen-doctor -o` shows only DP-1 at 0,0 and HDMI-A-1 at 2560,0 enabled (else the restore command from CLAUDE.md and report), `~/.local/state/krdp-server/` empty, plasmashell answering (`qdbus6 org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.evaluateScript 'print(panels()[0].screen)'` → `0`; if not, STOP and report, never restart plasmashell); never open a window on hal9000's display; no sudo; no X11 tooling. The own client on buzz drives acceptance headlessly (see the client plan's C1 for `--query-layout`/`layout-apply=`); until C1 exists, Task 2's acceptance uses the `krdpctl-probe` tool this plan adds.
- Commits on `master`, message ending `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`; no amend/rebase/stash; no push.

---

## File structure

| File | Responsibility |
|---|---|
| `src/LayoutControl.h/.cpp` (new, in `KRdp` lib) | `HostMonitor`, `Layout`, `ApplyRequest`, record codec (`encodeRecord/decodeRecord`, framing), `Planner::plan(current, request, caps) → Plan{actions, resultingLayout} | Error` |
| `autotests/LayoutControlTest.cpp` (new) | codec + framing + planner cases |
| `src/RdpConnection.h/.cpp` | `KRDPCTL` open/read/write (`hasControlChannel()`, `controlRecordReceived(QJsonObject)`, `sendControlRecord(QJsonObject)`), 3 s gate timer helper |
| `server/LayoutOwner.h` (new, header-only) | owner state machine (owner/viewer/none, takeover, heartbeat misses) — pure, tested in `autotests/LayoutOwnerTest.cpp` |
| `server/HostLayoutExecutor.h/.cpp` (new) | applies a `Plan`: guard on/off, stand-in/extra virtual create/remove, returns the actual `Layout`; owns the "who owns which virtual output" table |
| `server/SessionController.h/.cpp` | build gate, records → owner → planner → executor → sessions diff; `takeover` broadcast; viewers |
| `examples/krdpctl-probe/` (new) | tiny libfreerdp client: connect, join `KRDPCTL`, send `query`/`apply` from a JSON file, print records — for server acceptance before the own client speaks the channel |
| `src/Cursor.cpp`, `src/PlasmaScreencastV1Session.cpp` | Task 5: cursor metadata verification (spike + fix) |
| `~/dev/kpipewire` (private KPipeWire, branch `westers/opt-015`) | Task 6: S6 teardown guard |
| `README.md`, `research.md`, `server/krdpserversettings.kcfg` | docs |

---

### Task 1: `LayoutControl` — records, framing, planner (pure)

**Files:**
- Create: `src/LayoutControl.h`, `src/LayoutControl.cpp`, `autotests/LayoutControlTest.cpp`
- Modify: `src/CMakeLists.txt` (add the .cpp to the `KRdp` library), `autotests/CMakeLists.txt` (`add_executable(LayoutControlTest …)` + `add_test`, include dirs `src/`)

**Interfaces (produces):**
```cpp
namespace KRdp::LayoutControl {
enum class Kind { Real, Virtual };
struct HostMonitor { QString id, name; Kind kind; QSize size; QPoint position; qreal scale = 1.0;
                     bool primary = false, lit = true, standIn = false; QString owner; bool operator==(const HostMonitor&) const = default; };
struct Caps { int maxOutputPx = 4096; int maxUnionPx = 8192; bool cursorMetadata = true; };
struct Layout { QList<HostMonitor> monitors; QString owner; QString you /* owner|viewer|none */; Caps caps; };
struct ApplyMonitor { QString id; bool isNew = false; std::optional<bool> lit; std::optional<QSize> size; std::optional<qreal> scale; };
struct ApplyRequest { bool privateMode = false; bool takeoverLayout = false; QList<ApplyMonitor> monitors; };
struct Error { QString code; QString message; };                 // "not-owner" | "invalid" | "unsupported"
QJsonObject toJson(const Layout&); std::optional<Layout> layoutFromJson(const QJsonObject&);
QJsonObject toJson(const ApplyRequest&); std::optional<ApplyRequest> applyFromJson(const QJsonObject&);
QJsonObject errorRecord(const Error&); QJsonObject layoutRecord(const Layout&); QJsonObject takeoverRecord(const Layout&);
QByteArray frame(const QJsonObject &record);                     // 4-byte BE length + UTF-8 JSON, "v":1 added
class Deframer { public: void feed(const QByteArray&); std::optional<QJsonObject> next(); bool overflowed() const; }; // > 64 KiB → overflowed
enum class ActionKind { LightReal, DarkenReal, CreateStandIn, RemoveStandIn, CreateVirtual, RemoveVirtual };
struct Action { ActionKind kind; QString id; QSize size; QPoint position; qreal scale = 1.0; };
struct Plan { QList<Action> actions; Layout resulting; };
std::variant<Plan, Error> plan(const Layout &current, const ApplyRequest &request, const QString &requester, const Caps &caps);
}
```
Planner rules: monitors not mentioned keep state; a mentioned real monitor with `size` ≠ its native size → `CreateStandIn` at that size/scale at the real one's position and the real one dark (`standIn=true`, `lit=false`); a real monitor previously stand-in and now mentioned without `size` → `RemoveStandIn` + lit per request; `isNew` → `CreateVirtual` with `id = "virtual-<n>"` (lowest free n), placed at the right edge of the union (`x = union.right()+1`, `y = 0`), scale from the request or 1.0; virtual monitors owned by `requester` that are omitted → `RemoveVirtual`; virtual monitors owned by others untouched; `privateMode` → every real monitor `lit=false`; positions of real monitors never change; sanitise the resulting layout (each output ≤ `maxOutputPx` per side; union ≤ `maxUnionPx`; disjoint; exactly one primary — the real primary, or the first monitor when the primary is dark and there are only virtual/stand-in monitors… keep the real primary flag as is; `primary` never changes) → `Error{"invalid", …}` on violation. `you` in `resulting` = "owner".

- [ ] **Step 1: Failing tests** — codec round trips for `Layout` and `ApplyRequest` (all fields; `size`/`scale` optional); `Deframer` splits two records from one buffer and one record from two chunks; > 64 KiB → `overflowed()`; `frame()` adds `"v":1`. Planner: (a) hal9000 layout (DP-1 2560x1440 @0,0 primary, HDMI-A-1 @2560,0) + `apply{private:true}` → two `DarkenReal`, both `lit=false`; (b) `apply{monitors:[{id:"DP-1", size:{1920,1080}, scale:1.0}]}` → `CreateStandIn DP-1 1920x1080 @0,0` and DP-1 `standIn=true, lit=false`; (c) a second apply without `size` for DP-1 and `lit:true` → `RemoveStandIn` + `LightReal`; (d) `{new:true, size:{1920,1080}}` → `CreateVirtual "virtual-1" @ (5120,0)`; (e) omitting `virtual-1` in the next apply from the same requester → `RemoveVirtual`; another requester's virtual stays; (f) `{new:true, size:{5000,1080}}` → `Error invalid` (output > 4096); two 4096-wide extras → union > 8192 → `invalid`; (g) untouched monitors keep state; (h) unknown id → `invalid`.
- [ ] **Step 2: run, expect failure. Step 3: implement.** Header-only structs + `.cpp` for codec/planner. **Step 4: run, expect pass;** ctest green; zero warnings.
- [ ] **Step 5: Commit** `layout-control: records, framing and planner for the KRDPCTL channel (pure, tested)`.

---

### Task 2: `KRDPCTL` on `RdpConnection` + the build gate + `krdpctl-probe`

**Files:**
- Modify: `src/RdpConnection.h/.cpp` (next to the cliprdr detection at ~:605: when `WTSVirtualChannelManagerIsChannelJoined(vcm, "KRDPCTL")` and not yet opened → `WTSVirtualChannelOpen(vcm, WTS_CURRENT_SESSION, "KRDPCTL")`, keep the handle, poll `WTSVirtualChannelRead` in the same loop (the manager's event handle wakes it), feed a `Deframer`, emit `controlRecordReceived(const QJsonObject&)` per record on the connection's thread (queued to consumers); `bool hasControlChannel() const` (joined at handshake — available once the peer is activated); `void sendControlRecord(const QJsonObject&)` (thread-safe: queued onto the connection thread, `WTSVirtualChannelWrite`); close the channel on disconnect), `server/SessionController.cpp` (in `onNewConnection`: if `hasControlChannel()` → do NOT connect `clientDisplayInfoReceived` to the build; start a 3 s `QTimer`; on the first record: `query` → reply `layoutRecord(currentLayout(you:"none"|"viewer"|"owner"))` and mark the connection "query-only" (no build ever, closes when the client disconnects); `apply` → Task 3's path; timeout → log `KRDPCTL client sent nothing in 3 s; using the configured MonitorMode` and fall back to today's build; unknown → `error unsupported`), `src/CMakeLists.txt`
- Create: `examples/krdpctl-probe/main.cpp` + `CMakeLists.txt` (libfreerdp3 client: `freerdp_new/context`, settings from args `host port user pass`, `freerdp_channels_client_load_ex` with an in-process `VIRTUALCHANNELENTRYEX` registering `KRDPCTL` (`CHANNEL_OPTION_INITIALIZED|ENCRYPT_RDP`), `--query` sends `{"type":"query"}` and prints every record received then exits; `--apply file.json` sends the file's object as `apply` and prints records until Ctrl-C; NLA/TLS like the own client (`FreeRDP_ConfigPath` under `$XDG_RUNTIME_DIR/krdpctl-probe`, auto-accept the certificate with a printed fingerprint); prints nothing secret)
- Test: `autotests/LayoutControlTest.cpp` unchanged; the gate is covered by the probe acceptance.

**Interfaces:** consumes Task 1; produces `RdpConnection::hasControlChannel()`, `controlRecordReceived(QJsonObject)`, `sendControlRecord(QJsonObject)`; `SessionController::currentLayout(const RdpConnection*) const → LayoutControl::Layout` (real outputs from a fresh `OutputSnapshot` via the guard's reader, virtual outputs from the executor table — in this task the table is empty).

- [ ] **Step 1:** implement the channel + gate + probe. **Step 2:** build zero warnings; ctest green.
- [ ] **Step 3: Acceptance** (no output changes in this task): `MONITOR_MODE=multi scripts/test-server.sh start` (client repo script; multi so a fallback build is visible); `krdpctl-probe hal9000 3390 krdptest krdptest --query` prints a `layout` record with `monitors[0].id == "DP-1"` (2560x1440 @0,0, primary, lit) and `[1] == "HDMI-A-1"`, `owner == null`, `you == "none"`, `caps.maxOutputPx == 4096`; the server log shows `KRDPCTL: query → layout (2 monitors), no session built` and NO screencast/encoder lines for that connection; a probe that connects and sends nothing → after 3 s the server logs the timeout line and builds the multi session as usual (frames flow; the probe is killed); a third-party connection (own client 0.3.0 from buzz, or `sdl-freerdp3`) is unaffected. Stop the server; verify no ESTAB.
- [ ] **Step 4: Commit** `rdp: KRDPCTL static channel on RdpConnection; session build gated on the first record; krdpctl-probe`.

---

### Task 3: Owner state machine + executor + first `apply` (build from a layout)

**Files:**
- Create: `server/LayoutOwner.h` (pure: `class LayoutOwner { enum Role {None, Owner, Viewer}; Role roleOf(id) const; std::optional<Error> tryAcquire(id, bool takeover); void release(id); void heartbeatMissed(id) /* 3 → release */; void heartbeatOk(id); QString owner() const; QList<QString> viewers() const; }`), `autotests/LayoutOwnerTest.cpp`, `server/HostLayoutExecutor.h/.cpp` (`class HostLayoutExecutor : QObject { explicit HostLayoutExecutor(PhysicalOutputGuard*, KScreen/kscreen-doctor runner…); LayoutControl::Layout current() const; std::variant<LayoutControl::Layout, LayoutControl::Error> execute(const LayoutControl::Plan&, const QString &requester); void releaseAll(); QList<VirtualOutputRecord> virtualOutputs() const; signals: layoutChanged(); }` — actions map to: `DarkenReal/LightReal` → guard per-output disable/enable (extend `PhysicalOutputGuard` with `setOutputEnabled(name, bool)` that snapshots on first use and tracks per-output state; restore restores all), `CreateStandIn/CreateVirtual` → the OPT-041 virtual-monitor creation path (`AbstractSession::setVirtualMonitor` per session — see below) with placement/park rules, `RemoveStandIn/RemoveVirtual` → session teardown + output removal)
- Modify: `server/SessionController.h/.cpp` — on `apply`: `LayoutOwner::tryAcquire(connectionId, takeoverLayout)` → `error not-owner` or proceed; `LayoutControl::plan(executor.current(), request, id, caps)` → `error invalid` or `executor.execute(plan)`; then **build sessions for this connection from the resulting layout**: one `PlasmaScreencastV1Session` per host monitor that is lit-or-virtual-or-stand-in (real dark monitors with no stand-in are still streamed — the desk is dark, the client sees them: that is `replace`'s behaviour today; a real monitor stays a stream target whether lit or not), RDP layout = the resulting layout's rects (translated to origin), primary as in the layout; reply `layoutRecord(resulting with you:"owner")`; heartbeat `ping` every 5 s to the owner, `pong` resets; 3 misses → `release` → `executor.releaseAll()` (guard restore) → `layout` to remaining connections with `owner:null`; owner disconnect → same; viewers (other connections with the channel) get `you:"viewer"` and their sessions are built from the same current layout; a viewer's `apply` without takeover → `error not-owner`; with `takeoverLayout` → owner change, previous owner keeps its sessions until the next diff (Task 4) — in this task the previous owner's connection is re-described with the new layout after the executor ran.
- Test: `LayoutOwnerTest` (acquire/none, second acquire refused, takeover switches, release on 3 misses, release on disconnect); `PhysicalOutputGuard` per-output enable/disable covered by hardware.

- [ ] **Step 1: Failing tests** for `LayoutOwner`. **Step 2–4:** RED → implement → GREEN; zero warnings.
- [ ] **Step 5: Hardware acceptance** (virtual-style run: outputs WILL change; OPT-041 rules): `MONITOR_MODE=multi scripts/test-server.sh start` (the mode no longer matters for a channel client; multi keeps third-party behaviour visible); (a) probe `--apply private.json` (`{"private":true,"monitors":[]}`) → server log `apply from <id>: owner acquired`, both physical outputs disabled (kscreen shows DP-1/HDMI-A-1 disabled, no virtual outputs), the probe prints `layout` with both `lit:false`, `you:"owner"`; the connection's sessions = two streams (frames flow — check the server's frame lines or the probe's `ResetGraphics` print); kill the probe → within ≤ 15 s (3 missed pings) or on disconnect immediately: guard restore, both outputs back, state dir empty; (b) `--apply standin.json` (`{"private":false,"monitors":[{"id":"DP-1","size":{"w":1920,"h":1080},"scale":1.0}]}`) → kscreen: `Virtual-krdp-…1920x1080` at 0,0, DP-1 disabled, HDMI-A-1 enabled; `layout` shows DP-1 `standIn:true, lit:false`; disconnect → restored; (c) `--apply extra.json` (`{"private":false,"monitors":[{"new":true,"size":{"w":1920,"h":1080},"scale":1.25}]}`) → a `virtual-1` at (5120,0) 1920x1080 scale 1.25 (kscreen shows scale 1.25), both physical lit; disconnect → removed; (d) two probes: the second's `--apply` → `error not-owner`; with `"takeoverLayout":true` → it becomes owner and the first prints `layout` with `you:"viewer"`. After each: layout check, state dir, plasmashell answering. Stop the server.
- [ ] **Step 6: Commit** `server: layout owner, HostLayoutExecutor and KRDPCTL apply — sessions built from the requested host layout`.

---

### Task 4: Live apply (diff), viewers, desk takeover broadcast, config default

**Files:**
- Modify: `server/SessionController.cpp` (second and later `apply` from the owner: run the planner against `executor.current()`; execute; for EVERY connection with sessions: tear down only the sessions whose output was removed/recreated, create sessions for new outputs, keep the rest running; re-describe the RDP layout (`ResetGraphics` path already used by `adoptActualVirtualLayout`) on each connection; reply `layout` to the owner and push `layout` to viewers), `server/TakeoverDetector`/`SessionController` (on desk takeover: `executor.releaseAll()` semantics but the sessions continue over the real outputs (as today: continue in extend) → send `takeoverRecord(current)` to every channel connection; the owner keeps ownership; its next `apply` re-runs the planner from the restored state), `server/HostLayoutExecutor.cpp` (diff support: `execute()` returns which output ids were removed/created so the controller can rebuild only those), `README.md` (MonitorMode section: "own client: `KRDPCTL`; the config keys are the default for other clients; Steve's fallback = `multi`"), `research.md` (OPT-044 IN PROGRESS → DONE lines with evidence)
- Test: planner diff cases in `LayoutControlTest` (`plan(current_with_standin, apply_light_DP1)` → `RemoveStandIn`+`LightReal`, nothing for HDMI; `plan(current, apply_same)` → no actions).

- [ ] **Step 1–4:** tests → implement → green; zero warnings.
- [ ] **Step 5: Hardware acceptance:** probe as owner: `private.json` → `standin.json` → `extra.json` → `private.json` in one connection (the probe gains `--apply-seq a.json b.json …` with 8 s between) — each step: kscreen state as in Task 3, the server log shows only the affected sessions torn down/created (`session for HDMI-A-1 kept`), the probe prints a `layout` per step and a `ResetGraphics` re-description per step; a second probe connected as viewer prints a `layout` per step too; desk takeover simulated as in OPT-041 (`kglobalaccel invokeShortcut restore-physical-outputs`) → every probe prints `takeover`, kscreen shows both physical lit, virtuals parked; owner `--apply private.json` again → dark again. Stop; checks.
- [ ] **Step 6: Commit** `server: live KRDPCTL apply — diff-based session rebuild, viewers follow, desk takeover broadcast; docs`.

---

### Task 5: Cursor metadata verification (OPT-042 server half)

**Files:**
- Modify (only if the spike finds a gap): `src/PlasmaScreencastV1Session.cpp` (cursor mode on virtual-output streams), `src/Cursor.cpp`; always: `research.md` OPT-042 entry with the findings.

- [ ] **Step 1: Spike** (hardware, `MONITOR_MODE=specific` and a virtual stand-in via Task 3's probe): with `QT_LOGGING_RULES="org.kde.krdp.debug=true"`, connect the own client 0.3.0 from buzz, move the pointer over a text field on hal9000 (drive with the client's `pointer-test`/debug moves; a Konsole/Kate window is fine — no clicks) and record: does the server log `PointerNew`/cursor updates (`Cursor::update` → `updatePointer->PointerNew`) for (a) a physical output stream and (b) a virtual output stream; is the cursor painted into the frames (compare a frame region snapshot from the client's `snapshot=` around the pointer with and without the pointer present)? Expected from the code: `Screencasting::Metadata` on all three stream kinds → metadata, pointer PDUs sent, no embedded cursor.
- [ ] **Step 2:** if any stream kind lacks metadata or paints the cursor, fix it (request metadata; never embed when metadata works) with a unit test where the choice is testable; otherwise no code change.
- [ ] **Step 3: Commit** `cursor: verify metadata mode on all stream kinds (OPT-042 server half)` (or the fix), `research.md` updated with the evidence and the caps flag `cursorMetadata` set accordingly in `LayoutControl::Caps` (Task 1 default `true`).

---

### Task 6: S6 — teardown of a frame-less session must not crash

**Files:**
- Modify: `~/dev/kpipewire/src/encoder.cpp:179` `Encoder::finish()` (guard: if the codec context was never opened / no frame was ever sent, skip `avcodec_send_frame(nullptr)`/drain), and/or `PipeWireProduce::stateChanged` in the private KPipeWire; rebuild with `~/dev/krdp/scripts/build-kpipewire.sh`; export the patch to `~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/`; `research.md` S6 → DONE.
- [ ] **Step 1: Reproduce** with a test instance: `kscreen-doctor --dpms off` is NOT allowed while any client is connected and wakes are the default — instead reproduce with `WakeDisplayOnConnect=false` in the test config and hal9000 DPMS-off ONLY if no client is connected and Steve is not at the desk (check `ss` and `loginctl show-session … -p IdleHint`); connect the own client from buzz, disconnect after 5 s → server SIGSEGV (`coredumpctl`), stack `Encoder::finish → avcodec_send_frame`. **Step 2:** fix; rebuild; repeat → clean teardown, log line `encoder never received a frame; skipping drain`. **Step 3:** `kscreen-doctor --dpms on` / `qdbus6 … wakeup`; layout/state checks.
- [ ] **Step 4: Commit** (kpipewire repo) `encoder: finish() without a frame is a no-op (KRDP S6)`; (krdp repo) `research: S6 done; kpipewire patch exported`.

---

## Self-review

- Spec coverage: §3 messages/codec → T1/T2; §4 gate → T2; owner/planner/executor/private/stand-in/extra/placement/scale → T3; live apply, viewers, takeover, config default → T4; cursor metadata → T5; S6 (found during 2b) → T6; §6 safety: the guard covers every change (T3/T4), no config writes, heartbeat release (T3); §7 tests: planner/owner/codec unit tests (T1/T3/T4), hardware in T3/T4/T5.
- Placeholders: none. The primary-flag rule in the planner is fixed ("never changes"). `PhysicalOutputGuard::setOutputEnabled(name, bool)` is defined in T3 and used only there/T4.
- Type consistency: `LayoutControl::{HostMonitor, Layout, ApplyRequest, Error, Plan, Action, ActionKind, Caps, plan()}` used with these names in T2–T4; `LayoutOwner` API as declared in T3; `HostLayoutExecutor::{current, execute, releaseAll}`; `RdpConnection::{hasControlChannel, controlRecordReceived, sendControlRecord}`.
- Ordering: T1 → T2 → (client plan C1 may start after T2) → T3 → T4 → T5 → T6. T5/T6 are independent of T3/T4 and may run when the hardware is free.
