# Desk activity signal (OPT-071)

Status: approved scope (Steve, 2026-10-14). Server wire 17 -> 18, KRDPCTL capability `deskActivity`, client 0.9.x.

## Problem

On a Console connection the remote client shows the host's desktop, but nothing tells the remote user that someone
is also using the physical desk (mouse or keyboard on the host). The video just changes under them. The client also
never draws the host's pointer, because RDP clients draw their own pointer where their own mouse is.

## What exists today

- `PhysicalInputWatcher` (broker, `farside-console-host`, system service) already opens seat0's evdev keyboard,
  mouse, touchpad and touchscreen nodes read-only and emits a bare `activity()` for any key-down, relative move or
  absolute event. It drops every key code in the callback. It excludes `/devices/virtual/input/` (uinput).
  `ConsoleHostController::physicalInputActivity()` uses it today to end Console Replace.
- KWin `fake_input` events (what the remote client's input becomes) never enter evdev. This is how the desk is
  told apart from the remote user: no new KWin capability, no worker involvement, nothing to filter.
- The worker already receives KWin's screencast cursor metadata, including the pointer position, for every capture
  source (`PipeWireCursor::position`), and `CursorTracker` deliberately discards it ("an RDP client moves its own
  pointer").

## Design

### 1. Detection (broker only)

The broker's `PhysicalInputWatcher` stays the only source. Self-injected input is excluded structurally:
fake_input is not an evdev device, uinput devices are skipped, and only `EV_KEY` down, `EV_REL` and `EV_ABS` count
(`EV_LED`, `EV_SYN`, `EV_MSC` do not, so lock-key LED sync cannot count). The device filter moves out of
`Private::add` into the pure `PhysicalInputWatcher::isDeskDevice(syspath, seat, properties)` so it is unit-tested.
The watcher fires on every event batch (hundreds per second for a mouse), so the broker puts a
`DeskActivityTracker` behind it (pure, fake clock): see section 3.

### 2. Signal payload (broker -> clients, KRDPCTL)

```
{"type":"desk-activity","v":1,"active":true,"seq":17,"context":"session"}   // someone is at the desk
{"type":"desk-activity","v":1,"active":false,"seq":18,"context":"session"}  // quiet for IdleMs
```

- No key code, key name, device name, device kind, button or coordinate. Exactly these keys, nothing else
  (a test pins the key set).
- `active:true` is sent at most `MaxRateHz = 4` times per second (leading edge at once, then one trailing record per
  250 ms window while activity continues). `active:false` goes once, `IdleMs = 5000` after the last event.
- `seq` counts records per connection (diagnostics only). `context` = `session` | `greeter` | `locked`, taken from
  the seat sessions the broker already watches (session class greeter, logind LockedHint). The client words these
  neutrally (see 6). `unknown` is never sent; no seat session means `session`.
- The client keeps its own receive clock. "Seconds since last activity" is `now - lastActiveReceived`, so server and
  client clocks never meet.

### 3. Host pointer (worker -> broker -> clients)

Standard RDP pointer PDUs do not fit. A server `Pointer Position Update` (TS_POINTERPOSATTRIBUTE) makes FreeRDP-based
clients warp their own pointer, which would yank the remote user's mouse. So: KRDPCTL, gap-only, advertised.

```
{"type":"host-pointer","v":1,"x":1234,"y":567,"visible":true}
```

`x`,`y` are in the client's RDP desktop coordinates, the same space as the client's pointer input (so the existing
`wire` atlas used for input is used backwards). The worker computes them: one workspace capture is the identity;
per-output captures add the output's wire origin to the stream-pixel position. The worker's `HostPointerTracker`
(pure) keeps per-source visibility like `CursorTracker`, sends only on change, at most every 50 ms (20/s).

Wire 18 adds the record `HostPointer` (worker -> broker only, after Ready; payload `int32 x, int32 y, uint8 visible`,
exactly 9 bytes, |x|,|y| <= 65535; a malformed one fails the worker like a malformed heartbeat). The Virtual worker
never sends it (`physicalActions()` is false). The Virtual broker's endpoint accepts and ignores it.

The broker relays `host-pointer` only to clients that subscribed with `pointer:true`, and only while desk activity is
live (last event within `ActiveHoldMs = 10000`): outside that window the host pointer is wherever the remote user
put it and carries nothing. The latest pointer is kept so the first `active:true` can be followed by one right away.
It is rate-limited again at the broker (50 ms) because the relay must not depend on the worker's limiter.

### 4. Capability and subscription

`capabilities.deskActivity = {"pointer":true,"maxRateHz":4,"idleMs":5000}` on Console only. Absent on Virtual and on
older servers. Old clients ignore the group, and are never sent the records: they must opt in with

```
{"type":"desk-activity-subscribe","v":1,"enabled":true,"pointer":true}
```

Subscription is per connection and lost on disconnect. On subscribe the broker answers with the current state
(`active:true` if the desk is live, else nothing). Controller and viewers can both subscribe; both receive. Because
a viewer never sends input, its client shows the ghost pointer whenever the host pointer is visible and the desk is
live; the controller shows it only where it differs from where its own pointer was last sent (tolerance 6 px).

### 5. Per-state behaviour

| State | Signal |
| --- | --- |
| SDDM greeter | sent, `context:"greeter"` (someone is at the desk typing a password: still true) |
| Lock screen | sent, `context:"locked"` |
| Session | sent, `context:"session"` |
| No controller (no admitted client) | nothing to send to; tracker still runs and is reset on connect |
| Virtual route | not applicable: no capability, no records, nothing watches evdev |
| Replace active | unchanged: desk input still ends Replace (OPT-060); the signal is sent as well |

"Neutral" means the client does not treat greeter/lock as an alert: same badge, no accent colour, text names the state.

### 6. Client

- `ControlCapabilities` parses `deskActivity` (all of it or none, like `clipboard`) and exposes `deskActivityAdvertised`,
  `deskActivityPointer`.
- Pure `DeskActivityModel` (src/core, fake clock): `onRecord`, `onPointer`, `active(now)`, `secondsSince(now)`,
  `ghost(now, lastSentPointer, isController)`; activity expires on its own `ActiveHoldMs` (10 s) if no `active:false`
  arrives (lost record, host restart), so the badge can never stay stuck.
- `SessionModel` subscribes after `capabilities` when the setting is on, feeds records, exposes `deskActive`,
  `deskSecondsSince`, `deskContext`, `hostPointer` (desktop coordinates), `ghostVisible`.
- Setting "Show desk activity" (default on, `SessionSettings`/Advanced page). Off = never subscribes, so the host
  sends nothing.
- UI: a small badge over the picture ("Someone is at {host}'s desk - 3 s ago", neutral wording on greeter/lock),
  a ghost cursor (a translucent arrow with a thin outline) at the host pointer, mapped through the view like input.

### 7. Wire changes

Wire 17 -> 18: `Kind::HostPointer` added at the end (`LastKind`), `ProtocolVersion = 18`. Broker and worker stay paired
(unchanged rule). Mismatch tests and the wire version pins move to 18. `KRDPCTL` `v` stays 1: new records and a new
capability group only.

### 8. Test plan (daemon-free)

- `DeskActivityTest`: limiter (leading edge, 4/s cap, trailing, idle edge, clock jumps), tracker states and contexts,
  record key set (no extras), `isDeskDevice` (virtual and non-seat0 excluded), `isActivity` (EV_LED/EV_SYN no),
  `HostPointerTracker` (change-only, 50 ms, visibility vote, multi-output origin).
- `ConsoleWorkerWireTest`: v18 pins, `HostPointer` round trip, size and range rejects, `LastKind`.
- `ConsoleWorkerEndpointTest`/`OrderTest`: accepted after Ready, rejected before Ready, malformed fails.
- `LayoutControlTest`: capability record, subscribe parse, records.
- `ConsoleHostControllerTest`: only subscribed clients receive; viewers receive; Virtual/old client nothing; the
  payload has no extra keys; physical input while Replace still ends Replace.
- Client: `ControlCapabilitiesTest`, `DeskActivityModelTest` (fake clock), `SessionModelTest` additions, QML smoke through
  the existing offscreen tests.

## Open questions (recommended answers)

1. Should the controller's own ghost also show when the host pointer equals its pointer? No: only where it differs
   by more than 6 px.
2. "Don't flag activity when I'm the desk user": not implemented. The client cannot know it is the desk user (it is a
   remote machine), and "Show desk activity" already turns everything off. Revisit only if Steve connects from the
   host itself (loopback); then suppress on a loopback address.
3. Keyboard-only desk activity has no pointer to show: the badge shows, the ghost does not move. Recommended: fine.
4. Should the ghost be visible to viewers all the time? Recommended: only while the desk is live (as designed).
5. Stream-pixel units of KWin's cursor `position` on per-output captures are assumed (output pixels, origin at the
   output). Unverified without a live capture: first item on Steve's checklist. If KWin reports logical units on a
   fractional-scale output, the worker mapper needs the output scale (the data is already in the worker).
6. Desk-side tray indicator, sounds and auto-viewer semantics: later, out of scope.
