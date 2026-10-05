# Camera menu stays in Error after login at the Console greeter

Status: diagnosed 2026-10-04; implemented 2026-10-05 as OPT-058 (see "Implemented wire design" at the end). Candidates built, NOT installed; live check pending Steve.
Server `farside-server` on Sol (Console :3391), client `farside-client` 0.6.x on Hal.
Client paths below are relative to `~/dev/krdp-client`, server paths to `~/dev/krdp`.

## Symptom (Steve, 2026-10-04 evening)

Hal client to Sol Console :3391, connected while Sol showed the SDDM greeter:

1. A warning said the camera could not be connected because it needs an already
   logged-in session.
2. After logging in through the remote greeter, the Devices menu Camera entry
   still had a red X icon. It was clickable, and clicking it did connect the camera.
3. Opening the menu again, the icon was no longer red.

His hypothesis: the client did not re-sync device state after the login.

## Evidence

Sol `journalctl -u farside-console-host` and Hal `journalctl --user` (client PID 2151791),
same minute, CDT, 2026-10-04:

| Time | Where | Line |
|---|---|---|
| 22:49:02 | Hal client | `KRDPCTL: capabilities {"devices":{"camera":{"reselect":true,"toggle":true},...` (camera advertised as toggleable at the greeter) |
| 22:49:02 | Hal client | `devices: camera on asked as r2` (remembered start mode; `sendInitialDevices()` right after the capabilities) |
| 22:49:02 | Hal client | `devices: camera answered r2 error  unavailable` (refusal, with the "logged-in desktop" message) |
| 22:49:02 | Sol host | greeter worker (PID 5586) streaming; no camera log line for the refusal (it is a plain reply, not logged) |
| 22:49:16 | Sol host | greeter KWin gone, `Console worker exited: 255` (login) |
| 22:49:17-18 | Sol host | `Session 96 ready after 308 ms`, `Console worker ready: "96" forwarding true` |
| 22:49:19 | Hal client | `host layout: 1 monitor(s), owner 1, you owner; 1 view(s) re-mapped` (the only client-visible event of the handoff) |
| 22:49:30 | Hal client | `devices: camera on asked as r22` (manual click; no device record of any kind arrived between 22:49:02 and here) |
| 22:49:31 | Sol host | `RDPECAM device channel ready`, `KRDP remote camera publishing V4L2 loopback "/dev/video10"`, `Device "camera" is "on"` |
| 22:49:35 | both | camera off (Steve's test), `answered r26 off` |

Control run: on the next two connects (22:53:12, 23:01:33) the worker was already the
logged-in session 96; the same remembered `camera on` at connect went `starting` then `on`
within one second, so the loopback, PipeWire and RDPECAM path is fine. Only the greeter
timing is affected.

## Root cause

The capability says the camera is available (it only checks the V4L2 loopback bridge), but
the actual request is refused while the worker is the greeter. The refusal is stored as a
sticky Error that nothing re-evaluates, and the client clears the user's wish for the
camera because of it.

### (a) What the server reports, and what it sends after login

- Capabilities are built once per client: `server/ConsoleHostController.cpp:1825-1841`
  (`capabilitiesSent`), using `CameraAvailability::capabilities(ConsoleDeviceCapabilities,
  CameraAvailability::reason(loopback))` (`src/CameraAvailability.h:18-62`,
  `ConsoleDeviceCapabilities` `ConsoleHostController.cpp:1552-1558` with
  `.cameraToggle = true`). The reason depends only on the loopback device, never on
  whether a user session is present. So the greeter advertises a working camera
  (log line 22:49:02). Confirmed.
- The camera request itself is refused at `ConsoleHostController.cpp:1645-1650`
  (`!m_inputEnabled || !m_endpoint.ready() || adapter != PhysicalUser`) with a normal
  `device` record: `state=error`, `code=unavailable` (`DeviceControl::Unavailable`,
  `src/DeviceControl.h:90`), message "camera requires a ready logged-in desktop". The
  worker-level twin is `server/ConsoleCameraSession.h:68` ("camera requires control of a
  logged-in desktop"), which the broker pre-check normally pre-empts. The microphone has
  the same pre-check (`ConsoleHostController.cpp:1676-1679`).
- After login the server sends nothing about devices. On `workerReady`
  (`ConsoleHostController.cpp:262-277`) it re-arms outputs, control state, quality and
  media only; `sendLayouts()` (`:1843`) republishes the monitor layout only. The only
  device pushes are camera demand/in-use (`:206-218`), `stopCamera` (`:2224-2235`) and
  `cameraResult` (`:2255+`), all tied to a running camera. There is no availability or
  capability refresh message. Confirmed (log: no device record between 22:49:02 and 22:49:30).

### (b) What the client does with it

- At connect the wish is the stored start mode: `src/ui/SessionModelDevices.cpp:74`
  (`m_deviceWanted = mode == On`). `sendInitialDevices()` (`:489-512`, called from
  `src/ui/App.cpp:633` and again on `consoleControlAcquired`, `App.cpp:661`) sends
  `camera on` because the capabilities said toggle (`deviceToggleCapable`, `:113-126`).
- The `unavailable` reply goes to `DeviceState::Machine::reply`/`apply`
  (`src/core/DeviceState.h:129-145`, `:233-255`): status Error, and `m_wanted = false`
  (`:244`). `deviceSettled()` (`SessionModelDevices.cpp:280-299`) then sets
  `m_deviceWanted = false`, closes the local gate, and `setDeviceNotice` (`:337+`, the
  `unavailable` branch at `:354-358`) raises the red warning with a "retry" action.
- The red X is purely `info.state === "error"` in `src/qml/SessionActions.qml`
  `DeviceAction` (`icon.name: ... "dialog-error"`), and the text suffix "Error"
  (`deviceText`). `Machine::state` leaves Error only when a new request is sent
  (`sent()` `DeviceState.h:104-126`), a push arrives (`push()` `:147+`, none came),
  `reset()` (connection loss), or `stopLocally`.
- Why the click worked: the entry stays `enabled` because `toggle` is
  `deviceBlockedReason().isEmpty()` (`SessionModelDevices.cpp:113-146,176`), and that
  looks only at capabilities and mode, not at the Error state. `toggleDevice`
  (`:297-306`) treats Error as "off" and sends `on` (`:213-221`), which succeeded
  because the worker was now session 96. Not a client bug in itself.
- Why the next menu open was fine: after the successful `on`, `Machine` is On; the
  red icon was the stale Error from 22:49:02, not live state. Confirmed by the 22:49:30
  to 22:49:31 sequence.
- Nothing re-syncs: `consoleControlAcquired` only fires on a `console-control acquire`
  ack (`src/ui/SessionModelControl.cpp:840-842`); the worker swap does not change the
  controller (log "owner 1, you owner" unchanged), so it never fired; and even if it
  had, `sendInitialDevices` skips devices whose wish is false, which the refusal set.
  The "capabilities" handler (`SessionModelControl.cpp:651-668`) reacts to a changed
  capability, but the server never resends one.

### (c) Server or client?

Both, as a missing contract rather than a single defect. Confidence: confirmed for the
mechanism (the logs, plus the code paths above); likely for "this is all of it" (no
other trigger was seen).

- Server: advertises `toggle:true` at the greeter, refuses with a code the client
  treats as a permanent failure, and never announces that the condition has ended.
- Client: treats a transient, state-dependent refusal as a terminal Error, discards the
  user's wish, and has no path that retries when the host layout/worker changes.

### (d) Related edge cases (code-read; none exercised live, so likely or unproven)

- Microphone: identical (`ConsoleHostController.cpp:1676-1679`, same client path). Likely
  also shows a red X when remembered "on" and connecting at the greeter. Not reported.
- Lock screen: `m_inputEnabled`/`m_endpoint.ready()` conditions cover a worker that is
  not yet ready; whether a locked PhysicalUser session counts as unavailable depends on
  `m_inputEnabled`, which was not checked here (unproven). The product contract says a
  locked Console must stream, so camera behaviour while locked needs its own decision.
- Logout or switch user with a camera running: `workerStopped` calls
  `stopCamera(Unavailable, "console camera worker stopped")` (`:277-286`) so the client
  gets Error/unavailable and, per the same client rule, loses the wish; after the next
  login nothing restores it. Likely same bug in the other direction.
- Virtual: capabilities carry a static `virtualReason()` (`CameraAvailability.h`), so the
  menu says "Unavailable" up front; not affected by login. A `virtualSessionAttached`
  re-send exists (`App.cpp:765`).
- Reconnect: starts fresh (`resetDevices`, `SessionModelDevices.cpp:544`) and
  `sendInitialDevices` runs again, so a reconnect after login works (control run above).
- Standard (non-KRDPCTL) RDP clients: `startStandardCamera` (`ConsoleHostController.cpp:
  2204-2210`) silently returns when the worker is not a PhysicalUser and is only called
  from the admission path; likely no retry after login either (unproven).

## Proposed fix

Principle: availability of a camera is a host state that changes at runtime; the client
should show "waiting for a signed-in desktop", not Error, and should start it by itself
when the host says the condition ended, but only for a camera the user already wanted
(consent unchanged).

### Server (ConsoleHostController, `server/ConsoleHostController.cpp`)

S1. Make the refusal self-describing. In the camera and microphone pre-checks
(`:1645-1650`, `:1676-1679`), when the reason is "not a PhysicalUser worker yet" reply
with a distinct code, for example `needs-session` (add to `src/DeviceControl.h` next to
`Unavailable`), still `state=error`. Keep `unavailable` for real failures. Older clients
treat unknown codes by their generic branch, so this is compatible in practice.

S2. Announce the change. Add one push record, `device-availability`
`{v:1, device, available, reason}` (or reuse a capabilities resend limited to the
`devices` block; pick whichever fits the KRDPCTL v2 contract, record it in
`~/dev/rdp/KRDPCTL-V2-CONTRACT.md` and advertise it via a capability bit). Send it:
after capabilities when the worker is not PhysicalUser (so the first state is accurate),
and from `workerReady` (`:262`) and `workerStopped` (`:277`) and any lock change that
flips `m_inputEnabled`, to every admitted client. In the greeter the camera capability
reason should then be non-empty (`cameraToggle=false` plus the reason) or availability
`false`; at PhysicalUser it flips to true.

S3. Do not auto-start a camera on the server. Consent and RDPECAM enumeration are
client-driven; the client retries.

### Client (`src/ui/SessionModelDevices.cpp`, `src/core/DeviceState.h`, `SessionModelControl.cpp`, `SessionActions.qml`)

C1. `DeviceState::Machine`: on `needs-session` keep `m_wanted = true` and use a new
status "Waiting" (or Off plus a `waiting` flag) rather than Error; no red X and no
"retry" error notice, only an information notice or a menu line ("Waiting for a
signed-in desktop"). Keep the existing Error path for `unavailable`.

C2. Handle `device-availability` (and a changed `devices.camera.toggle` in a resent
capabilities, `SessionModelControl.cpp:651-668`) by calling the per-device part of
`sendInitialDevices` for devices with `wanted && available`. Also retry on the existing
worker-change hint (`host layout ... re-mapped` after an owner-unchanged worker swap) as
a fallback for servers that predate S2.

C3. Make the entry's `enabled`/icon follow the machine, not stale Error: after a failed
`on`, a later successful state or availability change must clear the Error (any push
already does; add an explicit clear when availability turns true).

C4. Microphone gets the same treatment (shared code), unless Steve wants it scoped to
the camera first.

## Test plan

### Unit and pure tests (no daemons, safe on Hal)

Server, `autotests/ConsoleHostControllerTest.cpp` (existing camera test near `:1230`) and
`autotests/DeviceControlTest.cpp`:

- T1: with the endpoint on a Greeter target, `camera on` yields a `device` record with
  `state=error`, `code=needs-session`; `query` yields Off. Pass: exact code string.
- T2: transition the fake handoff to PhysicalUser and `workerReady`; every admitted
  KRDPCTL client receives exactly one `device-availability {camera, available:true}`.
  Pass: one record per client, none to non-KRDPCTL clients.
- T3: `workerStopped` sends `available:false` and, if the camera was running, the
  existing Error/unavailable for that client. Pass: both records, in that order.
- T4: capabilities at admission carry the greeter-time state (camera not toggleable, or
  availability false). Pass: field values match.

Client, `autotests/DeviceStateTest.cpp` and the SessionModel devices tests:

- T5: `needs-session` reply leaves `wanted()==true`, status is not Error, `toVariantMap`
  has no `error` state. Pass: assertions.
- T6: then a `device-availability` available record triggers exactly one `device camera
  on` request; with `wanted==false` (user turned it off meanwhile) it triggers none.
- T7: genuine `unavailable` still ends in Error with `wanted()==false` (no regression).
- T8: QML scene (`tests/ux/scenes`, next to `devices-menu-*`): waiting state shows the
  camera icon, not `dialog-error`, and the entry text says waiting.

### Live check (Sol server, Buzz client; not Hal; needs Steve's go-ahead before running)

Preconditions: Sol at the SDDM greeter (log out the test user), no existing connection,
Buzz client with camera start mode "On" and a working webcam, installed packages from
the candidate build. Collect `journalctl -u farside-console-host --since` on Sol and the
client's user journal on Buzz.

1. Connect Buzz to Sol :3391 at the greeter. Pass: Camera menu entry shows "waiting"
   (no red X, no error warning); client log has `needs-session`; Sol log has no camera
   start.
2. Log in through the remote greeter with a PAM test user. Pass: within 10 s of
   `Console worker ready` on Sol, with no click, client logs `camera on asked`, Sol logs
   `RDPECAM device channel ready` and `Device "camera" is "on"`, menu shows On.
3. Repeat but turn the camera entry off while waiting. Pass: after login no camera
   request is sent.
4. Log out at the remote desktop while the camera is on. Pass: client leaves On, shows
   waiting; log in again, camera resumes (if the wish was kept), no red X at any point.
5. Microphone repeat of 1 and 2 (if C4 is in).
6. Regression: connect with the user already logged in (as 22:53:12). Pass: camera
   `on` within 2 s, as before.
7. Old client against new server and new client against old server: no crash, camera
   works after a manual click.

Fail criteria for the whole check: any red X while waiting, any camera started without
the wish, or any leftover `v4l2` consumer or process after disconnect.

## Not proven

- Whether the lock screen of a PhysicalUser session blocks the camera (depends on
  `m_inputEnabled`); needs a live lock test.
- Whether Steve's red X appeared exactly via this path in his client build; the client
  journal at 22:49:02 matches it exactly, but the screenshot was not seen.
- Behaviour of non-KRDPCTL clients and of the logout path (code-read only).

## Implemented wire design (OPT-058, 2026-10-05)

Messages and fields (KRDPCTL v2 style: capabilities first, unsolicited records carry no `requestId`):

| Name | Direction | Advertised by | Content |
|---|---|---|---|
| `device` state record, `code:"needs-session"` | server to client, reply to a camera/microphone `on` | n/a (new code of an existing record) | `state:"error"`, `code:"needs-session"`, message "camera|microphone needs a signed-in desktop on the remote computer". Replaces `unavailable` only when no ready PhysicalUser worker exists. `unavailable` stays for real failures (for example a client with no camera channel). |
| `device-availability` | server to client, unsolicited | `capabilities.devices.availability.push == true` | `{"type":"device-availability","v":1,"camera":{"available":bool,"reason":"needs-session"?},"microphone":{...}}`; one record per availability edge, sent to every client that received `capabilities`. |
| `capabilities.devices.availability` | server to client | n/a | `{"push":true,"camera":bool,"microphone":bool}`: the state at admission, so a client at the greeter waits without asking. Camera/microphone `toggle` flags are unchanged (they describe the bridge, not the session). |

Availability is `m_inputEnabled && endpoint ready && adapter == PhysicalUser`, the same condition as the old refusal. It is re-evaluated (edge only) from `setWorkerActive()` (worker ready/revoke, which covers workerReady, handoff, physical-lease and failed-topology paths) and `workerStopped`. The Virtual broker is unchanged (no push, no capability). The broker/worker wire is NOT changed, so no wire version bump.

Client (0.6.8): `DeviceState::Status::Waiting` (variant state `"waiting"`, menu icon `chronometer`, text suffix "Waiting for a signed-in desktop", info notice without a retry action). A `needs-session` reply, or admission-time availability false, leaves the wish set and the local gate closed. A `device-availability` available edge asks `on` once for each waiting, wanted device (camera, microphone). Repeats of the same state are ignored. Clicking a waiting entry: if the host last said available (race) it retries at once; if the host said unavailable it withdraws the wish (otherwise a waiting device could never be cancelled; deviation from "click retries").

Deviations and limits (decisions for Steve):
- Logout with a camera running: the server still ends it with `revoked` ("consent must be renewed"), which is a real consent reset, so the client shows the existing Error/turn-on notice and does NOT resume after the next login. Only the greeter/not-yet-signed-in case waits and auto-starts. Making logout resume as well needs the revoke code changed to `needs-session` in `setWorkerActive(false)`; not done (consent semantics).
- Old server with new client: the refusal is `unavailable`, behaviour as before. New server with old client: unknown code and unknown record type fall into the generic branches (red X as before), no crash. The "retry on the worker-change hint" fallback (C2) for old servers was not added.
- Standard (non-KRDPCTL) RDP clients get nothing new; their camera/mic auto-start path (`startStandardCamera`) is called from `setWorkerActive(true)` and already retries at login.
- Lock screen: availability does not depend on the lock state (`m_inputEnabled` stays true while locked); unchanged.

Tests (all daemon-free): server `ConsoleHostControllerTest` (greeterRefusesCameraAndMicrophoneWithNeedsSession = T1, availabilityIsPushedOncePerEdgeToKrdpctlClientsOnly = T2/T3, workerStopSendsCameraErrorBeforeUnavailable = T3 order), `LayoutControlTest::capabilitiesDeviceAvailability` (T4), `DeviceControlTest::availabilityRecordShape`; client `DeviceStateTest` (T5, T7), `SessionModelTest` (transcript, T6 x2, genuine unavailable), `ControlCapabilitiesTest`, `DevicesMenuTest::waitingForASignedInDesktopIsNotAnError` (T8, scene `tests/ux/scenes/devices-menu-waiting.qml`). The camera half of the server tests needs a loopback at /dev/video10 (skipped otherwise; the older `deviceRecordsOwnerViewerBusyAndUnsupported` now sets it the same way and passes on a host that has one).

### Live check for Steve (Sol server, Buzz client; never Hal; install the two candidates first)

Candidates: server `farside-server` from the OPT-058 commit and client 0.6.8 (paths in HANDOFF Log 2026-10-05). Preconditions: Sol at the SDDM greeter (nobody logged in), no connections, Buzz client with camera and microphone start mode "On" and a working webcam/mic. Collect `journalctl -u farside-console-host --since` on Sol and the user journal of the Buzz client.

1. Connect Buzz to Sol :3391 at the greeter. PASS: Devices menu shows Camera and Microphone as "Waiting for a signed-in desktop" with a clock icon; no red X; no error banner (only an info line); client log has no `camera on asked` (the admission capabilities already said unavailable); Sol log has no camera start. FAIL: any red X or error banner.
2. Log in through the remote greeter. PASS: within 10 s of `Console worker ready` on Sol, with no click, client logs `host desktop available: true` then `camera on asked` and `microphone on asked`; Sol logs `RDPECAM device channel ready` and `Device "camera" is "on"`; menu shows both On. FAIL: needs a click, or red X at any point.
3. Log out, reconnect to the greeter, turn the camera entry off while it waits, log in. PASS: no `camera on asked` after login; microphone still starts.
4. Camera running, log out at the remote desktop. EXPECTED (see deviations): camera ends with the existing "console changed users, turn it on again" notice; it does not resume by itself. PASS if that is what happens and nothing crashes or hangs. (This step documents current behaviour, not a new feature.)
5. Regression: connect with the user already logged in. PASS: camera `on` within 2 s as before (22:53:12 run), no waiting state shown.
6. Old client (0.6.7) against the new server and new client against the old server (`a8c7d6f`/`1859f57`): no crash; camera works after a manual click once logged in.
7. After each run: no leftover `v4l2` consumer or process after disconnect (`fuser /dev/video10`, `pgrep -af krdp-console-worker`).

Fail criteria for the whole check: any red X while waiting, any camera started without the wish, any leftover v4l2 consumer or process after disconnect.

## Delivery status and remaining live checks (2026-10-05)

Decision (Steve, 2026-10-05): logging out with a running camera ends it (`revoked`, consent must be renewed) and does NOT resume automatically after the next login. Only the greeter / not-yet-signed-in case waits and auto-starts. So step 4 of the live check above is the intended behaviour, not a gap.

Installed 2026-10-05: server `6.6.80+git202610051353.2bbd793-1` and client 0.6.8 on Sol, Buzz and Hal (see the HANDOFF Log 2026-10-05).

Bounded check by the agent, Sol server + Buzz client: NOT exercised. Reason: when it was due, Sol had a signed-in, locked `westers` session on seat0 (so the Console worker is ready for a user, not the greeter, and `needs-session` is not the expected answer), and reaching the Console KRDPCTL channel needs a PAM or alias login. No real user password was entered and no scratch broker was started on Sol against the live session. Server and client behaviour is covered only by the daemon-free tests listed above plus the installed package contract checks.

Steve to run (post-login half, needs a password entry; Sol must be at the SDDM greeter, nobody logged in, for step 1):

1. Make Sol show the greeter (log the local user out; do not restart sddm). From Buzz open Farside, connect to Sol Console (:3391) with Camera and Microphone start mode "On".
2. At the greeter: Devices menu shows Camera and Microphone as "Waiting for a signed-in desktop" with a clock icon, no red X, no error banner.
3. Sign in through the remote greeter with your own account. Within about 10 s the camera and microphone start by themselves with no click (menu shows On). A red X, or a need to click, is a FAIL.
4. With the camera on, log out at the remote desktop. Expected: the camera ends with the existing "turn it on again" notice and does not resume after the next login (decision above).
5. Reconnect to the greeter, turn the camera entry off while it waits, sign in: no camera request after login, microphone still starts.
6. Afterwards: `fuser /dev/video10` on Sol shows no consumer, and `pgrep -af farside-console-worker` shows only the live worker. Tell me the result so tag `v0.6.8` can be decided.
