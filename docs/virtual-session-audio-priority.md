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
PAM or prove adaptive behavior over a live congested network. No physical output,
live microphone capture, host restart, or deployment is part of this slice.
