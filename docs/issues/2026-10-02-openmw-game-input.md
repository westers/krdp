# OpenMW mouse capture over Farside RDP (OPT-054)

## Capture synchronization follow-up (Steve, 2026-10-02)

Steve confirms installed0.6.6 capture works and selecting the View-menu action
again successfully recaptures. Right Ctrl is release-only; host-key chords such
as Right Ctrl+T continue to work, releasing capture before running the chord.
The remaining bug is **client/host capture disagreement**: local release leaves
OpenMW/KWin constrained, with hidden or stale game cursor behavior. Showing a
local arrow alone is insufficient. Closing an OpenMW menu also needs a defined
relationship between the app's capture request and local capture.

Requirements for the paired follow-up:

1. Observe actual KWin pointer-lock requests and activation, separately from
   confinement, cursor visibility and fullscreen. Read the current state when
   the observer attaches; subscribe to subsequent changes. Capture is not a
   cached client boolean and a server started after OpenMW must see its existing
   request without requiring the game to toggle it.
2. Track app-requested lock, host permission/actual lock, user capture intent,
   and local compositor-confirmed lock separately. On an admitted controlling
   connection, explicitly released local input must suspend the host constraint
   too; continue to honor ordinary visible remote cursor shapes.
3. A game menu temporarily removes the app request: release the local lock.
   Its return may resume an already armed user capture intent only while the
   view is active and the pointer is deliberately in it. Right Ctrl cancels that
   intent; no immediate automatic recapture after explicit release.
4. Reconnect starts free under the agreed manual-entry policy and takes a fresh
   host snapshot. A still-existing app request enables/explains the explicit
   capture action, but does not override the prior manual release. Native lock
   confirmation and a host acknowledgement precede a successful capture claim.
5. On disconnect/owner loss/worker death remove Farside's host override and
   restore normal local compositor behavior. Sol's physical user/apps control
   capture when no controlling RDP client exists. The observer may keep reading
   state without clients; durable historical state is unnecessary.
6. Bind commands/replies/snapshots to connection, current owner generation and
   compositor epoch. Reject delayed work after takeover, reconnect or compositor
   restart. A disappearing hook releases local input and reports unavailable,
   rather than claiming synchronization succeeded.
7. KWin6.6.6's native SDK exports `setEnableConstraints(bool)`, current surfaces'
   locked-pointer objects/signals, and runtime plugin loading. Ordinary D-Bus
   has no equivalent capture API. A native integration must match exact KWin
   ABI, cooperate with its own temporary overrides (e.g. Alt+Tab), expire a dead
   worker's lease and avoid changing compositor permissions or desktop locking.
   SDK was downloaded/extracted only; no plugin installed or live KWin changed.

Client-only cursor fallback has a focused production-QML regression check on
Buzz: release while host hidden, menu image→hidden changes, explicit recapture
and second release. This does not establish host synchronization and is not a
new deployed release. Evidence: `~/dev/rdp/evidence/2026-10-02-cursor-release/`.
Paired compositor/protocol implementation remains open; 0.6.6 is still installed.

## Delivery (2026-10-02)

**Paired implementation deployed; sustained gameplay acceptance remains manual.**

- Sol server `00e74e4395` / `6.6.80+git202610020702.00e74e4-1`;
  client **0.6.6** installed on Sol, Buzz and Hal. Hal/Buzz servers remain
  `2828a7c`; Ace/Cray/Marvin are unchanged by this task.
- Server package SHA256 `75fa789efc80afa45ae04dac0d7294db127f40d111db9c6de707d331aa73967e`;
  client package SHA256 `fdcc468b390b240995240f437d4b9114fe421cdc81d39c6446ea96a6f23e5dd3`.
  Clean committed exports; production client debug actions OFF.
- Native Buzz → installed Sol Console check passed using the committed client
  with debug actions in a separate software KWin instance. Actual native local
  relative motion (three 800.25-pixel events) changed OpenMW's view; capture hint
  appeared only after lock confirmation. Bounded W down/up reached the game and
  the scene changed slightly near a crate. Right Ctrl removed capture; Escape
  opened and closed the game menu. Client remained owner throughout. This is
  not a measured 360-degree turn or unconstrained walking acceptance.
