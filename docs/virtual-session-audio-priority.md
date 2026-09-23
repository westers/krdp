# Virtual desktop audio priority

An authenticated owner attached to the exact ready virtual desktop may change
`audio-priority` during the connection. The existing protocol requires version 1,
a nonempty request ID of at most 64 characters, and a boolean `enabled`. Replies
echo the ID and report `ok` and current `effective` state. Effective priority
requires the preference plus playback OR a ready, consented microphone. Pending
microphone startup alone does not enable it. Priority does not enable audio.

Only a successfully bound, authorized Transport attach reply advertises the
optional boolean `audioPriority: true`. Control-level or refused attaches do not.
Old servers may omit it; clients must not infer capability from attachment alone.

Virtual video keeps its existing quality cap of 80 and ordinary network adaptation
disabled. Audio priority uses VideoStream's existing temporary steering algorithm.
Requested qualities cross to the worker through the existing VideoQuality wire
record. Each queued signal connection captures its binding's control generation;
delivery rechecks that generation and current PAM-authenticated ownership. Old
queued changes cannot be relabeled with a new attachment's generation. The worker
also checks its active control generation. Quality is bounded to the fixed cap.

Priority-off and the last effective audio direction switching off restore both
worker and local quality 80 immediately, including microphone source failure.
No further priority request or video frame is required. Microphone startup/failure
does not reset quality while playback still keeps priority effective.
Queued reductions are neutralized when neither audio direction is effective.
Revoke clears the preference/default,
restores the old worker's quality under its old generation, and resets local
quality. A new binding starts at 80 and requires fresh preference/consent.

Tests use private identity seams and authenticated fake-worker sockets for wire
quality, policy changes, and stale-generation rejection. They do not authenticate
PAM or prove adaptive behavior over a live congested network. Live acceptance
is recorded separately below.

## Live acceptance (2026-09-22)

Native Buzz client075b7d9 against Sol server4e2a83d passed playback-only and
microphone-only priority toggles, same-desktop reconnect, synthetic microphone
PCM capture, detach source removal and clean desktop stop. Bounded frame-ack
delay triggered broker quality requests, but revealed that the CPU libx264
backend ignored the generic global_quality field.

Private KPipeWire183a140 (exported patch0024) fixes that backend by updating
private CRF before the next encoded frame, preserving the existing mapping and
default. Failed updates retain the previous working quality and permit an
identical later request to retry. Producer quality snapshots are synchronized
with setters and rollback. Nine headless CTests pass, including actual encoded
packet reduction/recovery and eight encoder Qt cases; the display-dependent
media monitor test was excluded.

After installation only in Sol's isolated virtual prefix, a fresh disposable
desktop's worker mapped the verified new library. Native GUI acceptance with
playback consent and5000ms injected frame-ack delay confirmed worker CRF changes
17→34→52→69→78; priority-off restored17 without disconnect. Baseline and recovery
screenshots were visually checked. The lowest rungs may saturate to the same
encoder limit. This is live CRF steering under synthetic frame backlog, not
proof of acoustic latency or asymmetric network performance.

An initial test deliberately restarted its detached capture worker solely to
enable logging. The existing fail-closed path marked that desktop unavailable;
normal authenticated Stop retired that known-owned fixture cleanly. The passing
test seeded logging before its worker started instead. Automatic capture-worker
recovery is not implemented by this change; maintenance work remains parked.

Both new disposable desktops stopped successfully. The three historical failed
records were preserved. Physical-console service, physical session and Hal were
unchanged. No new audio recording or camera/browser-conference test was made by
the CPU-fix acceptance; prior media/host-silence recordings remain separate.
Evidence: `~/dev/rdp/evidence/virtual-x264-live/RESULT.md` and
`~/dev/rdp/evidence/virtual-audio-priority-live/RESULT.md`.
