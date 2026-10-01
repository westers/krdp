# Farside remaining work: implementation and acceptance plan

Date: 2026-09-30. Status: execution in progress; no consolidation cutover accepted.
Requested by Steve after the Sol/Hal `f255ee2` deployment.

## Outcome and scope

Farside offers two connection types: **Console**, the existing desktop including sign-in and lock screens, and **Virtual**, a separate retained desktop. Complete the authentication fix, preserve the useful features of the old per-user server in those two types, migrate configuration and saved connections, then remove the redundant implementation. Finish the remaining camera, NVIDIA, reliability, performance and client UX work as separately accepted tasks.

Each task below specifies instructions, completion criteria and evidence. The
execution ledger records current status; a source acceptance is distinguished
from T27 package delivery. Task IDs are local to this plan. Existing `OPT-###`
entries in `research.md` remain the roadmap identifiers.

## Goal execution contract (Steve-approved revision, 2026-09-30)

Implement only T01–T28 in this plan. Record newly discovered out-of-scope
defects in `deferred-issues.md` for Steve's review without fixing them. Persist
through errors and continue independent work while waiting for feedback.

**Next delivery milestone:** Complete P0 (T01–T13), including the applicable
T27 package/deployment and T28 documentation gates, before expanding NVIDIA
support or later UX work. Existing accepted P1 work remains valid. P0 is
delivered only when its task acceptance, migration, rollback and fleet gates
are satisfied; a source checkpoint or a build does not close it.

**Feedback requirement:** A task may be reported as requiring Steve's feedback
only after its independent implementation and agent-run verification are
finished. Record the task ID, exact remaining test or decision, host/session
type, source/package version, steps or decision options, pass criteria,
evidence already collected, and what the answer will unblock. Ask for that
specific result and continue other available work. An unfinished implementation,
unattempted agent-run test, unavailable environment or dependency alone does
not qualify as requiring feedback; where Steve must provide hardware or access,
identify the exact provision needed.

**Goal stopping condition:** Every T01–T28 task is either accepted with its
required evidence or has only a documented, specific Steve-owned test or
decision remaining, and no independent work remains. Feedback-dependent tasks
stay `PARTIAL`; reaching this stopping condition does not declare those tasks,
P0 or the full plan delivered. Full delivery still requires the task and
milestone criteria below. Keep implementation, source acceptance and installed
delivery distinct in the ledger.

## Execution checkpoint (2026-09-30)

- Later Console feedback: Steve requested Sol session 429 unlock; verified and
  completed (`LockedHint=no`). Historical emergency-mode locker and Discover
  faults share the Fontconfig/Qt fallback stack. A standalone offscreen probe
  fails with Sol's old user font cache and passes with a clean cache. The old
  cache was backed up and regenerated; current Chrome's isolated launch creates
  no incompatible version links, and the greeter survives a bounded offscreen
  test. Steve subsequently confirms both normal lock-screen and Discover
  password entry work on Sol Console. Two successful authorization grants are
  logged with the same agent PID158549/NRestarts0. Completed PackageKit
  operations, the full Console/Virtual failure/restart matrix, original agent
  stack and output-release race remain open; T01/T02/T03 remain PARTIAL.
- T14 retained Virtual hardware HEVC motion now passes functionally on Buzz/Sol
  for about a minute, with changing snapshots, hardware VAAPI decoding and
  NVENC worker/backend reports. No resize/two-screen/failure or quiet-performance
  claim; T14 remains PARTIAL. See the ledger's latest evidence references.
- T04 shared authenticated-user preferences and immutable root authentication
  scopes are implemented in source. Console PAM/alias video and both brokers'
  allowed/denied authentication gates pass on Sol/Buzz. The Virtual auth fixture
  intentionally launches no desktop; T08 privileged editing and T10 actual
  credential/retained-desktop migration remain. See shared settings coverage.
- T09 explicit client types and T19 health explanations are accepted in client
  `e247d54`, released as 0.6.3 (`2df38a7`) to all six hosts. Focused tests and
  Buzz-to-Sol default/custom-port and packaged HEVC/type gates passed. Existing
  saved routes are not cut over. Sol client rollback/return passed; Hal was last.
- T01's bounded debugger window captured only an idle stack. The debugger has
  detached; the original authentication crash is still undiagnosed.
- T21's dialog and strict certificate/cache product fixes passed and ship in
  0.6.3. The remaining Hal preload warning comes from old running Codex parents,
  despite clean current startup files/manager environment; voluntary Codex
  relaunch is a user-owned cleanup gate. It remains recorded as PARTIAL.
- All installed clients are now 0.6.3; servers remain the starting baseline
  below. No new server preferences may ship before matching KCM acceptance.

Live task status and exact evidence:
`.superpowers/sdd/2026-09-30-farside-remaining-work/progress.md`.

## Verified starting point

| Item | Current state |
|---|---|
| Sol and Hal server | Held `f255ee2`, package `6.6.80+git202609301221.f255ee2-1`, private KPipeWire `9d6b08c` |
| Sol routes | Per-user :3389, Console :3391, Virtual :3395 active; retained desktop preserved |
| Hal routes | Existing per-user :3389 active; Console and Virtual inactive |
| Other servers | Buzz, ace, cray, marvin on `4b308bd` |
| Clients | Sol/Buzz/Hal 0.6.2; ace/cray/marvin 0.6.0 |
| Implemented parity | Broker quality/adaptive quality, audio-priority defaults, standard-client media and camera loopback options; currently host-wide service settings |
| Camera | Both Sol session types previously delivered remote JPEG samples and 150 consumer frames, then stopped on consumer exit; remaining human/application gates below |
| NVIDIA | HEVC NVENC on Sol Console verified; HEVC NVDEC verified in earlier Sol test and Steve's Hal session; device selection and broader acceptance remain |
| Latest deployment gate | Four focused suites passed; Buzz received user AVC444v2, Console hardware HEVC, and retained Virtual AVC420 video |
| Open authentication fault | Sol KDE PolicyKit agent SIGSEGV after requesting a password; runtime `KDE_DEBUG=1` drop-in awaits reproduction |

Deployment evidence: [Sol/Hal rollout](../../../../rdp/evidence/2026-09-30-parity-deploy/SUMMARY.md). That evidence proves functional connections, not unloaded performance or completion of the two-type migration.

## Instructions applying to every task

