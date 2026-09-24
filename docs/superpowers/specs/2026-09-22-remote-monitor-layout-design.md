# Remote monitor layout and client views

Status: agreed product direction; implementation in progress, not delivered.
Source client now has a proportionally scaled visual retained-monitor arrangement
draft with drag, edge snapping, keyboard placement and multiple positions in one
revisioned Preview/Apply. One or more retained virtual-output position moves,
one new retained virtual output, or removal of one worker-added retained output
is supported in source by the authenticated preview/commit backend; X/Y fields
are an Advanced/test control. Batch moves have not passed native GUI acceptance.
Client source `23b2ba8` also offers explicit left/right/above/below placement
relative to another monitor. For an all-owned retained virtual arrangement,
a left/above draft that would use a negative origin is shown as the equivalent
nonnegative multi-output translation; it still requires Preview and Apply.
Physical or unowned peers are never translated by this editor. Native acceptance
of this normalization is still pending.
Retained multi-output Fit with managed dependent reflow, primary selection, and a
limited explicit Match planner now exist in source behind private capability gates;
they have unit/transaction tests but not the full native GUI acceptance matrix.
Match can map explicitly selected client screens onto retained owned outputs and
can conditionally include one new output; it does not remove unmatched outputs.
Console physical Preview/Apply and temporary worker-owned Add/Remove also exist
in source, but their mixed native GUI acceptance is still outstanding. Matching
server/client package installation and deployment are incomplete. A disposable 125%/100%
Sol/Buzz GUI run also accepted one visual position move with two independently
captured outputs; a later disposable GUI run added a third captured output by
actual Preview add/Apply clicks. A subsequent GUI run selected that third output,
previewed its removal, then committed while retaining the original two outputs.
See `~/dev/rdp/evidence/2026-09-23-retained-multi-output-sol.md`.
Recorded 2026-09-22 after Steve reported that adding a virtual monitor did not
allow moving KDE windows onto it. The cause is NOT diagnosed. Documentation
originally documentation-only. A source-level placement defect now has red-to-
green tests and a local geometry correction; no live deployment or runtime
acceptance of the complete topology feature is claimed. See
`~/dev/rdp/evidence/2026-09-23-monitor-layout-diagnosis.md`.

Related work: OPT-044 layout control, OPT-041 virtual outputs, OPT-018 per-output
capture, OPT-043 server settings. This extends those features; it does not
reopen their completed slices or replace the delivered single-output virtual
Fit work. Implementation plan: [remote-monitor-layout plan](../plans/2026-09-22-remote-monitor-layout.md).
Transaction contract: [remote monitor transactions](2026-09-23-remote-monitor-transactions.md)
(partially implemented for retained position moves, Add or owned Remove; other operations proposed).

## 1. The product model

Two independent layouts, joined by stable remote-monitor identities:

| Model | Authority | Meaning |
| --- | --- | --- |
| Remote monitor layout | The connected session's KDE/KWin compositor | Real outputs, native resolution, scale, logical position, enabled state and primary output; where applications and windows live. |
| Client views | This client | Which remote monitor each local window displays, local screen placement, fullscreen/windowed, Scaled or 1:1. |

Moving, resizing or closing a client view must not implicitly move, resize or
remove a remote output. A remote output may have no visible client view, one
view, or several views; closing its last view is not deleting the monitor.
One-screen clients can switch among multiple remote monitors. Showing one
monitor on two client screens duplicates its view, not its remote output.

Use unambiguous names:

- **Virtual monitor:** a real compositor output without a physical panel,
  belonging to the same KDE session as that session's applications.
- **Virtual desktop session:** an independent retained desktop/compositor,
  potentially containing several virtual monitors. It is not an extra monitor
  of the physical console and cannot share that console's application windows.
- **Client view:** a local window showing a remote monitor, not a compositor output.

## 2. Connection and lifetime behavior

### Console / an existing desktop

Inherit the actual current remote layout on connection. Different client screen
counts, scaling or placement must not rearrange the host. Treat an existing
session reached through SDDM the same way once login completes: rediscover the
new compositor and invalidate greeter identities and pending operations.