- Separate native lock gate passed: compositor confirmation, fractional motion,
  manual release, focus/hide release, no automatic recapture and fullscreen
  re-entry. The private test compositor allowed its synthetic driver only;
  actual desktop permissions and Buzz's locked session were unchanged.
- Focused RDP/wire/input/device classification suites and generation-bound
  takeover/held-W cleanup pass. Earlier native keyboard baseline also showed
  A/S movement while owner. Intermittent original revocation was not reproduced;
  the cursor-based takeover mechanism that could explain it has been replaced.
- Deployment preserved configuration/TLS/profiles/holds/service enablement,
  work/client/AI processes and OpenMW PID322976. Sol Console328097 and
  Virtual328098 active/NRestarts0; stored/running80, `/dev/video10`,3391 match.
  Root-private rollback directories: `~/farside-game-input-backup-20261002-sol`,
  and `~/farside-game-input-backup-20261002-{buzz,hal}-client` on each host.
- Reopen Hal's client; choose **View → Capture Mouse for Games** and move into
  the view. Right Ctrl releases; Escape reaches OpenMW. Steve checks repeated
  turns, free walking/stopping and mouse buttons. Full Virtual game, actual
  physical-seat takeover and ordinary multi-window drag were not live-tested.
  Automatic remote-lock observation remains deferred as described below.

## Implementation record (2026-10-02)

The following records the paired implementation and its earlier diagnosis.

- A native Buzz → installed Sol baseline shows gameplay and actual movement after
  bounded 300 ms A and S holds through Qt's keyboard path. Scan codes/down/up are
  correct. This does not reproduce the earlier intermittent failure.
- Console logs show automatic control revocation at 00:59:38 and 01:02:31 after
  OpenMW started at 00:59:05. The paired worker inferred physical input from
  screencast cursor positions; app recentering can trigger that same heuristic.
  Once ownership is revoked, keyboard and pointer events are correctly rejected.
- Replace that heuristic with a root-broker read-only evdev activity watcher:
  seat0 physical keyboard/pointer devices, no grabs/injection/key logging,
  software input devices excluded, hotplug supported. Reclaim requests and
  acknowledgements carry the current control generation. The legacy user-server
  replace detector is outside this paired Console/Virtual route.
- Standard negotiated RDP relative motion is distinct through InputHandler,
  worker wire12, Console/Virtual mapping and KWin/portal injection. Relative
  buttons/wheel and cleanup do not inject absolute warps. Cursor-settle nudges
  stop on entry; ordinary desktop absolute motion restores the usual behavior.
- Client has explicit View → Capture Mouse for Games, native compositor-confirmed
  pointer lock/relative motion, host-key release, focus/hide/surface/disconnect/
  ownership cleanup, fractional deltas and shortcut inhibition while captured.
  Native capture and installed-server acceptance are recorded above.
- Automatic capture was investigated separately after Steve's proposed behavior.
  KWin6.6.6 distinguishes surface `lockedPointer()` from `confinedPointer()`.
  Ordinary scripting/D-Bus does not expose it. The native plugin API explicitly
  requires recompilation with every KWin release. This release uses manual
  capture; do not guess from fullscreen/hidden cursors. A future supported
  observer must drive unlock/focus/disconnect release and suppress immediate
  recapture after a manual release, until deliberate click/re-entry.
- Focused pure gate passes: RDP W down/up and signed deltas; relative wire bounds,
  no geometry scaling/clamping/position-before-dispatch, held cleanup and real
  device activity classification. Existing input/wire suites pass. Controller
  gate verifies stale reclaim cannot revoke a newer owner and held W is released.
  No full suite, daemon/media test on Hal, wallet operation or game restart.