1. Read `~/dev/rdp/CLAUDE.md` and `HANDOFF-CODEX.md` §7, §2 and newest §8 before execution. Their current snapshots supersede historical release notes. Preserve unrelated working-tree edits and source tarballs; do not edit `~/.claude/`.
2. Use Sol as the live server and Buzz as the live client. Use ace/cray for Intel/AMD acceptance when appropriate. Hal is Steve's work desktop: no agent live connections, windows, PipeWire/compositor/daemon tests or output-changing tests there. Daemon-starting suites run on Sol. Pure unit tests and isolated builds are allowed on Hal.
3. Hal deployment is authorized after a gate elsewhere: simulate apt, preserve holds, check zero incoming established RDP connections immediately before stopping services, deploy Hal last, and preserve work sessions, settings, certificates and saved profiles. Agents do not write Hal's stock `~/.config/krdpserverrc` or use `kwriteconfig6 --notify`. Steve performs Hal's live visual/GPU checks.
4. Credentials stay in `~/dev/rdp/.env` or the existing secret store. Pass credentials through stdin/environment; never place them in argv, evidence, screenshots, commits or plans. Do not bypass PolicyKit or disable authentication to make a test pass.
5. Standard RDP channels carry standard functionality; private control records cover extensions after authenticated capability negotiation. Do not build compatibility layers for old Farside protocol versions. One-time saved-data migration is required and distinct from protocol compatibility.
6. Every user-facing server setting ships with its matching KCM binding, validation, help, defaults, scope and live/restart behavior. Unsupported features are visibly unavailable. New GPU controls also require matching client UI and actual-backend reporting.
7. Keep Console topology experiments off by default. Implement the legacy feature parity Steve requested; unrelated expansion of the frozen topology experiments is outside this plan. Record any necessary change to that boundary before implementation.
8. Run focused tests appropriate to the change, then its listed acceptance checks. Preserve logs, package/commit hashes, environment and observed limitations under `~/dev/rdp/evidence/<date>-<task>/`. A build or a mock UI alone does not satisfy a live behavior requirement.
9. Mark each task `PLANNED`, `IN PROGRESS`, `PARTIAL`, or `DONE <date>` in this plan's execution ledger. `PARTIAL` must identify the missing gate, dependency or hardware. An unavailable test environment does not count as a pass.
10. Record completed work in the handoff Log and relevant `research.md` entry. Commit only intended files. Push server changes to `github`, client changes to `origin`; export KPipeWire patches without pushing that repository. Follow the existing client version/tag/package procedure for client releases.
11. Steve's execution scope is only T01–T28. Record newly discovered bugs outside this scope in the ledger's `deferred-issues.md` for his later review; do not fix them in this goal. If a task requires his feedback, name the specific decision or acceptance evidence needed and continue independent tasks.

## Order and dependency register

P0 completes the immediate authentication and two-type consolidation request. P1 finishes GPU support and observed reliability/diagnostic issues. P2 completes the later performance and UX work. Delivery and documentation apply to each accepted release, not only the final milestone.

| ID | Task | Priority / tracker | Required predecessors for completion |
|---|---|---|---|
| T01 | Reproduce and diagnose PolicyKit crash | P0 | None |
| T02 | Fix Discover authorization | P0 | T01 |
| T03 | Fix lock/teardown race | P0 / OPT-049 | Reproduction on a non-Hal host |
| T04 | Shared configuration and authentication scopes | P0 / OPT-053 | Inventory below |
| T05 | Complete video policy parity | P0 / OPT-053 | T04 |
| T06 | Complete monitor/layout parity | P0 / OPT-053 | T04; T03 for destructive lifecycle gates |
| T07 | Preserve display wake/inhibition | P0 / OPT-053 | T04 |
| T08 | KCM scope and coexistence | P0 / OPT-043, OPT-053 | T04–T07 |
| T09 | Explicit client Console/Virtual types | P0 / OPT-053 | T04 contracts |
| T10 | Migrate settings, profiles and credentials | P0 / OPT-053 | T04–T09, T11, T12 before fleet cutover |
| T11 | Camera and media acceptance | P0 / OPT-050 | Current camera implementation; fixes found during acceptance |
| T12 | Standard-client acceptance | P0 | T04–T08, T11 |
| T13 | Retire redundant per-user route and code | P0 / OPT-053 | T02–T12 |
| T14 | Complete NVIDIA HEVC acceptance | P1 / OPT-046 | Current HEVC implementation |
| T15 | Stable GPU inventory and actual telemetry | P1 / OPT-046 | T14 observations |
| T16 | GPU selection and fallback UI/policy | P1 / OPT-046 | T04, T08, T15 |
| T17 | NVIDIA AVC420 encode/decode | P1 / OPT-046 | T15, T16 |
| T18 | Supported-GPU NVIDIA AV1 encode/decode | P1 / OPT-046 | T15, T16; eligible non-Hal hardware or Steve's Hal acceptance |
| T19 | Accurate connection-health explanation | P1 / OPT-051 | Recorded health traces |
| T20 | Diagnose and fix Sol DrKonqi loop | P1 | Reproducible process/crash evidence |
| T21 | Fix session-dialog and startup warnings | P1 | T09 where type-dependent |
| T22 | Independent decode work per screen | P2 | T15 telemetry; T26 baseline |
| T23 | HEVC quality/bitrate tuning | P2 | T14; T26 baseline |
| T24 | Sessions page | P2 | T09; current retained-session ownership contract |
| T25 | Remaining client UX redesign | P2 | T09, T10, T24 |
| T26 | Load-aware performance and remaining visual gates | P1/P2 validation | Baseline first; repeat only affected cases after changes |
| T27 | Package and deploy accepted releases to fleet | Delivery | Accepted tasks included in each release |
| T28 | Reconcile documentation and close milestones | Delivery | Evidence from each completed task |

Start T01 and develop the independent parity work while awaiting a user-triggered authentication reproduction. Do not wait on GPU expansion or the later UX redesign to finish P0. Migration and legacy removal remain sequential gates even if their implementation is prepared earlier.

## P0: authorization and Console/Virtual consolidation

### T01 — Reproduce and diagnose the PolicyKit crash

**Instructions:** On Sol, capture package versions, service logs, core-dump limits and the active KDE agent's stack. Use the existing runtime `KDE_DEBUG=1` drop-in; verify it actually permits a useful core. Reproduce the first-character failure through Console, then compare Virtual and a controlled non-RDP requester on Sol where possible. Correlate keyboard events, agent prompts and process exit. Obtain matching debug symbols or a debugger trace if no core is produced. The GNOME fallback's X11-time failure on Wayland is a separate fault and is not an approved fix.

**Done means:** A reproducible trigger and symbolized stack identify the failing component, with evidence distinguishing an agent/library crash from RDP input or session lifecycle behavior. If the failure no longer reproduces, record the exact matrix attempted and leave the diagnosis `PARTIAL`; an active auto-restarted service does not prove the bug disappeared.

**Evidence:** Sanitized journal/backtrace, versions, reproduction steps and the local-versus-RDP comparison. Treat lock-screen involvement as an unproven hypothesis unless demonstrated.

### T02 — Fix Discover authorization in both types

**Instructions:** Fix the component identified by T01, or package a verified upstream fix/Wayland-capable replacement with a documented reason. Preserve one authentication agent per desktop and normal PolicyKit policy. Verify the retained desktop has its own correct agent/bus/session context. Remove diagnostic or failed fallback changes after acceptance.

**Done means:** In Console and Virtual, three consecutive Discover authorization attempts accept a multi-character password and complete a harmless, reversible PackageKit operation; a second PolicyKit requester succeeds. Cancellation and an incorrect password behave normally, and the agent stays alive without SIGSEGV, automatic restart or privilege bypass. The chosen fix is reproducible after session restart.

**Evidence:** Successful/cancelled/rejected request matrix, before/after agent PID and logs, exact patch/package and cleanup of the runtime debug drop-in.

### T03 — Fix the lock-screen/teardown race

**Instructions:** Reproduce the output-release versus lock-screen race on Sol or a disposable non-Hal desktop. Inspect `PhysicalOutputGuard`, `OutputRestoreJournal`, worker shutdown and session-switch ordering. Preserve the Console lock/login screen and its authentication boundary. Keep the existing explicit Virtual lock policy separate; do not use it to suppress a Console greeter fault.

**Done means:** At least ten controlled lock/restore/disconnect interleavings preserve a functioning Console greeter and the correct locked state, with no greeter crash or unexpected unlock. Login/logout and owner release restore the intended output layout. The Virtual policy remains intentional and documented. Steve's Hal check, if required to reproduce hardware-specific behavior, remains an explicit acceptance dependency.

**Evidence:** Event ordering, greeter/worker logs, `LockedHint` and visual results, output before/after state, and focused regression coverage for the demonstrated race.

