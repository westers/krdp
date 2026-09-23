# Retained virtual-desktop Fit

Implementation in development; automated tests and native acceptance are
tracked separately. This is not yet a production deployment claim.

The own client's Fit action changes a private compositor's sole output to the
client screen's native pixels and scale, without replacing the compositor,
applications or output. Completed changes survive detach/reconnect. Physical
console mode restoration and ordinary layout mapping remain separate paths.

A correlated authenticated attachment advertises `virtualResize: true`.
Requests use `virtual-resize` version1, a1..64-character correlation ID,
integer even `width`/`height` and finite `scale`. Width320..4096,
height200..4096, scale1..4; scale rounds to1/120. Extra fields (including an
output selector) are refused. The broker resolves the current PAM owner's
attachment and sends generation-bound Resize IPC to its virtual worker.

Only an explicitly virtual-launched worker accepts the fixed internal selector
`virtual-desktop`. It reads its private compositor's KScreen snapshot, requires
exactly one connected/enabled normal-orientation output and preserves identity,
position and refresh. It reuses an advertised matching mode or adds a custom
mode using fixed `kscreen-doctor` arguments and discovers its fresh mode ID.
At64 advertised modes it refuses additional distinct modes; existing sizes
remain selectable. No mode removal or new display-management SDK is required.

Fit releases held input even for no-op requests, gates input and suppresses
both frame and output-metadata forwarding. Successful helper exit alone is not
completion: readback must match, the old encoder must fully tear down, and the
replacement must activate and produce a matching keyframe. The worker verifies
that candidate's self-contained H.264 SPS/PPS/IDR and software-decodes it with
strict error handling and bounded dimensions; decoded size must match both
readback and frame metadata. This check runs only on Fit/recovery candidates,
not ordinary video. Native acceptance also checks client-decoded picture size.
Timeouts are errors, never proof of rollback. Failed/cancelled changes use
conditional rollback without overwriting independently changed values; uncertain
capture remains gated until verified recovery. Completed geometry is never
rolled back merely because a client disconnects.

The client permits one pending Fit, correlates replies, clears pending state
before attachment/reset notifications and ignores stale replies. Unsupported
servers leave the action disabled. Errors stay out of unrelated layout flows.
Deploy matching broker, worker and KRdp library together; a newer broker with
an older worker can advertise the feature but the worker will refuse it.

Automated coverage: `VirtualResizeTest`, `VirtualResizeSessionTest`,
`VirtualSessionTransportTest`, client `VirtualSessionClientStateTest` and
`SessionModelTest`. Native acceptance uses Buzz's Wayland GUI and a new Sol
disposable desktop, including the actual Fit action, several sizes/fractional
scales, decoded frames, pointer coordinates, app retention, reconnect, media
continuity and explicit Stop. See the dated virtual-fit plan for the full gate.
