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
