# Farside: release assessment and plan to finish

2026-10-01. Steve requested a stop at the next checkpoint, an assessment of
shipping new client/server releases, and a new plan. He explicitly emphasized
wallet care. Execution pauses after this assessment is recorded; no candidate
build, package installation, service restart or wallet operation occurred here.

**Execution process superseded later on October 1:** Use the [task-by-task plan](../../plans/2026-10-01-farside-task-plan.md)
for individually selected work and bounded verification. This assessment remains
a reference; do not automatically resume R1–R6 or the paused Goal.

## Governing scope

The [T01–T28 plan](2026-09-30-farside-remaining-work.md) retains its scope,
dependencies and completion criteria. P0 T01–T13 plus applicable T27/T28 remains
the first full delivery milestone. This document changes execution order toward
concrete releases; it does not waive migration/native/rollback gates or mark
partial tasks done. Smaller accepted releases are permitted by T27; shipping
one does not close P0 or the entire plan.

## Current assessment

| Surface | Delivered baseline | New source / evidence | Shipping assessment |
|---|---|---|---|
| Client | 0.6.3 / `2df38a7` across all six hosts | `ef692be`, three newer commits: owned Console resize/Fit, transient capability handling and explicit default-off initial monitor request; pure/Buzz UI/private worker evidence | A 0.6.4 candidate is feasible independently of complete server consolidation. It is not ready today: wallet retry protection, fresh packaged regression against installed and candidate servers, version/metadata/dependency/rollback gates remain. |
| Server | `f255ee2` on Sol/Hal; `4b308bd` elsewhere; wire 6 | `d437511c`, 30 newer commits, 172 changed files including tests/docs; source wire 11 | A package candidate can be built after the wallet guard. Full production rollout is not accepted: installed KCM/helper/polkit/restart, integrated worker/native regression, migration and rollback remain. Broker/worker must be paired; a newer binary on disk is not proof of an activated release. |
| Settings | Previous installed KCM | `44726890` main cutover: four scoped pages, 17 preferences, 25 host/session controls, system services and runtime inspection; eight pure suites and Buzz Main 11/KCM 14/Services 3/Hosts 13 pass | Source accepted; real installed authorization, saved-policy effect and live stock dual-module behavior are unverified. |
| Migration | Earlier rename copy/retry | `d437511c` planner: pinned 26-field schema, explicit effective defaults, owner-bound admission facts, locks/conflicts/no-op; 13 pure suites in 7.52s, 32 migration cases | Planner accepted, no writes. Durable backup/journal/apply/rollback and actual secrets/profiles/retained/trust integration remain. |
| Sol runtime | Installed brokers 141254/141255 active | GPU 169MiB used/7616MiB free/3%, no compute apps; physical SDDM greeter 1043, no signed-in UID 1000 desktop | Quiet for native work. Does not settle loaded CUDA failures or prove Console desktop integration. |
| Wallets | Original stores unchanged in this review | Buzz migration metadata still reports three pending client entries; no secret-store access performed | Do not retry blind writes. Pending entries remain pending until safely verified. |

Steve confirmed Sol Console Discover password entry and normal lock/unlock work.
That is useful live evidence, not the remaining wrong-password/cancel/Virtual/
restart matrix or the original lock/output-release interleavings. Delivered T09
client types and T19 health explanations remain done. Other source checkpoints
must keep their native/installed/migration limitations visible.

### Why delivery stalled

- Work repeatedly closed individual source components, without producing a
  candidate package early enough to test the installed chain. Package-dependent
  authorization/coexistence/restart evidence cannot come from injected unit tests.
- The server change set grew to30 commits. Task completion, source acceptance,
  installed files and running processes were kept separate, but release work did
  not move alongside implementation. A large set of green component tests still
  leaves a large integration surface.
- Required end-state consolidation was effectively treated as a prerequisite for
  every release. A client release can be evaluated separately. A non-retirement
  server release still needs its included behaviors accepted; it must not claim
  all of P0 or silently omit required parity from the final target.
- Historical diagnosis gaps, absent physical login and Windows/second-camera
  access need explicit disposition rather than indefinitely accumulating unrelated
  source work. Continue all independent work; ask for a user test only with the
  exact task/host/version/steps/pass criteria/evidence/unblock specified by the goal.

