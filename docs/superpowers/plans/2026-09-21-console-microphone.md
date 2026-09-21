# Physical console microphone forwarding

Required for Steve's conferencing goal and mic-only audio priority. Existing
regular-session AUDIN publishes through PipeWireMicrophone in the desktop
process. The privileged console host has no desktop graph and must never
publish a microphone into its own/root PipeWire context.

## Runtime contract

- Only the authenticated console controller may inject microphone audio.
  Playback-only viewers retain playback access but cannot publish a microphone.
  Release, local takeover, disconnect and ownership transfer revoke consent;
  reacquisition needs a fresh media request. Camera remains separately gated.
- Configure AUDIN for existing PCM48kHz/S16LE/stereo. Add an external-input
  destination to RdpConnection before enabling the channel; receive into a
  mutex-protected bounded queue, not an unbounded queued Qt signal per packet.
- Stop/close AUDIN and clear pending PCM on revocation. Callback admission must
  check current consent; late callbacks cannot publish after consent is removed.
  Current regular-session code initializes AUDIN once but lacks that live
  revocation path: fix it as part of this work, not just for console mode.
- Broker drains only current controller PCM into its authenticated worker socket,
  with bounded writes and generation-scoped records. Drop stale captured audio
  across worker replacement/logind handoff; never replay it into a later session.
- Worker creates PipeWireMicrophone only in the intended desktop session after
  explicit consent; do not create it in SDDM. Acknowledge actual source startup
  or refusal, and do not report successful microphone delivery merely because
  the host queued a policy record. Destroy source/queue on revoke/Stop/socket loss.
- Bounds: PCM records integral stereo frames, maximum20ms per message (3840
  bytes at48kHz), bounded total capture queue and worker socket backlog. Prefer
  fresh speech to replaying old buffered audio after congestion.

## Acceptance

Tests cover unauthenticated/viewer refusal, mic-only consent, release/reacquire,
late data/generation, queue bounds, malformed PCM and failed worker source startup.
Real Buzz GUI→Sol desktop test uses known non-silent input and captures the
virtual source there; verify consent off/release stops it and no physical/default
microphone routing changed. Test mic-only audio-priority acknowledgement and
asymmetric uplink pressure separately from downstream video steering.

Initial implementation: ConsoleControl policy now carries a controller-only
microphone bit, cleared on release; unit tests include mic-only, refusal and
fresh consent after reacquisition. Broker still rejects microphones until the
actual transport/source/revocation path is integrated and verified.

Regular-session revocation implementation now uses MicrophoneConsent, a
mutex-serialized enabled/generation gate around callback delivery. Each AUDIN
context records its consent generation; repeated identical consent is a no-op,
but off→on invalidates old callbacks even if the session loop never observed
the disabled interval. The session loop closes/joins a revoked/stale context
before freeing it or destroying its PipeWire endpoint, then may construct a
new generation. It checks current consent again before opening a channel.
Disconnect also revokes delivery first. Unit tests verify stale-context refusal
and idempotent consent; actual AUDIN off/on transport/device acceptance remains.
PCM callbacks reject non-stereo-frame-aligned or >1-second data buffers; the
future worker-wire packet limit remains20ms as specified above.

External AUDIN destination is now available on RdpConnection, selected only in
Initial state before initialization. It skips local PipeWireMicrophone entirely
and sends consent-gated callbacks into MicrophonePcmQueue. The queue is bounded
to200ms, timestamps each chunk, expires samples older than250ms, drains at most
20ms per call, validates stereo-frame alignment and discards previous consent
generations. A stale reset cannot erase a newer generation. No console broker
selects this route yet: worker IPC/source startup acknowledgement and lifecycle
integration must be completed before lifting its microphone rejection.

Worker IPC now defines separate microphone policy, result and PCM records,
correlated by controller generation and per-activation request ID. Parsers reject
zero IDs, malformed/trailing data, unaligned PCM and packets over20ms; endpoint
delivery requires worker Ready and bounds queued socket writes. Worker/broker
consumers are still pending; this is not live microphone forwarding.

PipeWireMicrophone now exposes asynchronous Starting/Ready/Failed/Stopped state.
Ready means PipeWire reached PAUSED or STREAMING, not merely that connect was
submitted. A private, hardware-free graph test proves readiness without a
consumer, stop/reopen, daemon-loss failure and missing-graph refusal. The worker
must poll this state with a bounded startup deadline before acknowledging enable.

Console worker now owns ConsoleMicrophoneSession: controller-generation and
strictly increasing request-ID admission, 3s startup deadline, actual readiness
acknowledgement, runtime failure notification, and immediate source destruction
on control change/local takeover/Stop/socket loss. Old activation PCM is refused.
Launcher grants --desktop-media only when both target adapter and freshly read
logind data identify an active physical user; default/manual workers and greeters
refuse microphone activation. Private-graph tests exercise off/on, revoke,
ownership change, stale PCM/policy, greeter/viewer refusal and daemon loss.
Broker wiring and real Buzz-to-Sol sample delivery are still pending: console
microphone requests remain explicitly unsupported until that is complete.

Broker integration now selects the external AUDIN route before initialization,
accepts controller mic policy only with a ready PhysicalUser worker, and enables
AUDIN only after the matching generation/activation startup result. A4s broker
deadline complements the worker's3s source deadline. A20ms timer drains bounded
PCM and discards sends under socket pressure rather than retrying old speech.
Control loss, worker failure/replacement and handoff revoke consent and clear
queued PCM; a successor desktop needs fresh consent. Camera remains refused.
New ConsoleHostControllerTest exercises actual connection consent/priority state
for stale acknowledgements, mic-only readiness, source failure and revocation
without initializing RDP sockets. This supersedes the earlier broker-rejection
notes; live Buzz-to-Sol sample delivery and AUDIN re-open remain unverified.

PipeWireMicrophonePcmTest now verifies actual sample delivery, not just source
registration: a hardware-free private graph plus isolated D-Bus/WirePlumber
policy, synthetic997Hz stereoS16 input into PipeWireMicrophone and independent
pw-cat recording of that exact node. One measured run:131072 frames at48kHz,
RMS2389.38/32768 and997Hz coherent energy fraction0.659658; test requires
non-silent RMS plus fraction>0.25. Three repeated runs passed. This proves the
worker's source implementation can publish usable audio; it is NOT evidence
for client AUDIN, broker transport, hardware mic capture or conferencing.
