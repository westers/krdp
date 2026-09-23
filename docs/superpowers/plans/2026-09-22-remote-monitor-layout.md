# Remote monitor layout implementation plan

Status: implementation in progress as of 2026-09-23. Product model agreed by
Steve on 2026-09-22; detailed defaults remain proposals in the
[design](../specs/2026-09-22-remote-monitor-layout-design.md).

This is a follow-up to OPT-044/OPT-041, not a replacement for the delivered
single-output virtual Fit task. Steve explicitly authorized implementation of
this goal after Fit was completed. The local source-level coordinate fix is
not native acceptance or deployment of this whole plan. See
`~/dev/rdp/evidence/2026-09-23-monitor-layout-diagnosis.md`.

## Phase 0 — Diagnose the missing KDE monitor behavior

- Identify the actual connection mode and compositor: physical Console, ordinary
  existing desktop, or independent virtual desktop. Record server/client versions.
- Snapshot outputs from inside that exact session, before/after adding a virtual
  monitor: stable identity, enabled state, native mode, logical position/extent,
  scale and primary. Compare against server records, capture surfaces and client
  views. Do not query a different user's/greeter's bus and call it the same desktop.
- In a disposable session, reproduce with a recognizable test window. Test KDE's
  move-to-screen operation, dragging across the expected seam and pointer clicks
  at monitor corners. Capture the UI and relevant layout records/logs.
- Produce a cause/evidence report. If a narrow defect is independent of this
  redesign, fix/test it separately without representing the whole plan as shipped.

Exit: the reported symptom has a reproducible cause, or a precisely documented
missing observation; do not assume a topology redesign itself diagnoses the bug.

## Phase 1 — Define separate models and compatibility

- Inventory the existing server layout authority/executor, `LayoutRecords`,
  client `Mapping`, `ScreenLayout`, `Views`, `LayoutFlow`, `AppLayout`, monitor UI
  and physical/virtual worker paths. Use current source, not historical class names.
- Specify remote output IDs and topology revision scoped to compositor lifetime.
  Separate the authoritative topology from a draft and from client view preferences.
- Specify coordinate transforms and compositor rounding. Preserve local screen
  identities without confusing them with remote IDs; handle missing local screens.
- Document capability negotiation, strict schemas, expected revision, request
  correlation, owner arbitration, limits and old-server behavior before wire edits.
- Define migration of old connection mappings: preserve local views, and do not
  replay old implicit Fit/privacy changes on a new Console connection. If old saved
  intent cannot be translated unambiguously, retain it as an unapplied draft.

Exit: deterministic pure-model tests cover ID changes, mixed scales, negative
origins, local hotplug and migration. Reading a saved view cannot mutate remote state.

## Phase 2 — Apply genuine remote topology changes

- Add/enumerate/position real outputs in the owning compositor. Default append
  uses the rightmost output's right edge and top, preserving existing primary.
- Implement validated arrangement transactions, topology revisions and readback.
  Reject conflicting ownership, stale drafts, unsupported operations and limits.
- Implement per-output capture and inverse input transforms for independent
  multi-output desktops; never rely on one oversized workspace encoder.
- Integrate safe capture/encoder transition handling from completed Fit work,
  including actual payload-dimension evidence, not only frame metadata.
- Define console lease cleanup versus retained topology persistence. Explicit
  output removal preserves reachable windows and never removes the last usable
  output. Local view closure leaves topology unchanged.
- Handle external KDE edits, output hotplug, SDDM-to-user handoff, cancellation,
  partial failure and disconnect without stale success or destructive restoration.

Exit: backend/protocol tests prove each supported operation, failure semantics,
authentication and unchanged physical behavior; real output readback is required.

## Phase 3 — Placement-aware Fit and matching

- Define an anchor and deterministic edge/alignment relations for managed outputs.
  Specify a canonical ordering, branching behavior, cycle rejection and collision
  reporting. Inherited free-positioned/pinned physical layouts are not auto-packed.
- Fit updates one remote mode/scale and proposes necessary dependent virtual
  position changes. Test both expansion and shrinking, scale-only changes and no-op.
