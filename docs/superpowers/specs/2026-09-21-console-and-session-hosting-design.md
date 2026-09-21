# Physical console and virtual-session hosting design

## Outcome and order

1. **Physical console mode first.** An RDP connection made while the machine
   is at its SDDM login screen shows that login screen. After authentication it
   continues into the *same* seat0 Plasma session that is visible on the
   physical monitors. Disconnecting must not log out or otherwise disturb that
   console session.
2. **Virtual-session mode second.** A connection can instead create or resume a
   persistent, monitor-independent Plasma session. That is a different mode;
   it must never silently replace the physical-console behavior.
3. Remote playback is delivered to the client without also playing through the
   host's physical speakers when the client requests host silence.

## Evidence on Hal

Hal uses SDDM's Wayland backend (`CompositorCommand=kwin_wayland --drm ...`).
The normal desktop is logind session `3`, `Type=wayland`, `Class=user`,
`Seat=seat0`, launched by SDDM's `sddm-helper`. SDDM explicitly terminates its
greeter as part of a successful login. Therefore a server process embedded in
either the greeter or the user session cannot preserve an RDP connection across
the greeter-to-Plasma transition.

The existing `krdpserver --plasma` is intentionally a *user* service. It uses
KWin's private screencast and fake-input protocols and can only attach to an
already-running user compositor. This is the correct user-session backend but
not a login-screen server.

## Architecture

`krdp-console-host` is a system service and owns each TLS/RDP transport from
TCP accept through disconnect. It does **not** draw a second login UI and does
not acquire raw DRM or evdev access.

```
client RDP transport (owned by krdp-console-host)
       │
       ├── greeter adapter: SDDM compositor capture + KWin fake input
       │                         │
       │                     SDDM/PAM login
       │                         │
       └── seat adapter: active logind seat0 user session
                                  │
                          existing --plasma capture/input backend
```

The host observes logind's active-session change. The greeter adapter and seat
adapter are replaceable producers/consumers below the stable RDP transport:
the transport, authentication state, channel state, clipboard policy and client
connection stay alive while the adapter switches. The adapter handoff must
force a graphics reset/keyframe and revoke the old input endpoint before it
allows input to the new one.

The SDDM adapter is an explicitly installed integration component launched in
the greeter's Wayland environment, not a root process attempting to join an
unrelated Wayland socket. It exposes only a narrowly authenticated Unix socket
to the system host for encoded frames and normalized input. This keeps KWin's
Wayland permissions intact and prevents arbitrary system processes from
controlling the login screen.

The initial physical-console admission policy is one controller for seat0;
additional authenticated connections are viewers unless the current controller
releases control. Local mouse/keyboard activity remains authoritative and can
take over, matching the existing virtual-output safety rule.

Implementation progress (2026-09-21): the worker compares capture cursor metadata
against injected remote MouseMove events using the existing TakeoverDetector.
Its report carries the current controller generation; the broker ignores stale
reports after ownership changes. The worker immediately gates input and releases
held keys/buttons; the broker demotes the controller to viewer and restores host
audio routing. This is inferred pointer activity, not raw-device identification:
application pointer warps can also trigger it. In Plasma, `Meta+Ctrl+Alt+T`
(KRDP Physical Console → Reclaim physical console) uses KGlobalAccel to request
the same takeover. It is active only while there is a remote controller and
preserves the viewer connection. It deliberately does not auto-start the desktop
shortcut daemon in SDDM, so greeter keyboard takeover remains open where that
service is unavailable. Real physical-device acceptance remains required.

Sol acceptance at a9d7f21: invoked the registered `reclaim-console` action through
KGlobalAccel's component interface while the actual Buzz GUI was connected with
host silence enabled. Broker logged local takeover, GUI became viewer with no
owner while TCP stayed established, and default audio returned from the private
KRDP sink to SteelSeries analog-chat. This proves the action/worker/broker/media
path, not a physical keypress or greeter shortcut availability.

