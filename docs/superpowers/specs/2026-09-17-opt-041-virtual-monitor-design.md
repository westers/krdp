# OPT-041: Client-sized virtual monitors — design

**Status:** APPROVED by Steve 2026-09-17 ("replace as default"). Grounded in the spike and code
research of 2026-09-16/17 (`.superpowers/sdd/spike-opt-041-virtual-monitor/report.md`). Phases A and
B are planned together; Phase C gets its own plan.

## 1. Goal

When a client connects, hal9000 presents a desktop whose monitors match the client's screens,
the way Windows RDP and NoMachine do: one KWin virtual output per client monitor at the client's
exact pixel size, the physical monitors switched off for the duration (or kept, by policy), the
physical layout restored on disconnect, and (later) live resize when the client window changes.
For Steve's daily case — laptop 1920x1080, sometimes plus a 1920x1280 portable monitor — the
remote desktop is simply his real desktop at those sizes, with no scaling and no seam mapping.

## 2. What is already proven

- KWin's `zkde_screencast_unstable_v1.stream_virtual_output` creates a real output on the live
  DRM session; the fork's `--virtual-monitor WxH@S` path drives it: output created on client
  connect, removed on disconnect, H.264 of exactly that output streamed, no compositor churn,
  the idle live service unaffected.
- Physical outputs can be disabled and re-enabled with `kscreen-doctor` while the virtual output
  streams; windows migrate onto the virtual output; the layout returns to baseline.
- KWin remembers layouts keyed on the set of present outputs (virtual included) in
  `~/.config/kwinoutputconfig.json` and replays them when the same set reappears.
- Bugs/gaps found: the virtual session hardcodes its logical rect to `(0,0)` (KWin actually
  placed the output at `(5120,0)`), so client input lands on DP-1 — server-side bug.
  `refreshDisplayConfiguration()` is a no-op whenever a virtual monitor is configured. The fork
  never reads the client's advertised desktop size or monitor layout. Upstream's dynamic-resize
  commit `3f12de7` depends on a KPipeWire `setRequestedSize()` that neither stock 6.6.4 nor the
  private fork has, and conflicts in 4 files.

## 3. Configuration

New keys in `server/krdpserversettings.kcfg` (all live-reloadable, applied at the next
connection):

| Key | Type | Default | Meaning |
|---|---|---|---|
| `MonitorMode` | String | `workspace` | adds value `virtual` (requires `--plasma`; falls back to `workspace` with a warning otherwise) |
| `VirtualMonitorPolicy` | String | `replace` | `replace` = disable every physical output while a virtual session streams, restore on teardown; `extend` = leave physical outputs alone |
| `VirtualMonitorLayout` | String | `client` | `client` = one virtual output per client monitor when the client advertises a layout (Phase B); `single` = one output at the client's desktop size |
| `VirtualMonitorFallbackSize` | String | `1920x1080` | used only if the client advertises no usable size (< 640x480 or > 4096 on a side) |

**Decision (Steve, 2026-09-17):** `replace` is the default; `extend` is the opt-in.

Existing modes `workspace|primary|specific|multi` are byte-for-byte unchanged. The CLI flag
`--virtual-monitor WxH@S` stays as a debugging override (fixed size, policy `extend`).

## 4. Phase A — single virtual output, replace/extend, correct input

Lifecycle per connection (`server/SessionController.cpp`):

1. **Size is known** after the capabilities exchange: `RdpConnection` reads
   `FreeRDP_DesktopWidth/Height` (and, for Phase B, `FreeRDP_MonitorCount/MonitorDefArray`) in
   `onCapabilities()` and exposes them as `clientDesktopSize()` / `clientMonitors()`. Validation:
   each side 640..4096 (VA-API surface limit), else the fallback size.
2. **Session build:** `buildSessions()` creates one `PlasmaScreencastV1Session` with
   `VirtualMonitor{name = "krdp-<port>-<connectionId>", size = clientDesktopSize, dpr = 1.0}`.
   The name is unique per connection so KWin's remembered setups never replay a stale
   arrangement onto a new session (§6).