### T04 — Define and implement shared configuration and authentication scopes

**Instructions:** Inventory `server/krdpserversettings.kcfg`, `main.cpp`, `SessionController`, both brokers, workers, KCM and installed overrides. Define one validated policy model with explicit Console/Virtual applicability. Separate host-admin settings (listeners, TLS, account admission and device grants) from authenticated-user preferences and live client overrides. Preserve PAM behavior and any supported custom-user/credential behavior through an explicit equivalent; do not silently convert accounts or widen access. Read user configuration using that user's filesystem identity and bounded, validated data. Define ownership reset and reload/restart behavior for every field.

**Done means:** A setting/feature coverage table accounts for every working legacy field, including authentication/users, listen address/port, TLS, video/audio/media, monitor and wake settings. Both broker paths consume the validated model. Invalid/unauthorized values cannot partially apply or change another user's policy. Authentication and authorization behavior, including denied accounts, has focused tests and Sol live evidence. Any feature needing more implementation is assigned to T05–T08 rather than marked silently unsupported and discarded.

**Evidence:** Schema/scope/default/restart matrix, migration mapping, validation/authority tests and authentication results.

**2026-09-30 authentication source checkpoint (unshipped):** Both production
entry points validate a root-owned policy before startup. Independent PAM
any/allow-list/disabled scopes and explicit custom alias → original desktop-owner
UID mappings are implemented. Verifiers use salted PBKDF2-SHA256, not plaintext.
Identities remain absent until authentication; custom credentials never publish
a PAM identity or bypass seat/retained-session authorization. Policy snapshots
require restart. Twenty policy and eleven real RDP loopback entries pass; three
existing authority suites pass. Buzz → bounded Sol Console accepts PAM/custom
alias with first frames and rejects a valid but disallowed OS account. The
production Virtual controller/transport authentication fixture independently
accepts mapped/PAM identities and rejects disabled/wrong credentials. Both
production CLIs reject six unsafe/malformed policy cases before listening.
T08 privileged editing/help and T10 preserved legacy credentials/actual retained
desktop migration remain required before release; installed fleet is unchanged.
Evidence `~/dev/rdp/evidence/2026-09-30-t04-auth-scopes/SUMMARY.md`.

### T05 — Complete video and audio policy parity

**Instructions:** Carry codec preference, hardware/software policy, AV1 tile settings, AVC444 format/chroma timing and audio-priority semantics through both brokers and worker configuration. Reuse shared policy code instead of adding a third copy. Validate the already-deployed quality/adaptive/audio default options against T04's per-user model. Apply owner/viewer rules and clear stale overrides on release, worker replacement and desktop switch.

**Done means:** Every legacy video/audio control is honored in both applicable modes, with reported capabilities matching the encoded bytes. A working feature that remains unavailable prevents task completion and legacy retirement unless Steve explicitly approves that scope change. Changing a supported setting produces the requested stream/backend/quality policy. Playback-only, microphone-only and duplex audio priority are effective only with consent; under controlled congestion their measured delivery does not worsen relative to the disabled baseline. Fixed quality returns to its configured cap and adaptive quality recovers after pressure clears.

**Evidence:** One- and two-screen control matrix, hardware/software and codec/chroma logs, ownership/worker replacement tests, measured audio delivery and matching KCM coverage in T08.

**2026-09-30 chroma source checkpoint (PARTIAL, unshipped):** Paired wire8 sends
complete validated timing and adaptive auxiliary demand to capture workers,
including replacement per-screen encoders. Both brokers apply authenticated-user
defaults and accept current-owner partial timing overrides. Control transfer and
desktop detach reset overrides; worker capability returns separately from backend.
Six focused suites pass. Buzz→bounded Sol Console sends600/900/1900, the real
worker receives it and HEVC capture/snapshot continues. This proves dormant policy
transfer while HEVC is active. Actual AVC444 codec/auxiliary bytes, native Virtual
and multi-screen adaptive gates, telemetry and controlled audio acceptance remain,
along with T08. No package/config cutover or installed service restart. Evidence
`~/dev/rdp/evidence/2026-09-30-t05-chroma-policy/SUMMARY.md`.

**2026-09-30 AVC preference source checkpoint (PARTIAL, unshipped):** Both brokers
apply the saved standard AVC preference within actual client capabilities and
current worker availability. Shared Console viewers use420; sole admitted owner
and current Virtual attachment may use444. Control release/acquire and viewer
departure recompute policy; auxiliary/software failure revokes444, unbind clears
availability and replacement applies its own probe. Private HEVC/AV1 stays active
and falls back using current AVC policy. Codec choice is a single atomic snapshot,
queued consumers read current policy, and exact format changes discard stale
packets until a header-bearing keyframe. Eight focused suites pass. Sol/Buzz
one/two-viewer streams prove real AVC420 fallback when saved444 is unavailable;
this does not establish actual444 auxiliary encoding. Hardware444/multi-screen/
adaptive/telemetry/audio and matching T08/package gates remain. Settings restored
byte-for-byte, scratch listener/TLS/desktop permission removed, installed services
and physical/private desktop processes preserved. Evidence
`~/dev/rdp/evidence/2026-09-30-t05-avc-selection/SUMMARY.md`.

**2026-09-30 chroma cost telemetry checkpoint (PARTIAL, unshipped):** Paired
wire9 carries complete per-output min/avg/max costs and auxiliary max-gap under
the current generation/exact AVC444 codec. All capture producers participate;
broker unbind/rebind rejects stale costs. Legacy and broker logs share one full
format with per-output rate limits; measured encode time works without a stats
subscription. Ten focused suites and scratch production-target builds pass.
Raw bounded reports tolerate external-load stalls beyond the existing aggregate
presentation cap. No actual444 hardware/native timing or performance claim;
remaining T05/T08/package gates are unchanged. Evidence
`~/dev/rdp/evidence/2026-09-30-t05-chroma-costs/SUMMARY.md`.

### T06 — Complete monitor capture and layout parity

**Instructions:** Preserve Console's single-monitor, whole-workspace and independent-monitor capture choices, selection and coordinate handling. Carry the useful per-client virtual-output replace/extend/fallback behavior into Console with verified restore and local takeover. Keep these capture/layout choices distinct from the connection type. Preserve Virtual's selected-layout creation, retained topology and standard-client attach-or-create behavior. Use the existing topology/restore code where it fits; do not extend frozen experiments beyond the required parity without recording why.

**Done means:** The feature coverage table has no unexplained lost monitor mode. Applicable one/two-monitor and mixed-scale cases show correct pictures, pointer coordinates, mapping, drag between windows and resize. Disconnect, worker loss, login handoff and local takeover restore physical outputs and do not strand virtual outputs. Retained Virtual layouts survive disconnect/reattach and broker restart. Hardware surface limits are handled per captured surface or explained to the user before an unsupported request.

**Evidence:** Before/after output inventory, restore journal results, captured layout/keyframe proof, manual pointer/drag checks and focused lifecycle tests.

**2026-09-30 capture-selection source checkpoint (PARTIAL, unshipped):**
Paired wire10 carries authenticated Console owner workspace/primary/specific/
multi choices and MonitorIndex. Selected capture uses actual output index and
pixel atlas/global origin; stale epochs/input are gated, withdrawal resets
selection. Workspace keeps one aggregate surface. Selected readback is explicitly
incomplete and cannot authorize full-layout mutations; broker forwarding is
covered independently. Retained Virtual capture remains its committed layout.
Thirteen focused suites accepted (old cursor wire assertion corrected/rerun),
and five real-worker rows on a private two-output Sol compositor pass with parsed
and decoded H.264, readback and release/reset proof. Installed services/desktop
PIDs preserved; no Hal live test or performance claim under high GPU load.
Client-created Console outputs/restore/lifecycle, mixed-scale client interaction,
full limits/persistence and T08/package gates remain. Evidence
`~/dev/rdp/evidence/2026-09-30-t06-capture/SUMMARY.md`.