Adding a virtual monitor creates and enables an output in THAT compositor.
Default placement is directly to the right of the rightmost enabled remote
output, aligned with that output's top edge. This guarantees a shared edge,
unlike aligning to the top of an arbitrary bounding rectangle. Preserve the
existing primary and all existing output positions.

Proposed initial lifetime: client-created console outputs are temporary to the
controlling connection/layout lease. Label this explicitly. On release, remove
only outputs owned by that lease and reconcile any owned physical changes using
existing conditional restoration rules. Do not remove user-created outputs or
overwrite independent KDE changes. Persistent console extras are a separate
explicit future option, not an inferred consequence of reconnecting.

Removing an output must leave a usable surviving output and let KDE relocate
windows; confirm real window reachability in acceptance. Disconnecting a view
alone does none of this. Privacy/turning physical panels off is a separate,
explicit control, not a side effect of rearranging client views or adding an output.

### Independent retained virtual desktop

On FIRST creation, propose a remote layout matching the selected client screens:
their native resolutions, scales, primary and logical relative arrangement,
subject to advertised backend limits. Preview normalized differences. This is
new desired behavior, not a claim that today's single-output backend supports it.

Once created, remote topology belongs to that retained desktop. Preserve it,
its compositor and apps across detach/reconnect, including from a different
client. Persist client view preferences separately, per connection/client.
Retaining across a compositor exit/recreation is a separate lifecycle promise;
do not claim app/process persistence across logout or explicit Stop.

Source-only bootstrap planning now validates an explicit selected-screen set:
one primary, unique client-screen identities, even native modes, legal scales,
normalized relative logical positions, connected nonoverlapping seams and
backend count/per-output/atlas limits. It produces provisional Add operations
without treating a client screen name as a compositor output ID. The current
`virtual-session create` wire request still carries no screen selection and the
installed launcher still uses a fixed initial 1280×720 output; this planner
does not yet make first creation follow the proposal.
The request, immutable launch and native acceptance contract is in
[selected-screen virtual bootstrap](2026-09-23-selected-screen-virtual-bootstrap.md).

**Match my client screens** is an explicit one-shot remote-layout action, not
continuous synchronization. Show the proposed add/resize/move/remove changes.
Never silently delete outputs because the new client has fewer screens. Any
removal must be explicitly selected/confirmed and leave a usable output. For
Console, matching is also explicit and physical-output mutations require a
clear preview plus ownership; connection alone never invokes it.

## 3. Position, scale and coordinate rules

Remote placement uses compositor LOGICAL coordinates. Native capture dimensions
remain separate. For example, 1920x1080 at scale1.25 occupies1536x864 logical
units. A right-adjacent monitor starts at x1536, not x1920, if the first starts
at x0. Read back compositor-rounded geometry; do not accumulate independent
rounding errors or assume logical size times scale exactly recovers encoded size.

Keep explicit transforms between: client view coordinates, remote surface
pixels, per-output logical coordinates and compositor-global coordinates.
Negative origins and mixed scales are valid; normalize wire origins separately
without moving the host desktop. Do not multiply the entire desktop by one DPR.
Client physical-screen coordinates are presentation information, not remote
placement instructions unless a remote-layout action is explicitly requested.

Each remote output needs a stable session-scoped ID, separate from display name,
resolution, enumeration index, client screen ID and transient protocol handle.
Resolve aliases on authoritative readback; never bind a saved view to "the new
monitor at index1" after an unrelated output disappears.

### Arrangement and Fit

The layout editor snaps edges and supports left/right/above/below, offset rows,
and primary selection. New default layouts are non-overlapping and connected
by traversable edges (corner-only contact is not enough). Preserve inherited
unusual layouts on connect; explain gaps or mirroring instead of silently fixing
them. New edits reject accidental overlaps; intentional gaps require an explicit
warning/confirmation. Mirroring is a separate mode and not part of this first slice.

