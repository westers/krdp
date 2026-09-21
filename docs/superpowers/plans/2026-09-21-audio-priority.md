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
- Effective only when audio playback is enabled. Under existing measured RTT
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
6. Matching packages/docs after runtime acceptance. Do not deploy a cosmetic
   toggle before both encoder paths actually honor it.

Audio compression, silence suppression, and adaptive audio bitrate remain
separate follow-ups; this switch must not imply they have been implemented.