**2026-09-30 temporary-output policy/planning checkpoint (PARTIAL, unshipped):**
Paired wire11 carries the complete authenticated Console owner's replace/extend,
client/single/physical, fallback and canonical RDP display tuple. Identity/control
transfer, strict wire bounds and atomic invalid changes are covered. A pure
creator plan checks fresh physical/complete inventory, stable-name collisions,
foreign-output parking, output count/coordinate limits, per-surface sizes and
fractional physical mirror geometry. Eight focused suites pass; the planner's
final name/priority validation has a separate passing targeted rerun. Production
targets build in the scratch tree. No live test/package/config/service change.
The worker validates the tuple but does not yet create configured outputs; native
creator/guard/journal/restore/takeover/login/failure lifecycle and the remaining
T06/T03/T08 gates above remain. Pure geometry is not mixed-scale client acceptance.
Evidence `~/dev/rdp/evidence/2026-09-30-t06-virtual-policy/SUMMARY.md`.

**2026-09-30 configured-output lifecycle checkpoint (PARTIAL, unshipped):** The
worker now creates configured Console outputs under a physical guard/full
restore journal, captures only owned outputs and publishes incomplete verified
projection proof. Stop/control withdrawal restores and verifies surviving
outputs before journal release. Broker transfer waits for matching verified
release; failed predecessor recovery refuses startup capture. Local reclaim and
verified multi-to-single cleanup are implemented but await their native matrix.
Five strengthened private Sol worker cases pass with every screen decoded,
independent baseline enable/disable checks and exact restoration; nine focused
suites pass and all 61 corrected Virtual controller cases pass on Sol. Test-only
connector aliases exercise real KWin mutations without physical-panel claims.
Qt/KScreen primary selection after teardown and obsolete lifecycle/rename test
fixtures were corrected. Aggregate resize is unavailable until output-specific
resize/Fit is implemented. Hardware/local reclaim, forced failure/worker-loss/
replacement/login, T03 race, mixed-scale interaction/physical mirror, retained
persistence/limits and T08/T27 gates remain. Installed fleet/services/retained
desktop preserved; no Hal live test or performance claim. Evidence
`~/dev/rdp/evidence/2026-09-30-t06-virtual-lifecycle/SUMMARY.md`.

**2026-09-30 owned resize/Fit worker checkpoint (PARTIAL, unshipped):**
Configured Console creator outputs now use the shared output-specific resize
and managed Fit planners, including owned single output and fractional scale.
Complete fresh inventories bound authority and preserve every foreign output;
custom-mode preparation, apply and captured keyframe proof verify preservation.
Fit limits use the captured workspace independently of its global Extend origin.
Native sequence testing exposed KWin replaying remembered modes when a later
connector is created; creation now rechecks/restores the entire requested tuple.
Four focused pure suites and five strengthened private Sol worker cases pass
(seven including setup/cleanup, no skips), with stale/foreign resize refusal,
fresh decoded frames after resize/Fit, dependent placement, incomplete topology
and exact final restoration. Connector aliases are not physical hardware proof.
Broker one-use preview/commit, client capability/ownership acceptance and UI
integration remain independent work before this feature is exposed/deployed.
All remaining lifecycle/mirror/mixed-scale interaction/retained persistence,
T03/T08/T27 gates above stay open. Installed services/desktops/retained session
and lock state preserved; no Hal live test or performance claim. Evidence
`~/dev/rdp/evidence/2026-09-30-t06-owned-resize/SUMMARY.md`.

**2026-09-30 owned resize/Fit broker/client checkpoint (PARTIAL, unshipped):**
Broker preview/commit now uses explicit configured creator ownership, exact
before inventory/priorities and one-use revision/generation/controller-bound
tokens. Backend dispatch and exact independent topology readback are tested;
wrong result kinds/IDs/generations cannot advance the transaction, and partial
readback stops forwarding while retaining the restoration lease. Viewers cannot
write. Client validates `consoleOwned` and every output's owner/kind; owned
single or multiple outputs can resize/Fit without physical consent while
add/remove/position/primary/physical changes stay unavailable. Fit planning
shares one workspace translation helper with the worker.
Four focused server suites, the complete client topology suite and nine Buzz
offscreen dialog cases pass. The final private Sol worker regression passes five
cases (seven with setup/cleanup), with decoded frames and exact restoration.
These are separate broker/client/worker gates; actual RDP transaction integration,
native physical mirror/mixed-scale/pointer/drag, takeover/loss/login/failure,
retained persistence/limits and T03/T08/T27 remain independent work.
Fleet/services/retained session and locked state are preserved; no Hal live test
or performance claim. Evidence:
`~/dev/rdp/evidence/2026-09-30-t06-owned-transactions/SUMMARY.md`.

**2026-10-01 real-RDP attempt (PARTIAL, unshipped):** A bounded Sol private
compositor/production PAM/broker/worker fixture and Buzz client verify owned
single resize, rev2, fresh HEVC decode and exact independent output restoration.
Final Fit fails with CUDA context out-of-memory and a partial transaction;
external AI holds about6.8GiB of8GiB, but our allocation lifetime has not been
ruled out. The software rerun is not accepted because actual Console session429
signed out during the run; its cause awaits clarification. Console/Virtual
brokers remain active, but user3389, PolicyKit and retained desktop are now
inactive. Configuration hash remains unchanged. The fixture now aborts when
the UID1000 physical desktop disappears or becomes a greeter. Final host/client
builds and Buzz dialog9 pass; no installed update or Hal live test.
The client still omits its initial standard monitor tuple, so owned two-screen
RDP integration remains independent implementation work. Final software Fit,
loaded fallback, lifecycle/mixed-scale/persistence and T03/T08/T27 gates stay
open. Evidence `~/dev/rdp/evidence/2026-10-01-t06-owned-rdp/SUMMARY.md`.

**2026-10-01 monitor-request/quiet-worker checkpoint (PARTIAL, unshipped):**
Client `ef692be` adds an explicit persisted Console monitor request, default off
for new/existing profiles. App freezes current physical-pixel inventory before
the engine starts; bounded settings send the standard FreeRDP monitor block.
Retired assignments/local mapping do not enable it; Virtual and inventory
queries never request this layout. Three pure suites and Buzz real edit-form12
pass. Buzz actual GUI/engine with two offscreen screens reports MonitorCount2,
but a closed-port probe is not real RDP integration acceptance.
Steve paused Sol AI work (GPU169MiB used/7616MiB free, no compute apps). Seven
private native worker cases pass (nine including setup/cleanup, no failures or
skips), including hardware HEVC single/two creation, resize/Fit, decoded frames
and exact restoration. Quiet HEVC exposed KWin replaying overlapping connector
positions after mode repair; initial Extend placement now uses the requested
tuple, capture waits for KScreen/Qt agreement, and worker failures preserve their
bounded reason after Ready. Logged-out render ACL required only per-process
fixture render-group access; account/device permissions unchanged.
Installed brokers/greeter/config preserved, no deployment or Hal live test.
Sol is still at greeter1043; final real Console RDP requires a UID1000 KDE owner.
Broader T06 lifecycle/mirror/mixed-scale/persistence and T03/T08/T27 gates, plus
the earlier loaded CUDA OOM/fallback investigation, remain independent work.
Evidence `~/dev/rdp/evidence/2026-10-01-t06-monitor-request/SUMMARY.md`.