Evidence: `~/dev/rdp/evidence/2026-10-02-openmw-input/`.
KWin hook source: https://github.com/KDE/kwin/blob/v6.6.6/src/plugin.h,
https://github.com/KDE/kwin/blob/v6.6.6/src/wayland/surface.h.

## Original report and observed state

Steve reports Morrowind through OpenMW on Sol Console: he should be able to
move and look around, but cannot. He disconnected and deliberately left the
game running. The preceding connection was Hal (`192.168.48.93`) → Sol:3391.
Read-only inspection found OpenMW PID322976 and an Xwayland window with
`WM_CLASS=openmw`, `_NET_WM_PID=322976`, `_NET_WM_STATE_FOCUSED`.
No keys, mouse motion, game commands, screenshots or game configuration changes
were injected during diagnosis. Focus does not prove active gameplay versus a
paused/menu state, or the earlier client's keyboard focus.

Server logs contain absolute `Global pointer motion` positions reaching
`QPointF(1919,752)` on a 1920×1080 captured desktop. This demonstrates a finite
coordinate path, not continuous relative mouse input. No input drop/control
failure was established from the bounded log inspection.

## Confirmed implementation gaps

- Client `SessionView.qml` uses an ordinary MouseArea and explicitly allows the
  pointer to leave the window. `SessionModel::sendPointer()` maps/clamps movement
  into desktop coordinates. `SessionEngine` sends `freerdp_input_send_mouse_event`.
- Server `InputHandler::initialize()` has no `RelMouseEvent` callback.
- Plasma session pointer motion is injected through `pointer_motion_absolute`;
  the existing cursor-settle timer also injects absolute position nudges.
- Client pointer-position PDUs are deliberately ignored. They therefore do not
  provide a recentering fallback for applications that warp their pointer.
- The September 16 client design §5.5 excluded RDP relative mouse input; its
  September 17 amendment retained free pointer movement for normal desktop use.

Missing capture/relative motion is a concrete limitation for game mouse-look.
**Keyboard movement failure is not yet diagnosed.** It needs its own local
focus/key-delivery/game-state check; do not claim relative mouse support alone
fixes movement keys or infer an encoder/performance cause.

## Proposed implementation, as one task

1. Add an explicit **Capture Mouse** action for the active session view. Ordinary
   desktop use retains free pointer movement. Do not infer capture from cursor
   hiding, which terminal/text applications also use. Show a clear release hint
   using the existing host-key convention; preserve Escape for game menus.
2. Use native Wayland pointer-constraints and relative-pointer protocols on the
   client, following the existing ShortcutInhibitor lifecycle pattern. Bind
   native capability availability; prevent duplicate grabs, release on focus
   loss, modal UI, view removal, disconnect and unsupported protocol state.
   Handle fractional movement without per-event rounding loss; do not scale
   relative deltas through the video viewport or clamp them to monitor edges.
3. Use standard RDP relative-mouse capability negotiation and signed delta
   events, not a new private input channel. Advertise only routes that can
   deliver relative motion. Keep normal absolute input and unsupported routes
   explicit; no misleading successful capture against an unsupported server.
4. Preserve relative semantics through admission/control ownership, broker
   worker serialization and both Console/Virtual workers. Inject KWin relative
   `pointer_motion`; support portal relative motion where applicable or report
   unavailable. Never reinterpret a delta as an absolute coordinate. Suppress
   absolute cursor-settle nudges while game capture is active, with defined
   state reset when capture ends. Reject stale workers/owners and preserve
   existing pressed-key/button cleanup.
5. Verify keyboard focus and WASD key down/up delivery separately. Release all
   held keys/buttons when capture or the session ends. Capture must not consume
   movement keys, game Escape, or leave the host key held.

