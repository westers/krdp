# First virtual desktop layout from selected client screens

Status: design contract for OPT-044; pure planner `VirtualInitialLayout` exists,
but none of the request, journal, launcher, worker, or client UI path below is
implemented. Do not advertise this capability or claim a matching desktop.

## User-visible contract

Before creating a retained virtual desktop, the client shows its current
screens with an explicit selection, native resolution, scale, relative
arrangement, and primary. It asks the broker for a preview of the selected
remote layout. Create is a separate button that consumes that preview; merely
connecting, choosing a saved connection, or changing local screens never
creates or rearranges a remote output. Steve's default preselection choice
(all connected screens or only the current screen) is pending; the wire carries
an explicit selection either way. A client hotplug invalidates the preview.

For an older server, keep the existing four-field `virtual-session` v1
`create` request and fixed 1280×720 behavior visible as a legacy option. Do
not silently substitute it after a selected-layout preview fails. Retained
desktops keep their created layout on detach; reconnect ignores the new
client's screen count until an explicit Match action is previewed and applied.

## Broker boundary

Extend the v1 virtual-session command family with a separately advertised
`initialLayout` capability, an exact `preview-create` request, and a `create`
request carrying its one-use token. The preview request contains 1–16 selected
screen descriptors: a client-local selection ID, relative logical top-left,
native pixel size, exact scale, and exactly one primary. Client IDs are only
correlation keys; never use them as KWin output names or trusted paths.

`VirtualInitialLayout::plan` normalizes the minimum logical x/y, orders the
declared primary first, and uses `RemoteTopologyDraft` to reject invalid
modes/scales, overlaps, disconnected or corner-only layouts, and advertised
per-output/count/atlas limits. The broker replies with the canonical proposal
and a short-lived token bound to PAM UID, client connection, request ID,
selected descriptors, limits, and broker generation. Creating consumes the
token before any reentrant callback. Expiry, a changed selection, changed
limits, other owner, or reused ID with different content fails closed. An
accepted create response means an immutable launch intent exists, not that a
compositor or capture is ready; the existing list/attach state remains the
readiness authority.

## Durable launch boundary

The server persists only the canonical normalized output specification in a
new strict launch-journal record version, before spawning the independent
service. Keep reading existing v1 single-output records unchanged. A v2
reader checks exact fields, byte and output-count bounds, modes, scales,
primary count, normalized coordinates, and the same planner result; malformed
or mismatched records never launch. Root service argv is constructed from the
validated journal record, not from the RDP request, and carries no client
paths, arbitrary environment, or executable names. The layout is not secret,
but use bounded canonical encoding and the existing clean-environment/one-use
launch checks. Recovery and cleanup must compare the complete immutable
record; this feature does not authorize maintenance-guard expansion or an
uncertain failed-session relaunch.

## Worker and capture boundary

The private compositor creates the selected primary output first, then the
remaining outputs sequentially with the existing KWin settle interval. It
applies one complete KScreen arrangement and exact primary order, reads it
back, and starts one capture/encoder per output. It does not publish Ready or
accept input until every output has a decoded keyframe with the expected
native pixels and the full captured KScreen topology matches the committed
proposal. If KDE rounds a mode or geometry differently after creation, report
the observed difference as a bounded error and fail/clean up rather than
silently accepting a different layout. On failure, retire only these startup
creators and report Failed; never fall
back to a single output while reporting selected-layout success.

## Verification gate

Tests must cover old v1 journal/create compatibility; malformed/new journal
records; token replay, expiry and reentrancy; mixed 100/125/150% screens,
negative client origins and row offsets; one, two and three selected screens;
primary reordering, count/atlas limits, overlap/gaps/corner contact; KWin
creation and KScreen/capture disagreement; failed startup cleanup; and
detach/reconnect from a differently arranged client. Native Buzz GUI on a
disposable Sol desktop must prove windows move and clicks land on every new
output. No test may mutate Hal's protected physical session.