### T07 — Preserve display wake and inhibition

**Instructions:** Integrate the existing `DisplayWakeGuard` semantics into the desktop workers or equivalent user-session mechanism, not a root broker's session bus. Honor the configured wake policy and acquire/release inhibition with streaming ownership. Keep display power management distinct from unlocking and from Virtual's intentional lock policy. Handle pending D-Bus replies and teardown without leaking inhibition.

**Done means:** On a non-Hal Console, a sleeping display produces usable capture after connection when wake is enabled; disabled behavior follows the configured policy. Multiple viewers/owner changes do not release the final active stream's needed inhibition or keep it forever afterward. Disconnect and worker failure release it, including a pending inhibit reply. No request unlocks a locked desktop. Virtual activity affects its own desktop policy without altering the physical seat.

**Evidence:** Power-state/capture and inhibition-cookie lifecycle observations, async teardown tests, and lock-state checks.

**2026-09-30 source checkpoint (PARTIAL, unshipped):** Worker session/private-bus
guard integration, authenticated pre-Ready display demand, admitted viewer
aggregation, current Virtual binding/revoke policy and delayed-reply teardown
are implemented on paired wire v7. Five focused pure suites and the isolated
D-Bus guard suite pass. Buzz → a bounded Sol scratch Console broker :3397 wakes
after requested DPMS-off, presents the actual lock screen, retains one cookie
through two viewers/owner departure, and releases after the last disconnect.
An injected test-worker SIGKILL removes its cookie; its replacement recovers
capture and final disconnect releases the new cookie. LockedHint stays yes
during the locked test; an explicit restoration unlock is recorded separately.
WakeDisplayOnConnect=false sends no wake/inhibit request; temporary preferences
are restored byte-for-byte. No installed service, retained desktop or Hal test.
Virtual native namespace policy acceptance and matching T08 KCM/package gates
remain. Evidence: `~/dev/rdp/evidence/2026-09-30-t07-display-wake/SUMMARY.md`.

### T08 — Complete the KDE settings page and stock coexistence

**Instructions:** Update the separate Farside KCM to control the new shared model and the correct Console/Virtual services. Label host versus user scope, client overrides and live/restart requirements. Provide privileged handling only where host settings require it; saving per-user preferences must not rewrite host-admin policy. Cover the settings from T04–T07 and update this task's checklist as GPU controls land in T16. Preserve stock KDE KRDP's separate identity.

**Done means:** Every supported field has a working binding, default, validation and help; unsupported controls cannot appear applied. Save/reload/restart round trips reach the intended broker/worker. With stock KRDP and Farside installed together, each live settings page changes only its own configuration/service; file paths, certificates and secret-store identities stay distinct. Authorization cancellation leaves settings unchanged.

**Evidence:** Config-to-UI coverage audit, populated-state screenshots on non-Hal hosts, invalid-value and save/reload checks, and the outstanding live dual-KCM acceptance matrix.

**2026-10-01 authentication-settings checkpoint (PARTIAL, unshipped):** A
fixed-path privileged helper edits only root Console/Virtual admission policy,
using bounded stdin, exact revision checks before/after password derivation,
cooperating-writer lock, root0600 atomic replacement and the production parser.
Public snapshots contain only PAM/alias/owner metadata. Unchanged aliases retain
their verifier only for the same route/alias/owner; new/rebound aliases need a
new password. KCM exposes a scoped sign-in page with load/stage/save/reload,
password masking/clearing and explicit restart help; cancellation/denial keeps
pending edits. Three pure suites, 17 isolated root filesystem cases on Sol,
Buzz populated page flow and all 13 real KCM plugin UI cases pass. Initial
QML lexical binding/dialog sizing and fixture-owner-call errors were corrected;
failed logs are retained. Generated policy XML/package contracts and pkexec
dependency are wired; no installed package or policy change.
These are separate helper/protocol/UI gates, not a real installed pkexec/password/
broker-restart round trip. The main page still manages legacy service/settings;
correct Console/Virtual service status/actions, host listener/TLS/device controls,
atomic shared preference bindings/help, legacy UI cutover/migration, live stock
coexistence and T27 package/rollback/deployment remain independent work.
Spec: `../specs/2026-10-01-broker-settings-design.md`. Evidence:
`~/dev/rdp/evidence/2026-10-01-t08-auth-editor/SUMMARY.md`.

**2026-10-01 system-service settings checkpoint (PARTIAL, unshipped):**
A separate two-route page now reads the system Console/Virtual services and
offers normal authorized start/stop/restart and persistent startup controls.
Stop/restart confirms client disconnection. Job completion is matched by exact
unit/path, including signals before method replies; queued replies alone never
report success. Shared connection subscriptions and manager replacement/late
callbacks are handled. File changes/reload/actual status are separate; errors
survive readback/polling, and unknown/masked/static/runtime startup states have
appropriate controls. Pure model/lifetime tests, twelve private Sol bus cases
(including setup/cleanup and production read-only installed broker state), Buzz
page flow and fourteen compiled KCM cases pass. This does not restart installed
brokers or accept installed PolicyKit dialogs. Main-page legacy cutover, host
listen/TLS/device administration, atomic user-preference bindings/help, T10/live
stock coexistence and T27 delivery remain independent work. Evidence:
`~/dev/rdp/evidence/2026-10-01-t08-services/SUMMARY.md`.

**2026-10-01 shared-preference settings checkpoint (PARTIAL, unshipped):**
The scoped KCM page now binds all seventeen parsed preference fields, with
separate Console display/session/Virtual compatibility help, per-field host
inheritance, locked-key preservation, staged validation, explicit save/reload/
discard/default actions and reconnect help. Public metadata excludes raw legacy/
host/credential fields. Shared lexical/value parsing preserves unrelated bytes,
repeated sections and localized/expanded/immutable entries. Atomic user0600
writes take a safe lock, reject stale full-document snapshots and use an
inspected directory descriptor; new directories are private even with umask0002.
Four pure suites pass. Sol's private canonical-home namespace verifies production
editor save -> authenticated reader agreement for every field and restored host
inheritance despite unrelated HOME/XDG; four cases including setup/cleanup pass.
Buzz actual page4 (all bindings, keyboard edit, invalid/stale/cancel/default,
width640 geometry) and compiled KCM15 pass, with populated screenshots reviewed.
Actual Sol preferences/broker PIDs and Buzz config/profile hashes preserved;
private fixtures removed. This is reader/storage/UI acceptance, not native
worker codec/display/audio or installed package acceptance. Main-page legacy
cutover, host listener/TLS/device editor, real installed authorization/restart,
T10/live stock coexistence and T27 remain independent work. Evidence:
`~/dev/rdp/evidence/2026-10-01-t08-preferences/SUMMARY.md`.

**2026-10-01 host-document settings checkpoint (PARTIAL, unshipped):**
A pure bounded typed model now covers Console12, Virtual host11 and Virtual
session2 fields, fixed file/environment names, validation and unit defaults.
Quoted/multiline/continuation/duplicate statements round trip while unrelated
administrator bytes remain private and unchanged. Missing PCI means no grant.
The Console source unit supplies explicit address/port defaults0.0.0.0/3391;
installed units and legacy3389 remain unchanged. Five complete pure suites pass,
including reconciled old-name launch fixture expectations. Six actual systemd259
Sol disposable-unit checks verify raw and saved documents against the real
EnvironmentFile parser. Broker PIDs/preferences preserved, fixtures removed.
This proves typed format and serialization, not authorized host administration.
The spec now defines fixed root writing, revision/atomic/readback, TLS existing/
import/standard paths, device checks, runtime overrides and restart/new-desktop
scope. That helper, complete host UI, legacy main cutover, installed authorization,
T10/live stock coexistence and T27 still remain. Evidence:
`~/dev/rdp/evidence/2026-10-01-t08-host-format/SUMMARY.md`.