Fit changes the selected remote output's native resolution and scale to the
selected client screen. Scaled and1:1 remain local presentation choices.
For newly managed arrangements, store deterministic edge/alignment relationships
so resizing a monitor can keep adjacent virtual outputs adjacent. Keep the
selected output anchored; reflow only dependent, managed outputs. Show all
affected positions in the preview. Do not silently repack the whole desktop,
move physical/pinned outputs, or erase deliberate gaps. If constraints conflict,
require an explicit revised arrangement or refuse without partial success.
The implementation must specify tie-breaking and test chains, branches and cycles.

Fit does not change existing lifetime semantics: retained virtual geometry stays
after detach; physical-console changes follow the existing temporary ownership
and restoration contract. This plan must not silently rewrite current Fit code.

Source-only planning slice (not a live Fit implementation): managed adjacency
is represented as a directed parent→child edge with one parent per movable
virtual output, a side (left/right/above/below), and a logical offset along
that edge. Fit keeps the selected output's top-left fixed, then visits its
dependents in output-ID order and moves only those managed virtual outputs.
Unrelated/pinned outputs and intentional gaps remain where KDE put them. A
cycle, stale relation after an external edit, ownership conflict, collision,
or relation that cannot be kept while holding the selected origin returns an
error for preview instead of repacking or partly applying. The resulting
operation list still requires authenticated revisioned transaction, compositor
readback and verified capture before any capability can be advertised.

## 4. User interface

Separate sections, not one diagram combining unrelated coordinate systems:

1. **Remote monitors — where KDE windows live.** Arrangement rectangles with
   stable labels, physical/virtual badges, resolution, scale, primary, enabled
   state and temporary/retained lifetime. Add virtual monitor; arrange; Fit;
   Match my client screens; Apply/Cancel. Provide keyboard-accessible placement
   controls as well as drag-and-drop. Draft changes are distinguishable from
   applied state and readback failures are visible.
2. **Client views — where I see them.** Show monitor on a local screen/window,
   switch remote monitor, fullscreen, Scaled,1:1 and close view. Local hotplug
   moves stranded client windows to an available screen without deleting remote
   outputs or changing their layout.

Closing a view and removing a remote output must be visibly different actions.
Unsupported actions are disabled with the actual reason. Do not show successful
remote layout changes just because a client window or video surface exists.

## 5. Server and protocol requirements

Reuse existing KRDPCTL ownership and layout machinery where appropriate. Audit
the present coupled `Mapping::plan()` behavior before extending it: current
client models already mix remote Fit/creation and local presentation, so this
is a semantic separation, not just relabeling a dialog.

Advertise capabilities per session/backend (enumerate, add, remove, position,
resize, scale, primary, multi-output capture), with limits and lifetime semantics.
Old servers remain viewable; unsupported writes must not degrade into a different
operation. Do not infer independent-session isolation from output names.

Remote changes use an authenticated owner, exact compositor/session generation,
correlated request, expected layout revision and one serialized transaction.
Reject stale drafts and refresh after external KDE changes. View-only clients
cannot mutate layout. Account for concurrent clients and physical-console takeover.

Release held input before changes; prevent stale-coordinate input and stale-size
frames while applying. Success requires compositor readback plus working capture
with independently verified encoded geometry for each affected output. Failures
must surface bounded errors and conditionally reconcile owned changes, never
overwrite unrelated desktop changes or acknowledge metadata-only success.

Multiple virtual monitors require per-output capture/encoding and correct input
mapping. Do not squeeze a wide desktop into a workspace encoder past its limits.
Advertise actual per-output/count/aggregate/backend limits, including CPU-only
and GPU paths; unknown support is unavailable, not an optimistic default.

## 6. Investigation and acceptance boundary

First diagnose Steve's report: compare actual compositor identity/output geometry,
KDE's screen enumeration, client remote-monitor records and local views. Determine
whether the output is missing, disabled, overlapped, misplaced, in another session,
or correctly present with broken window/input mapping. These are hypotheses,
not established causes. Window movement by drag and by KDE's move-to-screen action
must work on the final implementation, alongside valid pointer coordinates.

The implementation plan contains the scenario matrix and delivery gates. No
output-changing tests on Hal's protected session. Use disposable test desktops
and native Buzz GUI acceptance, preserving physical sessions and saved settings.
No raw USB, conferencing changes, maintenance-guard work or automatic failed-session
cleanup belongs to this feature.