## Execution order after resume

### R1 — Protect wallet migration before launching candidates (T10)

**Observed code risk:** Client `LegacyMigration.cpp:129–131` and server
`FarsideMigration.cpp:144–146` write the source secret before checking the
destination. Client GUI and headless main paths both invoke this retry on startup;
legacy server main does too. An existing different destination entry can be
overwritten while marked pending. This is a code finding, not evidence of damage
to Steve's stores. No live wallet retry was exercised during this assessment.

**Instructions:**

- Make secret migration a deliberate transaction; diagnostic/startup paths must
  not blindly write or prompt for wallet access. Preserve source stores and exact
  profile IDs/secret keys. Never delete a source or overwrite a different existing
  destination. Matching existing entries can be verified as already copied;
  conflicting, locked, unavailable or uncertain entries stay pending with reasons.
- Read values only in memory, pass no secret in process arguments, logs, public
  manifests, screenshots or plaintext backup files. Report counts/reasons only.
  Do not restart/reconfigure/unlock/reset wallet services or change authentication.
- First use injected secret-store backends and private fixtures. Cover source
  unavailable, destination absent/same/different, wrong ID/owner, failed write,
  failed readback, interruption, concurrent change and safe rerun/rollback.
- Require create-if-absent protection or equivalent verified serialized backend
  semantics. Do not claim a read-then-write API is atomic. If safe creation cannot
  be guaranteed, leave the entry pending. Treat uncertain writes as uncertain;
  never delete a possibly independently changed entry during cleanup/rollback.
- Use an established consistent encrypted backup/export for any actual wallet
  mutation; do not copy a live wallet file and claim a valid backup, export
  plaintext, or disrupt Hal's wallet daemon. Originals plus metadata alone are
  sufficient for read-only inventory. Actual reconciliation follows the verified
  transaction, with no inference that Buzz's three unavailable entries migrated.

**Guard ready for candidate tests:** Both source paths cannot clobber an existing destination, all failure/
race cases preserve originals, diagnostics have no wallet side effects, and
injected-backend evidence demonstrates these guarantees. Any later real-store
operations require verified backup/preservation and counts-only evidence.
The real Buzz reconciliation stays PARTIAL until all3 have verified results or
specific user-owned actions. Finish the guard before any fresh candidate client
process is allowed to see real profile directories, including headless probes.

### R2 — Cut and gate a client candidate (T06/T09/T10/T27/T28)

**Instructions:** Freeze ef692be plus only wallet/release-gate fixes. Prepare
0.6.4 version/metainfo/README in the actual Farside paths, build a clean committed
package with diagnostic actions disabled, preserve apt holds and archive rollback
0.6.3 read-only. Simulate install on Sol/Buzz. Use private profiles first; test
actual packaged Console/Virtual default/custom ports, current-server behavior,
capability absence/mismatch, initial monitor request opt-in, multi-screen state,
resize/Fit when advertised, reconnect and existing windows/profile preservation.
No controls may claim unaccepted server capabilities work. Include current
candidate-server tests when needed for its advertised newer features.

**Done:** Exact package/hash/version/source and rollback are verified, available
live cases pass, remembered credentials/profiles/trust survive, and no server
restart is needed for client-only installation. Tag/push the release and deploy
accepted hosts, Hal last without closing its existing windows. If any included
feature still lacks its gate, name it and withhold that release or use a reviewed
dependency-complete release selection; do not silently redefine acceptance.

### R3 — Build and install-test a server candidate early (T04–T08/T27)

**Instructions:** Freeze d437511c plus only gate fixes. Build from a new isolated
clean checkout using `scripts/package-farside.sh`, pinned private KPipeWire9d6b08c
and bounded jobs; leave Hal's old build/dependencies/private KPipeWire untouched.
Audit hashes, ELF dependencies/private RUNPATH, two helper/policy paths, KCM
resources, PAM/units/presets/conffiles and stock-package path conflicts. Simulate
Sol/Buzz installs and archive the exact installed rollback packages/config
preservation manifest before a controlled Sol trial.