**2026-10-01 root host-writer checkpoint (PARTIAL, unshipped):**
Fixed three-scope stdin helper now performs revision-bound root0600 atomic saves,
directory/file/lock inspection, sync and independent readback. Keep/existing/
standard/import TLS operations are explicit. New private complete generations
publish both managed paths together; failed publication removes only staged
material, and old generations/standard fingerprints remain untouched. Shared
bounded noninteractive certificate inspection rejects invalid/mismatched/
encrypted/expired/future imports. Shared actual GPU device authority validates
changed stable PCI grants. Seven full pure/boundary suites and22 isolated Sol
root helper checks pass. Actual host/auth/user/TLS hashes and broker PIDs are
preserved, root fixture removed. Own fixed PolicyKit action and package contracts
are wired; no installed action/package change. Replies explicitly do not claim
runtime application. The scoped host UI, actual installed/drop-in/readback,
installed authorization/restart, legacy main cutover, migration/coexistence and
T27 remain. Positive Console loopback hardware is unavailable on Sol; Virtual
loopback namespace grants remain separate T05/T08 implementation, not fulfilled
by metadata/refusal. Standard PipeWire camera remains separate. Evidence:
`~/dev/rdp/evidence/2026-10-01-t08-host-writer/SUMMARY.md`.

### T09 — Make client types explicitly Console or Virtual

**Instructions:** Add an explicit persisted type and select it when creating/editing a connection. Use authenticated capabilities to validate behavior; a custom port must not change the type. Replace the old `physicalConsole` distinction throughout form, cards, session identity, menus, topology and device paths. Keep an internal migration representation only while old saved entries are awaiting T10. Standard RDP hosts retain their appropriate existing-desktop behavior.

**Done means:** New Farside profiles offer exactly Console and Virtual, including with non-default ports. Session titles and controls identify the selected type consistently; unavailable actions give a reason. An endpoint/type mismatch is explained without silently opening a different desktop. Existing profiles remain usable until migrated, and IDs, stored secrets and layouts are preserved. No active window changes route or profile as a side effect of saving a different entry.

**Evidence:** Profile serialization/migration tests, default/custom-port fixtures, capability mismatch and standard-host checks, and non-Hal UI screenshots.

### T10 — Migrate settings, profiles and remembered credentials

**Instructions:** Build an idempotent migration with a dry run, exact backups and rollback. Translate per-user settings using T04's scope rules; preserve originals rather than overwriting stock config. Migrate Farside :3389 profiles to Console only when its parity gates pass; retain explicit Virtual routes and custom endpoints. Preserve profile IDs or explicitly migrate their secret-store keys. Verify the destination service's certificate independently: Console and the old user server can have different certificates, so do not copy trust from the old port or auto-accept a different fingerprint. Reconcile Buzz's three previously pending passwords without logging them or pretending unavailable secrets migrated.

**Done means:** Dry-run and actual fixture migrations yield the same mapping; rerunning changes nothing. Settings, account admission, remembered passwords, host identities, trust and saved monitor layouts work after migration and after rollback. Unknown/custom profiles are retained with an actionable explanation. Every intended fleet profile has a verified result or a named outstanding user action; unresolved credentials prevent claiming full migration. Running client sessions are untouched until voluntarily reopened.

**Evidence:** Sanitized migration manifest, backup hashes, verified destination fingerprints, secret-store success/failure counts without values, connect/rollback results and remaining exceptions.

### T11 — Complete camera and redirected-media acceptance

**Instructions:** Use a real Buzz camera and consumer/conferencing application in Sol Console and Virtual. Exercise off/on, reselect between two available sources, consumer open/close, disconnect, worker loss and ownership takeover. If a second camera is unavailable, retain that acceptance as pending. Check the remote source belongs to the selected desktop. Include microphone/playback consent and host-silencing behavior, and the optional V4L2 bridge where configured.

**Done means:** Ten on/off cycles leave no stale source/channel, and acknowledged off prevents further redirected samples. Camera LED/demand starts only with an allowed consumer and stops within five seconds of consumer closure/off/disconnect. Reselect demonstrably changes the source and rejects old-generation frames. A real bidirectional conferencing call delivers recognizable camera, microphone and playback in both types; viewers and former owners cannot capture. Configured loopback works without granting a root broker access to the user's PipeWire graph.

**Evidence:** Human LED/source notes, application results, demand/start/stop logs, per-desktop node identity and stale-generation/backpressure tests. Existing JPEG/150-frame evidence satisfies transport smoke coverage only.

### T12 — Verify standard RDP clients

**Instructions:** Test current installed FreeRDP/Remmina on Buzz and Windows mstsc when a Windows test machine is available. Record client builds and negotiated capabilities. Exercise Console sign-in/lock, Virtual attach/create/reconnect, clipboard, monitor/resize behavior and supported playback/mic/camera consent. Unsupported channel combinations must be reported truthfully. Do not load Farside private extensions before their handshake.

**Done means:** A compatibility matrix states pass, genuinely unsupported, or untested for each client/feature/mode, backed by live evidence. Standard video and desktop lifecycle work without Farside's client. Available standard media channels honor the configured consent policy and stop on disconnect. Virtual's standard-client policy is honored. Required Windows cases remain `PARTIAL` until hardware is available; an unsupported feature needs a documented protocol/client limitation rather than a silent failure.

**Evidence:** Client/version/capability matrix, layout and media results, reconnect/consent logs and open external-environment dependencies.

### T13 — Retire redundant per-user service and remove old code

**Instructions:** After T02–T12 pass, migrate and cut over one test host first. Make the system Console broker the only Farside route to an existing desktop, keeping :3391 to avoid stock KRDP :3389. Remove the redundant Farside per-user listener/unit and implementations; extract genuinely shared policy first. Remove obsolete UI branches, flags, packaging entries, launchers and tests that cover only the deleted path. Keep offline backups and rollback packages rather than retaining a hidden third running route.

**Done means:** A clean install and an upgrade expose exactly Console and Virtual Farside routes. The obsolete Farside unit/listener cannot auto-start, and stock KRDP remains independently installable. No working inventory feature, saved credential or monitor layout is lost. A test-host rollback restores the previous usable state. Source and package audits account for remaining legacy names solely as documented migration inputs, upstream names or archived evidence. Hal cutover follows its separate deployment rules and waits for verified migration results.

**Evidence:** Dependency/source/package audit, clean-install/upgrade tests, listener/unit inventory, parity sign-off, profile migration and rollback report.

## P1: NVIDIA completion and reliability

### T14 — Complete NVIDIA HEVC functional acceptance

**Instructions:** Use Sol NVENC in both Console and Virtual and a hardware-decoding client. Exercise moving content, quality changes, live resize, two independent screens, forced keyframes, reconnect, worker replacement and injected encoder failure. Use an isolated NVIDIA decoder test on Sol; Steve owns the live Hal NVDEC check. Record external AI load and staging/conversion cost.

**Done means:** Both modes deliver sustained decodable HEVC, not only a first frame. Resize/reconnect produce valid independent keyframes; one failing stream does not corrupt the other. A backend failure recovers at a valid keyframe or produces a clear reconnect/error state without a permanent black screen or false hardware label. Both encode and decode performance pass the T26 scenario that applies to the hardware, or performance remains separately `PARTIAL`.

