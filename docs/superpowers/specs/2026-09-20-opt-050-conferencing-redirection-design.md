# OPT-050 design: conference audio and camera redirection

## Outcome

An application running in the remote Plasma session on hal9000 can select a buzz-local
microphone and camera, while its playback audio reaches the selected buzz-local speaker.
No unselected laptop device is exposed. A device disappearing is reported as unavailable
without ending the RDP desktop session.

## Scope and transport

This is standard RDP media redirection, not a capture of the desktop video stream and not raw
USB passthrough.

| Direction | RDP transport | Local endpoint | Remote endpoint |
|---|---|---|---|
| hal application -> buzz speaker | RDPSND | selected PipeWire sink on buzz | PipeWire capture of the session's playback stream on hal |
| buzz microphone -> hal application | AUDIN | selected PipeWire source on buzz | per-RDP-session PipeWire virtual source on hal |
| buzz camera -> hal application | RDPECAM | selected V4L2/PipeWire camera on buzz | per-RDP-session virtual camera on hal |

Raw URBDRC USB forwarding is excluded from this first implementation. It has much broader device
and security semantics, is not built server-side in the installed FreeRDP, and does not give a
conference application a normal camera identity.

## Policy and configuration

Each saved client connection stores three opt-in selections: playback sink, microphone source,
and camera. Device IDs must be stable PipeWire serial/path identifiers rather than volatile node
numbers. `none` is the default for microphone and camera; playback defaults to the local default
sink. The settings UI exposes enable switches and a device chooser, and a connected session can
turn each redirection on/off without reconnecting.

The server creates no physical microphone/camera device and opens no client-directed media channel
unless the client has explicitly enabled it. The client asks PipeWire for access under the buzz
session; loss of permission/device becomes a channel-level unavailable state visible in the UI and
to the remote application.

## Audio design

The client owns a PipeWire audio bridge per direction. Its RDPSND endpoint accepts the server's
negotiated PCM/Opus format and writes to the selected sink. Its AUDIN endpoint captures the chosen
source, converts/resamples to the negotiated format, and sends timestamps continuously. The server
uses FreeRDP's RDPSND/AUDIN server contexts and PipeWire streams on hal. AUDIN arrival creates a
named virtual source for the RDP user/session; teardown removes it. Echo cancellation is delegated
to PipeWire's selected source/filter, never improvised in the RDP framing path.

Initial acceptance codec is PCM 48 kHz stereo playback and PCM 48 kHz mono microphone; Opus is a
later optimization only after interoperability and latency measurements.

## Camera design

The client implements the RDP camera channel against the installed FreeRDP RDPECAM APIs and offers
only explicitly selected cameras. It captures from PipeWire where available, with V4L2 discovery as
the device-identity fallback. The server implements the RDPECAM device side and publishes each
active camera as a session-owned PipeWire virtual camera (and a V4L2 loopback node only where an
application requires it). Initial media types are MJPEG and YUYV/NV12 at 720p30; do not introduce
H.264 camera transport until the baseline call works.

## Safety and acceptance

- Never use a live :3389 session for disruptive media work; use isolated server/client configs.
- Verify selected-device-only exposure, hot unplug/replug, and clean channel/session teardown.
- WebRTC acceptance is a real call from a browser in the RDP session: remote speaker playback,
  laptop microphone input, camera preview and outbound camera, measured audio round trip, and no
  echo runaway.
- Verify physical display state, empty KRDP state directory, plasmashell response, and `LockedHint`
  after every output-changing test.