## Physical-console Fit amendment (Steve's requested host resolution change)

Physical Fit changes the actual physical output mode and desktop scale. It must
not reuse the normal layout executor's dark-monitor/virtual-stand-in behavior.
Only the authenticated current controller may request it. The broker validates
the request and forwards it with its control generation to the selected worker;
the worker runs kscreen-doctor in that compositor's environment, never as root.
The client needs an advertised capability before enabling physical Fit, and a
correlated result must not complete an unrelated layout/media operation.

The worker first reads available modes and records the original mode/scale.
ConsoleResize.h selects an exact advertised native-pixel mode, preserving the
current mode when possible and otherwise the closest refresh rate. Unsupported
sizes must be reported explicitly, not silently replaced by client scaling.
Sol also advertises kscreen-doctor custom-mode support; safe custom-size handling
remains follow-up, not something the first planner claims to implement.

Application must be asynchronous and verified against a fresh output readback;
exit status alone is insufficient. Resize suspends pointer-takeover inference
during compositor churn, gates stale input coordinates, and requires a fresh
capture layout/keyframe before success. Preserve other outputs and validate the
resulting capture dimensions. Roll back failed applies; restore temporary mode
and scale on release/disconnect/handoff only while they still match KRDP's last
applied values, preserving independent local display changes. Tests must prove
an actual non-no-op mode change on Sol and clean restoration. Hal is not a resize
test target. The planner is implemented first; worker execution/transport/client
Fit and runtime acceptance are still incomplete.

Implementation follow-up: ConsoleResizeExecutor now provides asynchronous
kscreen-doctor read/plan/apply/readback, a five-second per-command timeout,
bounded helper output, no-op avoidance, conditional restore, and field-wise
rollback of partial applies. Tests inject command replies; no physical modeset
has been exercised yet. Worker IPC Resize/ResizeResult records carry bounded
output/size/scale plus request ID and control generation. The authenticated
endpoint forwards them only after Ready. Neither primitive is connected to
Worker/ConsoleHostController resize lifecycle yet: integrate ownership changes,
handoff/disconnect restoration, capture/keyframe settling, and capability/client
handling before enabling it. Process exit or mode readback alone is not proof
that the RDP stream has adopted the new geometry.

## Virtual sessions (after console mode)

The virtual-session host uses the same stable transport and adapter API, but
its session manager creates/reuses an explicit user-owned headless Plasma/KWin
instance. Its lifecycle states are `active`, `disconnected-retained`, `locked`,
and later `frozen`. It never claims to be the physical console and never
changes seat0's monitor layout.

## Remote audio and host silence

Current RDPSND captures the default physical sink's monitor. It mirrors audio
to the client but leaves the local sink audible. Muting that sink is not an
acceptable implementation: depending on the PipeWire graph, the monitor may
also become silent, and it would overwrite a user's independently chosen mute
state.

Instead, the session audio router will create a session-owned PipeWire sink
with no physical-device link. Remote-session application playback is targeted
to that sink; its monitor feeds RDPSND. The router records and restores each
moved stream's prior target on teardown. `silenceHost=true` selects this route;
`false` retains the existing mirror-to-client behavior. The policy is
per-connection and must be rejected for a physical console unless that
connection owns control, avoiding one viewer silencing a person at the desk.

## Acceptance requirements

- From SDDM, a client sees the real greeter, enters valid credentials, observes
  the same already-visible physical Plasma desktop, and local input sees the
  same changes.
- Disconnect/reconnect preserves console apps and the current desktop.
- Greeter-to-seat handoff emits one reset/keyframe, has no stale input path,
  and does not expose a raw system input endpoint.
- With `silenceHost=true`, a known PCM fixture reaches Buzz through RDPSND while
  no audio reaches Hal's physical sink; disconnect restores original routing.
- With `silenceHost=false`, host audio behavior is unchanged.