**Evidence:** Motion/resize/two-screen/fallback matrix, actual contexts and backend logs, p50/p95 timings, and recorded load.

### T15 — Discover GPUs by stable identity and report actual use

**Instructions:** Probe allowed devices by opening codec contexts in the actual worker/client environment. Store stable PCI identity and map it to current render nodes/CUDA ordinals. Include software, AMD/Intel VAAPI and NVIDIA NVENC/NVDEC; Intel QSV is a separate backend only if implemented. Keep Vulkan Video and Steve's Vulkan compute AV1 shader as distinct future capabilities, unavailable until proven. Attach the actual selected backend/device to logs, control replies and stats, including fallback.

**Done means:** A device table lists real per-codec encode/decode availability. Reordered node/ordinal fixtures do not change the selected physical GPU. Missing/restricted devices cannot be advertised as usable. Runtime telemetry identifies the context producing each stream and changes after fallback. Sol's RTX 2070 is never advertised as hardware AV1. Multiple-GPU Hal enumeration uses read-only/offscreen checks, with Steve providing live selection acceptance.

**Evidence:** Probe/context results, PCI mapping and sandbox tests, stable-identity fixtures and stats/backend consistency assertions.

### T16 — Add encoder/decoder selection and fallback controls

**Instructions:** Add server allow-list/default controls and client selection UI using T15 identities. Support auto, software and a specific available hardware backend/device. Preserve AMD as Hal's automatic default and NVIDIA HEVC on Sol. Use one encoder/decoder device per stream; different screens may select different devices. Do not split one frame across vendors or migrate a running stream merely because a GPU-utilization snapshot changed. Define explicit-unavailable-device and measured-resource-failure behavior, and ship matching KCM/client help and telemetry.

**Done means:** The user can inspect and choose available encode/decode devices, save the choice and see the actual result. Authority and hardware limits prevent unsupported selections. Auto and explicit choices round-trip correctly; fallback/rejection has a clear reason and truthful stats. Two independent screens can use their assigned devices where supported. Resource pressure uses measured latency/context-allocation/VRAM evidence and a documented stream-boundary transition.

**Evidence:** UI/persistence and authority checks, single/multi-device scenario results, unavailable-device/fallback tests and updated shared setting matrix.

### T17 — Add NVIDIA AVC420 encoding and decoding

**Instructions:** Add the AVC420 NVENC/NVDEC paths and real-context probes using T15/T16. Preserve low latency, bounded queues, IDR controls, standard AVC compatibility and truthful hardware/software reporting. Keep AVC444 as its own compatibility work; do not relabel AVC420 as full color.

**Done means:** Sol Console and Virtual encode an independently decodable AVC420 keyframe and sustain motion to Farside and an available standard client. NVIDIA decoding works on the chosen NVIDIA test host. Quality, resize, reconnect and failure fallback pass, and hardware limits are validated. Matching selector/capability/packaging updates ship with the backend.

**Evidence:** Parsed codec/keyframe proof, standard/Farside client matrix, latency/fallback tests, backend telemetry and package hash.

### T18 — Add NVIDIA AV1 on supported hardware

**Instructions:** Add AV1 NVENC/NVDEC only for devices whose actual contexts open. Handle codec-specific dimensions, cropping/render size, tiles, keyframe proof and decoder availability. Use an eligible non-Hal GPU if available; otherwise implement/probe safely and leave live acceptance for Steve on Hal. Do not run an agent GPU/live-session benchmark on Hal.

**Done means:** Eligible hardware produces and consumes valid AV1 with correct visible size, tile policy and independently decodable keyframes. Motion, resize, one/two-screen operation, load and fallback gates pass on an authorized host. Unsupported devices offer an available codec instead. Hal-only hardware availability leaves the task `PARTIAL` until Steve's acceptance is recorded.

**Evidence:** GPU/context identity, parsed live headers, visible-dimension checks, performance/fallback matrix and user-owned Hal results when applicable.

### T19 — Make connection-health explanations accurate

**Instructions:** Separate or accurately label decode, hardware-frame download, conversion and presentation timing. Replay the observed retransmission and p95 traces against `SignalAssessment`/`SessionStats`. Explain recovered packet loss, actual queue/capacity trouble and client processing headroom clearly. Evaluate threshold/hysteresis changes with evidence; keep the client badge independent from the server's capacity-based policy.

**Done means:** A timing label describes the stages it measures, and the same evidence trace yields a documented, understandable verdict. Healthy recovered-loss, genuinely constrained-link and overloaded-decoder traces are covered. If thresholds change, tests show why the new verdict is better without hiding sustained problems or flapping. If existing thresholds are retained, the evidence and rationale are documented. No grade message starts controlling server codec/quality by accident.

**Evidence:** Trace replay, measured stage boundaries, before/after explanations and the explicit policy decision.

### T20 — Diagnose and fix Sol's DrKonqi loop

**Instructions:** Identify which process repeatedly crashes or relaunches DrKonqi, collect its stack/unit/parent chain, and separate a reporting-tool failure from repeated crashes in another application. Fix the demonstrated cause or package a verified fix. Keep legitimate crash reporting available.

**Done means:** The original reproduction and a normal 30-minute Console/Virtual observation show no repeated crash-dialog/reporter spawning. A controlled ordinary crash can still produce one useful report on the test desktop. Session restart preserves the fix. Disabling all reporting or killing reporters repeatedly does not count.

**Evidence:** Process/crash chronology, root-cause stack, exact fix and recurrence/reporting checks.

### T21 — Fix client session-dialog and startup warnings

**Instructions:** Guard null-session bindings and lifecycle teardown in `VirtualSessionsDialog.qml`; test open/close/connect/disconnect/failed authentication. Audit certificate-cache/name messages: preserve strict fingerprint and mismatch handling while avoiding misleading changed-host errors for a correctly verified saved service. Trace the obsolete NoMachine `LD_PRELOAD` to its actual source and remove only a demonstrated stale reference, preserving unrelated environment configuration.

**Done means:** The reproduced QML null dereferences are absent across the lifecycle matrix. A valid pinned connection gives clear certificate status, and a genuinely changed/untrusted certificate is still refused or requires explicit trust. Normal Farside launch no longer attempts to preload a nonexistent library; any legitimate certificate warning is retained and explained. No saved trust or credentials are weakened to reduce log noise.

**Evidence:** Before/after warning logs, certificate valid/mismatch tests, launch environment source/change record and profile hashes.

## P2: performance and product completion

### T22 — Decode independent screens independently

**Instructions:** Give each surface/screen a bounded decode queue and independent worker/context; keep Qt presentation on the correct thread. Preserve frame order, resize/keyframe generations, ack semantics and cleanup. Start from the T26 two-screen baseline and inject slow work on one stream.

**Done means:** A deliberately delayed surface no longer stalls the other surface's decode/presentation. Tests prove correct ordering, acknowledgements, resize/disconnect cleanup and bounded memory. Under the same recorded load, the unaffected surface's p95 processing time increases by no more than 10% versus its single-stream baseline. Aggregate CPU/latency results and limitations are reported; more threads alone is insufficient.

**Evidence:** Per-surface timing/queue traces, injected-stall result, lifecycle tests and matched baseline comparison.

### T23 — Tune HEVC quality and bitrate