- First virtual-desktop creation may initialize from selected client screens.
  Later reconnects use retained topology, even if client screen count differs.
- Implement explicit Match my client screens with preview. Add/move/resize are
  visible; removal needs explicit consent. Console physical changes are never implicit.

Exit: model/property tests show no newly introduced overlap or unintended gaps,
no physical movement without explicit approval, and no client-hotplug topology changes.

## Phase 4 — Clear client controls

- Split Remote monitors from Client views. Wire drag and keyboard arrangement,
  properties, add/remove, Fit, Match, primary and draft Apply/Cancel to the real paths.
- Show lifetime, owner, capability/limit reasons, pending status and useful errors.
- Keep Scaled and1:1 local. Distinguish Close view from Remove remote monitor.
- Test different client/remote monitor counts and reconnect preference restoration.
- Update help and any new server defaults in the separate fork KDE settings UI
  and packaging per OPT-043; don't add undocumented server-only knobs.

Exit: native GUI exercises the actual controls; screenshots and behavior are
reviewed for understandable state transitions, not just QML compilation.

## Phase 5 — End-to-end acceptance and release

| Scenario | Required evidence |
| --- | --- |
| Console connect from a differently arranged client | Host layout and apps unchanged; views usable. |
| Add virtual output to existing desktop | KDE enumerates it; test window moves to it by drag AND move-to-screen; existing primary/positions unchanged. |
| Position left/right/above/below, including negative origins | Compositor readback matches; pointer/clicks and cross-edge window movement work. |
| 100%,125%,150% mixed scales; differing resolutions | Logical adjacency correct, encoded sizes correct, text/view modes usable; no DPI-induced dead gap or pointer offset. |
| Fit growth/shrink and scale-only/no-op | Predictable reflow of managed virtual dependents; app contents retained; no silent physical movement. |
| Three remote outputs, one local screen; inverse arrangement | Switching/windows make every remote output reachable; no implicit remote deletion/duplication. |
| Local monitor unplug/replug | Client windows recover; remote topology and apps unchanged. |
| Detach and reconnect from another client | Same retained output IDs/layout/apps; local view preferences remain separate. |
| Explicit matching/removal | Preview matches actual changes; removal consent honored; windows remain reachable. |
| External KDE edit/concurrent viewer/control takeover | Stale drafts refused; owner/generation enforced; no overwrite of independent changes. |
| SDDM login/logout transition | Fresh layout discovery; no replay against stale greeter/user output IDs. |
| Failure during apply/capture and disconnect during change | Bounded error, no metadata-only success, safe input and conditional recovery. |
| Console release vs retained detach | Correct distinct lifetimes; physical outputs restored where owned, retained layout kept. |
| CPU-only and supported GPU encoding; large total desktop | Per-output/count/aggregate limits enforced; unsupported backend features honest. |

Run on disposable hosts/sessions with Buzz's actual Wayland GUI. Never use Hal's
protected desktop for destructive/logout tests. Record baseline and final physical
outputs/session processes and unchanged production client configuration. Preserve
uncertain failed records; no maintenance-guard expansion.

Before delivery: combined review, relevant server/client regression suites,
matching current server/client packages, dependency checks on target hosts,
release documentation and the normal user-run service-update workflow. Existing
single-output Fit passing does not satisfy this multi-output acceptance matrix.
Record unsupported backends as outstanding scope, not successful implementations.

## Decision checkpoints before implementation

The following proposed defaults must be visible in implementation review; if
evidence requires changing them, document the tradeoff rather than silently
changing the user model:

- Console-created virtual monitors are lease-temporary; retained desktop outputs
  persist across detach. Persistent console extras are not in the initial slice.
- Matching client screens is one-shot with removal confirmation, not auto-sync.
- Managed adjacency can reflow virtual dependents; physical/pinned outputs require
  explicit arrangement approval. Intentional gaps are preserved with warnings.
- Backend monitor-count/size limits come from verified capabilities, not UI guesses.

The plan itself grants no additional live-service or destructive-test authority.
Protect Hal's physical desktop and preserve the completed Fit behavior.