Check incoming connections and retained-desktop inventory before restart.
Install only the paired broker/worker package. Verify actual running versions,
not just dpkg. Preserve desktop namespaces and secrets; incompatible existing
workers cannot be ignored or forcibly discarded. If safe worker adoption is not
possible, record the exact scheduling/user-session gate. Trial failures restore
the known-good package/state and stop rollout. Hal is not a live test host.

**Done:** A concrete candidate and rollback artifacts exist; Sol/Buzz apt simulation
changes only intended packages; installed native dependencies/units/helpers/KCM
are verified, rollback demonstrated, and the installed authorization chain works:
password/cancel/denial → scoped save → explicit restart → actual allowed/denied
admission and runtime readback. Separate source protocol tests do not satisfy it.
An installed test candidate is not a production release verdict.

### R4 — Close the included native behavior matrix (T01–T08/T11/T12)

**Instructions:** Use that same candidate on Sol/Buzz. Finish normal/wrong-password/
cancel/restart Discover and second requester in Console and Virtual. Reproduce
remaining diagnostic gaps in isolated fixtures; do not manufacture failure in
Steve's live agent/cache. Test lock/output-release interleavings with independent
restore/locker checks, reconnect/takeover/worker loss, one/two-screen hardware and
software/AVC/chroma, owned resize/Fit and exact restore, Virtual wake/camera/device
namespace behavior, consent/audio and installed FreeRDP/Remmina. Quiet functional
tests and controlled load/fallback tests are separate; record unrelated CPU/GPU
load and make no unsupported benchmark claim.

**Done:** Every included feature has an exact host/mode/package pass or a named
remaining gate. Required native failures are fixed against this candidate and
retested only for their concrete risk. Historical trace gaps, a missing real
Console owner, Windows access and second-camera/human LED checks remain explicit;
they cannot be silently waived. No expanded NVIDIA/UX feature work during this gate.

### R5 — Finish consolidation migration and cut over (T10/T13/T27)

**Instructions:** Extend the accepted planner with safe canonical file identity,
private exact backups, complete stale revision checks, durable step journal,
atomic apply/readback, crash recovery and refusal to roll back over independent
edits. Integrate authorized host/admission, guarded real secrets, profile IDs/
layouts/custom endpoints and retained journals. Verify destination certificates
independently; never inherit trust from the old port. Dry run and apply must agree;
rerun must be a no-op and rollback must restore exact previous usable state.

Migrate Sol/Buzz first after their parity gates. Keep old :3389 and originals until
acceptance. Retire the redundant Farside route/code only after T02–T12 acceptance;
stock KRDP remains independent. Gate Intel/AMD as relevant, then remaining hosts,
Hal last with zero incoming RDP and preserved work processes/windows/settings.

**Done:** Every intended host/profile/credential has a verified result or named
specific exception, rollback works, exactly Console/Virtual Farside routes remain,
stock coexistence works and installed/running versions match delivery manifests.
P0 closes only when all its original acceptance and deployment gates pass.

### R6 — Complete the remaining original scope (T14–T26/T27/T28)

After P0 delivery, resume the original ordering and completion criteria: T14–T18
NVIDIA acceptance/device identity/selection/AVC/AV1; T20–T21 crash/startup/dialog
reliability; T22–T23 per-screen decode/HEVC tuning; T24–T25 Sessions/UX;
T26 controlled load/performance/visual acceptance. T19 is already delivered.
Every accepted slice needs matched UI/help, package/native/rollback evidence and
actual release records. This work is deferred in order, not removed from scope.

**Done:** Every T01–T28 task satisfies the governing plan or has solely a specific
documented user-owned gate with no independent work left. Full completion requires
the original acceptance conditions or an explicit Steve-approved scope revision.

## Operating rule and pause checkpoint

At resume, start with R1, then R2/R3 artifacts and installed gates. Limit new
source work to a demonstrated release blocker; do not alternate indefinitely
between implementation and documentation while never producing a candidate.
For each gate report task, frozen source/package, actual result, blocker/fix and
next concrete action. Keep source acceptance, installed files, activated runtime
and milestone delivery separate. Current remaining-work ledger and newest shared
handoff point here. Goal is PAUSED at Steve's request after this assessment.
