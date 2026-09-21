# Live audio-first media priority

Steve requests a setting that prefers audio quality and low latency over video
quality and can change during an active connection. This is separate from
audio codec/bitrate adaptation, which was discussed but is not implemented.

## Contract

- Server default `PreferAudioQuality=false`; client Advanced override with a
  debug-action hook. Changing the setting sends a versioned, correlated control
  request immediately to connected sessions, without reconnecting or reopening
  their audio codec. New sessions receive the saved preference.
- Explicit capability and acknowledgement. Unsupported/older servers must be
  shown as unsupported, not as having applied the policy. Invalid records must
  not partially change media routing or permissions.
- Effective when playback OR microphone redirection is enabled (Steve explicitly
  includes mic-only sessions). Under existing measured RTT
  or sustained video-backlog pressure, preserve the negotiated audio format
  and reduce video more aggressively. Do not permanently lower video quality
  on a clear link or interpret high stable WAN RTT as congestion.
- Initial tested policy: video quality steps down20 instead of10; if AVC444
  chroma is present, shed it AND step quality down in the same interval.
  Restore quality by2 instead of5 while enabled. Keep existing jitter guards,
  minimum/cap bounds and startup-backlog protection. Restore normal steering
  when disabled; do not instantaneously jump to full quality on a busy link.
- This is congestion prioritization, not a hard audio latency guarantee:
  audio/video share the RDP transport and already-queued TCP bytes cannot be
  preempted. Keep queues bounded and test actual audio arrivals, not only QP.
- Protect both playback and microphone without changing their negotiated formats.
  Downstream video reduction alone is not proof of microphone-uplink protection:
  test asymmetric uplink pressure and camera coexistence separately. Do not
  advertise mic support in physical-console mode until its AUDIN/worker path is
  actually implemented (it currently explicitly rejects microphone requests).

## Integration and acceptance

1. Pure adaptive policy and tests for pressure, clear-link ceiling, chroma,
   quiet-link recovery and live on/off transitions.
2. Versioned control parser/capability/ack; apply per-connection in normal
   SessionController and preserve server-default/client-override semantics.
3. Physical console worker uses a shared AVC420 encoder currently fixed at80.
   Add bounded generation-scoped encoder-quality IPC and actual setVideoQuality
   application. Only the authenticated controller may change shared encoder
   policy; viewers cannot steal control or degrade the controller's quality.
   Carry policy through worker replacement/login handoff; reset on owner release.
4. Client saved setting, immediate dispatch, acknowledged status and error
   handling independent from layout/chroma. Settings changes must not retry
   unrelated unsupported camera or host-silencing requests.
5. Unit protocol/worker/client tests, full builds and actual Buzz GUI toggles
   against isolated Sol. Use a known audio source and controlled pressure to
   compare delivery gaps and video decisions. Confirm no reconnect, first audio
   remains intact, disabled behavior restores, and unsupported-server feedback.
   Cover playback-only, mic-only and duplex sessions; confirm voice onset and
   quiet speech are preserved. An absent audio direction must not prevent the
   other direction receiving priority. No-audio sessions keep normal steering.
6. Matching packages/docs after runtime acceptance. Do not deploy a cosmetic
   toggle before both encoder paths actually honor it.

Audio compression, silence suppression, and adaptive audio bitrate remain
separate follow-ups; this switch must not imply they have been implemented.

## Implementation state

The server now parses `{"type":"audio-priority","v":1,"id":"...",
"enabled":true}` and replies with matching type/version/id plus `ok`,
`effective` and `message`. ID must be a nonempty string of at most64 characters;
enabled must be a JSON boolean. This record leaves the initial layout gate
alone. It does not reopen audio or alter its consent/routing.

RdpConnection holds default/override state and derives effective priority from
playback OR microphone consent. VideoStream reads that state on each decision,
including temporarily steering when ordinary adaptation is off. Console broker
accepts changes only from the admitted controller, forwards its quality signals
to the generation-scoped worker, replays quality on worker readiness, and clears
priority/quality state when ownership changes. Console baseline stays80 when
priority is off. Resetting an explicitly fixed-quality stream restores its cap;
normal adaptive streams instead continue their gradual recovery.

Still pending: configuration-file plumbing for the default, capability discovery,
client persistence/live correlated dispatch and truthful UI, runtime congestion
and audio-delivery proof (including microphone), and packaging. The server reply
acknowledges policy acceptance, not a measured latency/quality improvement or
completion of an asynchronous encoder restart. No live deployment performed.