3. **Geometry resolution (input fix):** after the screencast `created` event the session looks
   up the `QScreen` KWin added for the virtual output — matched by `QScreen::name()` containing
   the request name (KWin reports `Virtual-<name>`; verify in the plan's first task and fall back
   to "the screen added since the request whose size equals the request") — and sets
   `d->logicalRect = screen->geometry()`. It connects `QScreen::geometryChanged` so the rect
   follows KWin re-positioning the output when physical outputs are disabled/enabled. If no
   matching screen appears within 2 s, the session logs an error, keeps `(0,0)` and **refuses to
   apply `replace`** (input would otherwise drive the physical desktop).
   `sendEvent()` already adds `d->logicalRect.x()/y()`; no change to the input path beyond the
   rect being right.
4. **Streaming starts** exactly as today (surface reset, IDR on demand, adaptive quality).
5. **Policy apply (replace only):** on the first frame sent *and* the geometry resolved, a new
   `server/PhysicalOutputGuard` snapshots the physical layout (`kscreen-doctor -j` parsed to
   `{name, enabled, position, priority}` per non-virtual output) and disables every physical
   output that is currently enabled: `kscreen-doctor output.<name>.disable …` in one invocation.
   Verification 1 s later with `kscreen-doctor -j`; on failure it logs and leaves the rest as is
   (the virtual stream keeps working; the session is then effectively `extend`).
6. **Teardown (all paths — normal disconnect, error, `SIGTERM`/`SIGINT`, `aboutToQuit`, and the
   guard's destructor):** the guard re-enables the snapshot's outputs with their positions and
   priorities in one `kscreen-doctor` call, verifies, and only then the session closes its
   stream so the virtual output disappears. KWin therefore never has zero enabled outputs and
   windows migrate back to the physical monitors. If re-enable fails, the server logs at
   critical level with the exact recovery command (`kscreen-doctor output.DP-1.enable
   output.HDMI-A-1.enable`) and retries once after 2 s.
7. **Screen-change handling:** `refreshDisplayConfiguration()` no longer short-circuits for
   virtual sessions; instead the virtual session ignores `QScreen` added/removed events for
   screens other than its own, and reacts to its own screen's `geometryChanged` (step 3) and
   removal (treated as a fatal stream loss → session ends → teardown).

Mechanism choice: `kscreen-doctor` as a child process (proven in the spike, no new build
dependency; `libkf6screen-dev` is not installed on hal9000). The guard wraps it behind one small
interface (`snapshot()`, `disableAll()`, `restore()`), so an in-process libkscreen backend can
replace it later without touching callers.

## 5. Phase B — one virtual output per client monitor

When `VirtualMonitorLayout=client` and the client advertises ≥ 2 monitors
(`FreeRDP_SupportMonitorLayoutPdu` / `MonitorDefArray`; Remmina, `sdl-freerdp3 /multimon`, and the
own client all do), `buildSessions()` creates N virtual sessions, one per client monitor, each at
that monitor's size, and hands them to the existing multi machinery from Plan 3 (N encoders,
N RDPGFX surfaces, `SurfaceLayout`, per-monitor keyframes). Differences from `multi`:

- Positions: after all N outputs exist, the guard positions them to mirror the client layout
  (`kscreen-doctor output.<name>.position.x,y`, translated so the union's top-left is at the
  physical desktop's right edge in `extend`, or at `(0,0)` in `replace`), and marks the client's
  primary as `priority 1`. `SurfaceLayout::fromMonitors` then works from the real `QScreen`
  geometries exactly as it does for physical monitors, so the RDP layout equals the client layout
  and third-party `/multimon` works without the OPT-040 mapping.
- The 4096-px VA-API limit is per output, as in `multi`.
- If any of the N outputs fails to appear, the session falls back to Phase A single output at
  the client desktop size (bounding box), logged.

## 6. KWin remembered setups

KWin keys a remembered arrangement on the set of present outputs and replays it when that set
reappears. With unique per-connection names no set ever repeats, so nothing stale is replayed and
the server's explicit policy application (§4 steps 5–6) is the only thing that changes the
physical outputs. Cost: `kwinoutputconfig.json` gains one setup entry per connection. The plan
includes a check of how KWin bounds that list; if it grows without bound, the fallback is a
stable name per `(policy, size)` plus idempotent policy application on every connect, which is
also safe because the server always re-asserts the desired state after the output appears.

## 7. Phase C — live resize (separate plan, after A and B)

Port upstream `DisplayControl` (MS-RDPEDISP server side, `disp_server_context_new`, hand-ported
because `3f12de7` conflicts in 4 files) to receive the client's new layout. Resize strategy:
**create the new virtual output(s) first, then remove the old** (never zero outputs in `replace`),
re-run §4 step 3 and §5 positioning, and let the normal surface reset publish the new layout to
the client. An in-place resize (KPipeWire `setRequestedSize` + KWin virtual-output resize) is a
later refinement if the private KPipeWire gains that API; it is not required.

## 8. Own client (slice 2 notes, not part of this plan)

- Advertise the real screen size when full screen and the window's size when windowed; send the
  monitor layout (`MonitorDefArray`) when spanning multiple local screens.
- In `virtual` mode the Fit scaling and the thumbnail monitor switcher are unnecessary (Native 1:1
  is exact); both remain for the physical modes. OPT-037 shrinks to the physical modes; OPT-040 is
  retired for `virtual`.

## 9. Failure handling

| Failure | Behaviour |
|---|---|
| Virtual output never appears (`failed` event / timeout 5 s) | session ends with a logged reason; connection closed with an error; physical outputs untouched |
| Geometry unresolved | stream continues, `replace` refused, input warning logged once |
| `kscreen-doctor` missing or exits non-zero | logged; policy degrades to `extend` |
| Restore fails at teardown | critical log with recovery command, one retry; server keeps running |
| Server killed with `SIGKILL` while physicals are disabled | documented recovery command in README/CLAUDE.md; a `--restore-outputs` CLI flag re-enables everything from the last snapshot file (`$XDG_STATE_HOME/krdpserver/physical-outputs.json`, written before disabling, deleted after a verified restore) |
| Client connects while another virtual session is active | second connection is refused in `virtual` mode until Phase B multi-session policy is designed (one virtual desktop at a time) |

## 10. Testing

- Unit (QtTest, `autotests/`): `OutputSnapshot` parse/plan (pure: parse `kscreen-doctor -j`
  JSON → snapshot; snapshot → disable/restore command lines); client size validation and
  fallback; Phase B layout translation (client rects → KWin positions, primary priority).
- Integration on a test instance (port 3392, temp `XDG_CONFIG_HOME`, `MonitorMode=virtual`), own
  client headless from buzz: output appears with the client's size; pointer readback via KWin
  script equals `(virtual.x + 1234, virtual.y + 567)`; `replace`: physical outputs disabled after
  first frame and restored to the baseline JSON on disconnect; `extend`: physicals untouched;
  kill the test instance with `SIGTERM` mid-session → layout restored.
- Acceptance by Steve from buzz with Remmina and the own client: his real desktop at 1920x1080,
  windows present, input exact, monitors back on after disconnect. Phase B: laptop + portable
  monitor with `/multimon` on `sdl-freerdp3` and with the own client.

## 11. Deployment

Test instance first; then the live service switches with
`kwriteconfig6 --file krdpserverrc --group General --key MonitorMode virtual --notify` (takes
effect at the next connection). Rollback is the same command with `specific`. Never restart the
service with a client connected.

## 12. Out of scope

Independent headless sessions (NoMachine "virtual desktop" for other users) — needs its own spec;
audio/USB; client DPI/scale > 1 (dpr fixed at 1.0 in this plan); in-place KPipeWire resize.
