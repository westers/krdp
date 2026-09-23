# Microphone forwarding into an independent desktop

Transport implemented, deployed to Sol's isolated virtual broker, and live-tested
with the native Buzz client (2026-09-22). Physical microphone/browser-conference
and camera acceptance are separate.

Reuse the existing physical-console microphone transport: AUDIN on the RDP
connection, bounded external PCM queue, generation-tagged worker IPC, and the
worker-owned `ConsoleMicrophoneSession`. The independent desktop already starts
that worker with `--desktop-media` and its private PipeWire environment. No
microphone source or capture graph belongs in the root broker or physical session.

Only the authenticated attached owner of the exact ready VirtualUser endpoint
can enable the microphone. Each activation binds the worker control generation
and a new request ID. AUDIN consent stays off until the matching worker source
readiness acknowledgement; failure or a four-second timeout reports microphone
failure. Playback and host-silence settings remain independent of microphone
startup failure. Camera remains explicitly unsupported on this transport.

A 20ms timer forwards at most one bounded existing AUDIN queue chunk per tick,
rechecking attachment authority. Do not accumulate retries of old speech on a
slow worker socket. Mic-off, detach, revoke, worker loss and disconnect stop both
timers, clear consent/queued PCM and disable the worker source. Stale replies or
audio from a prior request/attachment must not enable a replacement session.

Acceptance: transport tests exercise correlated readiness, timeout/error, stale
replies, ownership loss, off/on, teardown, bounded forwarding and callback
destruction. Existing private-PipeWire tests cover the actual source. Live
acceptance still requires a synthetic source sent from the native Buzz client
into a disposable Sol desktop, source removal on disable/detach, and reconnect
with fresh consent. No physical mic/camera capture is needed for that test.
This work is independent of the parked maintenance guard; uncertain crashed
sessions stay recorded for manual cleanup.

Verification (2026-09-22): 55 non-maintenance CTests passed, including existing
console microphone, private-graph PCM and virtual-session playback tests.
Transport's 30 QtTest cases also passed ASAN/UBSAN (Qt/KRdp shared libraries
are not sanitizer-instrumented). Synthetic IPC tests cover the forwarding and
teardown logic; they do not fabricate a real PAM/RDP login or prove end-to-end
AUDIN capture. The unchanged client already resends saved opt-in media policy
after a correlated successful virtual attach.

Live Sol/Buzz acceptance passed using a synthetic997Hz source, not a physical
microphone: private PipeWire capture mean -24.7dBFS, reconnect/private Pulse
capture mean -25.0dBFS. Playback to Buzz concurrently measured -26.0dBFS;
all three monitored Sol physical outputs contained exactly zero PCM samples.
Detach removed the source and restored the private default; reconnect renewed
consent with a new worker generation. The disposable desktop stopped normally;
the older uncertain failed records were preserved. No physical service restart
or routing change. Detailed recordings/logs/results are in the shared workspace
`rdp/evidence/virtual-microphone-eb1da3c/RESULT.md`.
