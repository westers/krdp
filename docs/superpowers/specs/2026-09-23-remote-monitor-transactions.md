# Remote monitor transaction protocol (implementation contract)

Status: partially implemented in source, 2026-09-23. Retained multi-output
`topology-query`/`topology` and one owned-virtual-output `move`, `add`, or `remove` preview/commit
are wired; position, capacity-gated Add and owned-output Remove are advertised only for published multi-output
retained desktops, and accepted through disposable Sol :3396 → Buzz source GUI
(`d958e6d` server, `cf6bae8` client). The native Monitors dialog performed
Preview/Apply clicks and observed revision 1→2 with stable output IDs and
KScreen/captured-keyframe proof. No installed server/client or production path
is claimed. Console native acceptance, resize/scale/primary, Fit/Match and full
runtime acceptance remain absent. A visual arrangement is now the client UI's
main draft control; X/Y remains under Advanced.
Source WIP now includes a separate read-only Console `topology-query`: the
authenticated physical worker runs `kscreen-doctor -j` inside its session on
an encoded keyframe, requires exact capture/monitor geometry and decoded
payload dimensions to agree with KScreen's native mode/scale/position/primary,
and sends a bounded inventory over worker-wire v3. The broker assigns
generation-scoped IDs/revisions and answers an admitted Console query only
after a fresh worker result; the client accepts `lifetime=lease` only on a
physical Console attachment and rejects lease write capabilities. All Console
topology writes are false. This is source/build/unit-tested only, NOT native
Sol/Buzz GUI-accepted or deployed. KScreen arrangements with disabled, rotated
or mirrored outputs currently fail closed rather than yielding a partial
inventory. The older Console `layout` and `console-resize` paths remain
separate; no physical mutation is authorized by this topology query.
An additional disposable 125%/100% Sol/Buzz source-GUI run confirmed that the
visual editor uses logical aspect/position and committed one virtual move from
`(1024,100)` to `(1024,209)` with revision 1→2 and unchanged IDs. This does
not advertise any additional operation.
A later Sol-only compositor probe first proved creator lifetime; subsequent
`ad1b58a`/`f79f2d0` worker and `6e06fbc` broker Add code requires authenticated
generation/revision, unchanged existing outputs, fresh KScreen readback and
independently decoded three-output keyframes. Source GUI `f43a1e1` actual
Preview add/Apply clicks on disposable Sol/Buzz accepted a third 1280×720
output at `(2560,0)`, revision1→2, stable new ID `o-3` (`67487f9` capability).
`83dbcec`/`3dd59ec` proved worker-only owned Remove on Sol. Broker `2b7260e`
then bound Remove to an authenticated one-use preview token, rejects original
outputs and requires exact surviving catalog entries with one revision bump.
Client `afbb4fd` adds selection-based Preview remove and Apply. Disposable
Sol :3396 → Buzz source GUI clicked the added third rectangle, previewed only
the two originals, then applied Remove with topology revision 2→3 and unchanged
`o-1|o-2`; the worker independently KScreen-confirmed and recaptured both
survivors before its acknowledgment. Source-only; see the evidence report.
This fills in phase 1 of the
[remote monitor layout plan](../plans/2026-09-22-remote-monitor-layout.md).
The existing `KRDPCTL` v1 `apply` remains for older clients. A new editor must
not use that coupled request as if it provided revisions or independent views.

## Discovery and compatibility

Keep the v1 frame envelope. After normal `attach`/retained-session attachment,
send `topology-query` with a bounded correlation `id`. A capable server answers
`topology` with the same `id`, or an explicit `unsupported` error. Old servers
answer unknown-record `unsupported`; the client keeps video and local views
working and disables topology writes. Do not send a new version as the first
record: an old server would fall back to configured MonitorMode instead of
attaching to the intended existing compositor.