**Instructions:** Compare current HEVC quality/bitrate mappings for AMD VAAPI, Intel VAAPI, NVIDIA and software using the same saved text, motion and image corpus. Record actual codec headers, quantizers, rate control and staging costs. Define the reference operating point before changing defaults; retain user overrides and codec-specific mappings.

**Done means:** A published comparison selects a justified operating point for each supported backend. At matched objective quality within 1 dB PSNR on the same motion corpus, the accepted change reduces bitrate by at least 10% or p95 encode time by at least 10%, without exceeding the scenario's frame budget or causing a visible text-quality regression in the human review. If that target is not achieved, publish the measurements and retain the existing default; do not mark optimization delivered. Any shipped defaults have matching UI/docs and rollback.

**Evidence:** Corpus/settings, rate/quality/timing table, visual review and explicit keep/change decision.

### T24 — Implement the Sessions page

**Instructions:** Present running, retained, connecting and failed sessions with clear machine/type/desktop identity and ownership. Support the existing authorized operations to open/resume, disconnect while retaining a desktop, and explicitly end or dismiss eligible rows. Keep background listing separate from desktop attachment and preserve another owner's session. Use stable identities internally and readable names in the UI.

**Done means:** Populated Console/Virtual/multi-user, empty, offline, failed and reconnect states are usable. Refresh/listing opens no desktop or session window. Closing the page never ends a desktop. Resume attaches to the selected retained desktop; End requires an explicit destructive-action confirmation and reaches only that authorized desktop. Stale rows reconcile after broker restart and failure. Keyboard navigation and accessible action names work.

**Evidence:** Populated-state screenshots, operation/ownership tests and Sol/Buzz retained-lifecycle acceptance.

### T25 — Complete the remaining client UX redesign

**Instructions:** Use `~/dev/rdp/CLIENT-UX-REDESIGN.md` as the starting design, updating its obsolete :3389/:3391 screen-routing model to the current sole-Console target. Keep explicit Console/Virtual identity without inventing a third Direct Desktop type. Group machine/account/service data without merging distinct accounts, trust or layouts. Separate discovery/sign-in from opening a session, simplify desktop creation without weakening server validation, and keep supported actions discoverable with useful reasons when unavailable.

**Done means:** Add/edit/open/resume/create/disconnect/end flows work from real populated state in narrow/wide and light/dark views. Users can identify Console versus Virtual and the selected retained desktop before opening it. Discovery does not unexpectedly start capture or a window. Migration preserves per-service certificates and secrets. Keyboard, focus, accessibility and error recovery checks pass, and screenshots demonstrate the approved behavior rather than only a mockup. T24 functionality is integrated rather than duplicated.

**Evidence:** Updated design/routing decisions, visual comparison, flow/keyboard checks and real Sol/Buzz results.

### T26 — Complete load-aware performance and remaining visual gates

**Instructions:** Establish a repeatable 1080p30 motion baseline, plus text/idle, two-screen and resize/reconnect cases. Where claiming 60 fps, add a separate 60 fps scenario. Record requested/presented fps, per-stage p50/p95, bitrate, queue depths, CPU, GPU/encoder/decoder utilization, VRAM, TCP capacity/RTT/retransmissions and active AI processes. Run a quiet comparison when a host is naturally available, then a recorded representative concurrent-load run. Do not stop Steve's AI workloads without authorization; if no quiet window exists, retain performance acceptance as pending. Use scoped throttling on non-Hal test traffic with guaranteed cleanup.

**Done means:** Each supported tested backend has a reproducible table and clearly separated functional, quiet-performance and loaded-performance verdicts. For the declared quiet target, after warmup over five minutes: presented fps averages at least 95% of the requested rate, p95 encode and client processing each fit their frame interval, and queues/memory do not grow continuously. A loaded scenario may miss those targets but must identify measured pressure, bounded behavior and recovery; it cannot be presented as the quiet result. Slow-link tests meet the recorded 6 Mbit/s entry/recovery criteria (about 20/90 seconds) and have no false slow-link baseline event.

**Additional completion checks:** Repeat AV1's 1080p30 9–15 Mbit/s target on the defined corpus and capture current live tile/render-size headers; if data rejects that target, document and justify a revised criterion before closing it. A human verifies I-beam/resize/hidden cursors, pointer position, screen mapping and cross-window drag on non-Hal desktops. Verify intentional Virtual lock policy. Steve's Hal visual check is recorded separately. Only changed or unresolved cases are rerun after a release.

**Evidence:** Raw samples, scene/settings identifiers, load snapshots, plots/table and per-case verdicts. A changing AI workload without a controlled comparison cannot establish a Farside performance improvement or regression.

## Delivery and closure

### T27 — Package and deploy each accepted release

**Instructions:** Build committed sources in isolated package directories with pinned private libraries. Include the matching KCM/client controls for shipped settings. Simulate installs, verify hashes and package path coexistence, archive current and rollback debs read-only, and preserve holds. Gate on Sol/Buzz, then Intel/AMD hosts for relevant codec changes, then remaining fleet hosts; Hal last. Stop on a failed gate and restore the previous known-good package/state on the failing host. Migration releases additionally require T10/T13 cutover and rollback evidence.

**Done means:** Every intended release target has an explicit installed version, running service/worker version, active-route inventory, integrity/dependency result and acceptance verdict. No unrelated package upgrades, lost holds, cert changes, profile loss or surprise retained-desktop termination occur. Hal has zero incoming connections before restart and preserves its work processes/settings/windows. A failed/unreachable host remains a named incomplete target. Client release versions/tags and source pushes match the delivered packages.

**Evidence:** Fleet table, apt simulations, package hashes, preservation manifests, gate logs, rollback test and handoff entry. The current Sol/Hal `f255ee2` rollout is the baseline, not proof that future consolidation is deployed.

### T28 — Keep documentation current and close milestones honestly

**Instructions:** Maintain the ledger at `.superpowers/sdd/2026-09-30-farside-remaining-work/progress.md`, with one row per task and commit/package/evidence references. Update this plan's register status through the ledger, relevant `research.md` entries, current fleet snapshot and newest handoff Log after each accepted slice. Annotate old design/test documents whose deployment or routing assumptions are superseded rather than treating their old snapshots as current instructions.

**Done means:** Each task marked done has the evidence specified above, exact delivery scope and no hidden untested requirement. P0 closes only when T01–T13 are accepted, migrated and delivered through T27. P1 closes after its applicable tasks and hardware/user acceptance; P2 closes after its later behavior/performance gates. All-plan completion requires all tasks accepted or an explicit Steve-approved scope change documented with the reason. Hardware, Windows or user-owned Hal gates still awaiting evidence remain visible as `PARTIAL`. Archive the ledger only after actual completion.

**Evidence:** Up-to-date task ledger, tracker and handoff links, closed-versus-open acceptance table and accurate fleet versions.

## References and precedence

- [Console/Virtual consolidation inventory](../specs/2026-09-29-console-virtual-consolidation-design.md).
- [NVIDIA backend design](../specs/2026-09-29-nvidia-video-backends-design.md).
- `~/dev/rdp/DEVICES-DESIGN.md`, `KRDPCTL-V2-CONTRACT.md`, `CLIENT-UX-REDESIGN.md`, `PENDING-TESTS.md` and the dated evidence directories.
- `research.md` for existing OPT identifiers; shared `CLAUDE.md`/handoff current snapshots for actual deployment.

The earlier consolidation checkpoint's source-only `c8cc750` installation wording and the older UX design's three-route fallback are historical. Current deployment is `f255ee2` on Sol/Hal, and the latest user-approved target is one Console route plus one Virtual route. This plan supplies the remaining task acceptance; it does not revive the third Farside route in the finished product.