FreeRDP **3.22.0**, already pinned on Buzz, implements relative event send/receive
and the input capability bit. No FreeRDP package upgrade is needed just to add
this path. Sources:
[relative events](https://github.com/FreeRDP/FreeRDP/blob/3.22.0/libfreerdp/core/input.c),
[input capability negotiation](https://github.com/FreeRDP/FreeRDP/blob/3.22.0/libfreerdp/core/capabilities.c).
KDE's installed `fake-input.xml` has separate relative `pointer_motion` and
absolute `pointer_motion_absolute` requests.

## Done means

- Buzz → Sol Console OpenMW: hold W to move, release W to stop; turn repeatedly
  through 360° in both directions without an edge stop, jump or persistent spin.
- Game buttons and Escape/menu behavior work; the host release action restores
  ordinary local pointer movement immediately. Focus loss, dialogs and reconnect
  leave no grab or held input behind. Ordinary multi-window desktop pointer
  movement and drag behavior remain intact.
- Console and Virtual share the relative input path; capability-unavailable
  cases have an accurate explanation. Game capture is explicitly selected.
- One focused delta/ownership/lifecycle gate and one native Buzz→Sol game check
  suffice; reuse accepted codec/camera/TLS evidence. Steve checks Hal manually.
- Paired client/server packages are delivered with zero incoming RDP before
  restarts, preservation of the running game/configuration/desktop, and rollback.
  User confirms game behavior; no gameplay acceptance from packet receipt alone.

Implementation is delivered; the full user gameplay criteria above remain open.
This task creates no Goal or automatic queue run.

## Capture synchronization implementation (2026-10-02)

Implemented a disabled-by-default exact KWin 6.6.6 native bridge, shared worker
D-Bus adapter, paired wire 13, authenticated current-owner routing in both
Console and Virtual, and client 0.6.7 capture intent/acknowledgement handling.
The bridge snapshots actual lock requests (including pre-existing games),
separates request/activation/permission, and reads actual Alt+Tab grab state.
Confinement/cursor hiding/fullscreen are not automatic capture signals.

Right Ctrl releases local capture and suspends host constraints. A real app
unlock (usually a game menu) temporarily releases local capture without cancelling
armed intent; a renewed request resumes only in an active view with the pointer
inside. An explicit release cancels intent. Reconnect and new owner/compositor
grants start free. Relative input waits for local lock and correlated native host
acknowledgement. Epochs, decimal generations/revisions, bounded IDs/records and
current authenticated broker ownership reject stale commands/replies. Owner loss,
worker shutdown/bus death and a six-second lease deadline restore local behavior.
Only the client renews the lease (every two seconds); the worker cannot keep a
stalled client's capture override alive. An acknowledgement timeout sends a
compensating free request and stops renewal. No UI status polling was added.

The system package includes the native plugin, compiles the matching extracted
`kwin-dev`, and pins its exact `libkwin6`/`kwin-wayland` Debian versions. This is
private ABI integration: upgrades require a rebuilt/revalidated bridge. Missing
support disables synchronized capture with an explanation. Standard/older endpoints
retain the existing manual relative-input fallback without claiming synchronization.

Focused evidence: `~/dev/rdp/evidence/2026-10-02-cursor-release/`:

- Native isolated Sol compositor: pre-existing lock, free/recapture, app lock
  destruction/recreation, stale generation/epoch, real worker adapter grant change,
  disconnect cleanup and lease expiry (4 including setup/cleanup).
- Worker endpoint: snapshot required, current generation/epoch, revoked grant and
  old-state rejection (3 including setup/cleanup).
- Production QML on Buzz: visible released pointer, preserved temporary-unlock
  intent, explicit-release suppression and manual recapture (4 including setup/cleanup).
- Real client model: notification cannot grant input, matching acknowledgement
  required, old IDs/generations rejected, revoked permission and reset clear input
  (3 including setup/cleanup).

These are focused behavior gates, not full gameplay/Virtual acceptance. At this
source checkpoint no native plugin or paired release is installed on a live
desktop. Steve's active Hal→Sol Console connection and OpenMW PID322976 are
preserved. Build paired candidates, stage rollback, wait for zero incoming RDP,
then run the bounded Buzz→Sol game check and give Steve the Hal client for
hands-on menu/host-key/reconnect acceptance. Do not restart the compositor/game.