Initial read-only server slice: the retained virtual broker accepts an exact v1
`topology-query` with a 1–64 character correlation ID **after attachment**,
requests worker keyframes and answers only when a frame matches the
worker-reported output inventory and its generation-scoped catalog. For a
multi-output retained desktop, the worker captures/encodes each QScreen
separately and gates publication until every output has an H.264 keyframe
whose decoded dimensions match the reported pixels and logical scale. Five seconds
without such a frame returns a correlated `topology-error`/`timeout`; detach
cancels the query. It initially advertised enumeration only, with all topology-write
capabilities false; `multiOutputCapture` is true only for a currently published
multi-output capture. In source `b413c9e`, before first publication the worker
also queries `kscreen-doctor -j` inside its verified private compositor and
requires independently reported mode, scale, logical geometry and primary to
match every decoded captured keyframe. A mismatch fails the worker closed. The
published geometry remains worker/QScreen metadata after this KScreen check;
a later query does not itself request a new KScreen readback. The later
position-commit path does require fresh readback and recapture. This multi-output path has source tests
and isolated Sol/Buzz two-surface GUI acceptance; the source client also received
a read-only two-output topology reply (`ready/2/rev1`) in an isolated run. A
same-compositor Sol worker probe moved a KDE window to the second virtual output
and captured it there. Source `ea89fc7`/`b1f8698` also maps click/wheel
packets in retained multi-output mode; a bounded Sol private-compositor
click-only probe placed the pointer at the expected mixed-scale logical
coordinate. A later private Sol worker-endpoint pointer drag also moved a
marked Konsole across the seam to Virtual-1 and decoded a fresh destination
keyframe (`e982f21`/`0802ee6` probe). GUI/RDP-driven window drag, application click effect,
installed broker and deployment remain unaccepted; a later mixed-scale visual position write passed isolated RDP acceptance. Other
backends besides the source Console read-only path have no new query handler yet. The readback parser accepts the saved
isolated Sol 125%/100% two-output KScreen fixture; a subsequent bounded Sol
worker probe logged that the independent two-output KScreen readback matched
both decoded captures before publishing and stopped cleanly. Its shell-free
logical position arguments remain source-only. No topology-write capability
follows from this publication gate.
The client sends the query only after an acknowledged retained attachment,
validates its correlation and bounded geometry/capabilities, and treats an old
broker's generic unsupported error as query fallback without failing the
legacy layout flow. It exposes `remoteTopology` state in a separate KDE monitor
inventory in the Monitors dialog. Only the position editor is enabled when the
backend advertises `position`; unrelated writes remain disabled.

The authoritative `topology` record carries:

- `generation`: an opaque identifier for the actual compositor/session lifetime,
  not the RDP connection or monitor count. On SDDM-to-user transition, logout,
  worker replacement or compositor recreation it changes and invalidates every
  draft, preview and output ID from the old generation.
- `revision`: monotonically increasing integer scoped to that generation. It
  changes on *observed* output topology/state changes, including independent
  KDE edits and backend readback corrections. An unchanged query does not bump
  it. No floating-point JSON value outside exact integer range is accepted.
- `outputs`: IDs stable within the generation and never reused within it;
  connector/display name is a separate field. Each has native pixel mode,
  compositor logical origin, read-back logical extent, scale, enabled/primary,
  physical/virtual kind, owner/lease and lifetime. Do not derive IDs from
  enumeration order or the old reusable `virtual-<n>` label.
- `capabilities`: independently advertised enumerate/add/remove/position/
  resize/scale/primary/multi-output-capture, output count and dimension limits,
  aggregate RDP pixel-atlas limit, supported scales/modes, and temporary versus
  retained lifetime. A backend advertises a write only after its actual
  readback/capture path has been verified; unknown means unavailable.

The existing `layout` record is an old-protocol compatibility view, not the
new revisioned topology. Old saved mappings are read for client view intent;
their Fit, privacy and virtual-output intent remains an unapplied draft on a
new Console connection. A draft containing an ambiguous old output reference
cannot be silently migrated to a different ID.

## Draft, preview and commit

Client views are persisted separately by remote output ID and local screen ID.
Changing a view never creates a remote operation. A remote draft starts from
one authoritative `(generation, revision)` and contains explicit operations:
add virtual; remove an owned virtual; move in logical coordinates; resize/
scale a supported virtual; choose primary; and, where a backend explicitly
supports it, an approved physical-output change. Each operation identifies
the exact output ID or a request-local temporary ID for a proposed new output.
No operation means no remote write. Never infer a remove from missing local
screens or closed views.

`topology-preview` includes `id`, `generation`, `expectedRevision`, operations
and any explicit `allowRemoval`/`allowPhysicalChange` confirmations. The server
authenticates the current controller, checks backend capabilities and exact
generation/revision, validates the entire proposed topology (logical overlap,
traversable edges for new arrangements, primary/surviving output, output and
RDP atlas limits), and returns a correlated preview with a short-lived opaque
token, before/after records and warnings. The preview itself changes nothing.
The UI shows all consequences, including managed-dependent Fit moves. Removal
and physical changes require an explicit confirmation in the preview and an
identical commitment; they cannot be smuggled in as a side effect of Match.

The v1 preview request's exact envelope is `type="topology-preview"`, `v=1`,
`id`, `generation`, `expectedRevision`, `allowRemoval`,
`allowPhysicalChange`, and `operations` (1–16 items). Every operation has
`op` and `output`: `add` uses a unique `new:<id>` and requires `position`
`{x,y}`, `pixels` `{width,height}`, and `scale`; `move` requires `position`;
`resize` requires `pixels` and `scale`; `remove` and `primary` have no extra
fields. Coordinates are integral KWin logical positions. Unknown fields and
client-supplied `owner` fail parsing. Server source parses this strict shape
(`RemoteTopologyProtocol.h`). In source `8f51fb0` plus the subsequent broker
slice, a retained multi-output owner can preview and commit **one move of an
owned virtual output**; the preview token is short-lived and one-use, and
commit success requires fresh private KScreen checks, republished inventory
and both decoded keyframes agreeing with the entire previewed after-state.
Other operations return `unsupported`. The query now advertises `position=true`
for a published multi-output retained desktop after disposable RDP and GUI
Preview/Apply acceptance. This is not an installed remote arrangement release.

`topology-commit` includes the preview token, the same `id` and expected
generation/revision. Only one commit per compositor may execute at a time.
The v1 commit envelope has exactly `type="topology-commit"`, `v=1`, `id`,
`token`, `generation`, and `expectedRevision`; a successful source response is
`topology-result` with the same `id`, `ok=true`, and a nested authoritative
`topology` record. No-op moves retain the revision; observed changes must bump
it exactly once. An altered complete after-state returns `partial`, not a
target-only success.
The server rechecks owner, generation, revision, capabilities and all state
against fresh readback before any mutation; no queued stale draft is rebased
silently. It releases held input and gates stale video/input during the
transition. `topology-result` echoes the id and either an error or an
authoritative post-commit topology. Success requires compositor readback and
working per-output capture with verified encoded payload geometry; a
metadata-only update is not success. A bounded failure reports which changes
landed, conditionally reconciles only its own lease-owned changes and refreshes
the authoritative state without overwriting independent KDE edits.

The client must distinguish `stale-generation`, `stale-revision`, `not-owner`,
`unsupported`, `invalid`, `capture-failed`, and `partial` outcomes; every
terminal result is correlated. An unexpected disconnect cancels in-flight
work when possible and follows each backend's lifetime rule: Console extras
owned by the lease are removed conditionally, retained virtual outputs persist
across detach, and unrelated user-created outputs survive.

## Coordinate and identity invariants

All operation positions and read-back topology positions/extents are KWin
logical. Native capture dimensions are independent. The RDP pixel atlas is
computed separately and can have different origins from the compositor; input
and cursor map through the selected output's explicit logical origin and
per-output scale. Negative origins, offset rows, mixed scales, and a single
local screen viewing several remote outputs are normal. Read-back compositor
rounding wins over client arithmetic. The old `virtual-<n>` ID is a compatibility
alias only and must never be treated as a new stable output ID.

The internal worker `Outputs` record carries an explicit compositor-global
workspace origin (worker-wire v2). Its monitor rectangles remain normalized
for RDP capture/input, while the broker adds the origin back when publishing
`topology.logical`. The separate KScreen publication check also verifies this
origin. A zero origin cannot be inferred merely because the RDP atlas starts
at zero; doing so loses valid negative and shifted KWin arrangements.
In the isolated Sol KWin test, a negative-position `kscreen-doctor` request
was accepted but read back as an equivalent layout translated to origin
`(0,0)`. This is an observed backend normalization, not evidence that literal
negative origins are supported there. A commit must return the actual fresh
readback and never report the requested coordinates as if they landed.

No claim of support is made for retained multi-output desktops until the
broker/worker transports multiple independent capture streams and per-output
input mapping in one compositor. No client UI should display an enabled
operation merely because the legacy Console executor could once create a
virtual output through `apply`.
