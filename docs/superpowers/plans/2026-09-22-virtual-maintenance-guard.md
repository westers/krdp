# Virtual maintenance guard implementation plan

Binding design: ../specs/2026-09-22-virtual-maintenance-guard-design.md
Related full recovery design: ../specs/2026-09-22-virtual-registration-completion-design.md

User approved the maintenance direction; review approval covers the isolated core
only, not production integration. Do not equate a gate with a completed
registration proof. Existing failed-session history remains unchanged.

## 1. Durable gate core

Implement VirtualSessionMaintenanceGuard move-only shared/exclusive leases,
strict state parsing, fixed-path trusted production opening, test-only temp-path
seam, stable inode checks and durable atomic publication. No public clean setter;
rearm matches blocked transaction under future validator authority. Private
storage tests may seed state but no production bootstrap implicit success.

Accept: focused subprocess/failure tests, complete server build/tests, independent
combined review; commit only owned files. No deployment/call sites in this slice.

## 2. Maintenance transaction and approved baseline

Define coordinator transaction identity/lifetime and exclusive maintenance
boundary BEFORE running apt/dpkg or manual policy writers. Wrapper releases the
gate after durable invalidation so nested pre-hooks do not deadlock; retained
transaction ownership prevents premature rearm. Core clean publication remains
inaccessible to general CLI until this validator exists.

Acquire package frontend SH -> backend SH -> guard SH/EX nonblocking. Retain a
stable authoritative transaction record; reject stale owner/epoch/recovery after
interruption until writer quiescence and actual approved baseline are established.
Separate synchronous package actions from detached services. Only a finite
classified writer profile is admitted. No inotify/quiet-time substitution.

Implement compound startup admission first: move-only OFD shared package lease
retained by the guard admission lease; frontend/backend/gate order and complete
reverse unwind, stable trusted paths, unsupported OFD refusal. Gate-only
invalidation stays independent. Ordinary-user fixtures must prove actual POSIX
writer contention, lifetime/move/fork/exec behavior, alias-close safety,
replacement detection, and every partial-acquisition failure. This is not yet a
validator, transaction coordinator, installed hook, or production startup caller.

Accept: explicit deadlock, stale rearm, owner death, surviving worker and profile
change tests. Review installed Sol hook inventory before integration approval.

Next connected delivery (not another isolated helper): exact installer-owned
`sol-login-v1` footprint manifest, non-restarting coordinator with authenticated
invocation/transaction/generation, `validate-current`, broker compound preflight
and mandatory pre-NSS keeper lease. Initially validate the approved installed
baseline; do not accept arbitrary shell/apt operations or version floors. A
relevant package update requires an approved replacement profile. Unknown
surviving writers remain blocking, never cleared by coordinator exit or reboot
identity alone. Finite audit inputs: apt-news, esm-cache, PackageKit cache
notification, AppStream, command-not-found, debconf, ubuntu-virt and update-notifier
actual code/output/activation paths. Classify detached irrelevant work from
source, or require exact invocation completion/subtree extinction and excluded
reactivation for relevant work. Do not stop unrelated services/desktops to make
this pass. Connect and test approved baseline -> clean -> block -> create refusal
with retained desktop -> revalidation -> new create, plus crash/survivor/profile
mismatch/stale-generation refusal. This does not itself complete registration
certificates or keeper-crash cleanup acceptance.

## 3. Create-only production enforcement

Preparatory slice (before step 2 is complete): add an injected broker admission
callback and a typed creation result with a stable maintenance refusal. Test the
control boundary and lifetime/reentrancy behavior without accessing the guard or
enabling enforcement. The existing startup path remains unchanged until the
validator, bootstrap and mandatory keeper lease are ready together. A missing
callback in this temporary unwired stage is not the future production policy.

Broker createIndependent preflight before reconciliation/intent/quota writes;
typed control refusal for maintenance. Independent keeper gate before account
lookup/NSS/policy reads, retained to registration certificate cutoff. Check
already-loaded executable dependencies in baseline validation.

Do not gate list/attach/detach/stop/recovery or close of an existing PAM session.
Do not install before valid bootstrap/transaction handling exists. Test broker
zero side effects on refusal and existing-session controls while creates blocked.

## 4. Hooks, staging and operator workflow

Preparatory command is built (not installed): `krdp-virtual-maintenance
status|invalidate`, root-only, fixed paths, no rearm command. Status is gate-only
SH and expressly does not evaluate admission; invalidation succeeds only after
durable V2 ExternalUnknown publication. The provisioning receipt reader matches
the six immutable initial fields, verifies exact trusted storage and renews
file/directory durability; it neither creates state nor proves installer history.
These pieces remain prerequisites, not completed coordinator deployment.

Root-only helper, conservative apt/dpkg pre-hooks, explicit maintenance status and
revalidation commands, installer-owned root0700 state. Generic post-hooks do not
rearm. Document interruption behavior, supported maintenance boundary and manual
editing requirements. Keep matching server/client packages current at delivery.

## 5. Isolated acceptance then registration-proof integration

User-run privileged commands in existing Sol tmux only. Keep physical console and
Hal3389 untouched. Existing disposable desktop with unsaved content/audio must
survive guard transitions and reconnect; blocked create consumes no new intent;
successful revalidation admits a fresh session; crash stays blocked after restart.
Only after durable registration proof/code tests are ready repeat keeper-crash
acceptance on a NEW disposable session. Do not backfill existing failed records.

## Current connected-work boundary

The fixed bootstrap sequence is drafted and compiled as object code only. It is
not a runnable or installed helper: the writer-policy runtime gate still refuses
unconditionally until the finite effective execution/activation checks exist.
Private-bus tests currently cover typed systemd command/metadata decoding, not
writer quiescence, loaded-context approval, or bootstrap acceptance.

The fixed RuntimeProfile policy-byte reader and CoordinatorIdentity whole-inventory
reader are implemented. The latter retains raw dictionaries, uses GetUnit without
loading units, shares one deadline, and explicitly reads hidden
PermissionsStartOnly. The comparison-only implementation uses a compiled 321-entry
typed property table and canonical approved bytes, with private parser/comparator
tests and an independent source-checked descriptor oracle. It still returns
refusal from public validate after a complete match: selected
effective properties matching approved bytes is not path binding, guarded future
activation, quiescence, or shutdown-waiter provenance. No generic JSON commands,
queries, type declarations or approval-generation operation are permitted.

Comparison rules: required selected fields must exist at exact wire types;
duplicate dictionary keys refuse; unrelated exported properties may remain.
Only explicitly designated sets and command flags are normalized; other arrays
retain order. Condition/Assert runtime result must be -1/0/1 before discarding it;
command volatile timestamps/PID/results are decoded exactly but not configuration.
Public refusal after a matching fixture is exercised through the actual public
method, a private manager bus, and a temporary profile declaring the policy bytes.
An exact match produces the distinct runtimeChecksIncomplete refusal, not Clean.

The live Sol shutdown waiter is an active conditional upgrade launcher. Its
byte-matched 2.12ubuntu9 script can spawn `unattended-upgrade` through its existing
PATH/environment. The local upgrader loads plugins before package locks and may
perform direct dpkg repair, so APT pre-invoke alone is not its first boundary.
The fixed executable wrapper is now built and fixture-tested: it invalidates
before execing the real upgrader and never rearms. It is not installed.
Installation must validate the
running waiter's actual resolution/environment and package-safe diversion
lifecycle; an environment drop-in cannot change an existing Python process.
No service stop/restart or timer disabling is authorized as a testing shortcut.

## Authorized Sol first-install restart (2026-09-22)

Steve approved a one-time restart of only `unattended-upgrades.service` during
guard installation. No RDP, KDE, SDDM or unrelated service restart is included.
The previous launcher diagnostic is complete; do not ask him to run it again.
The installation remains user-run in the existing `krdp-sol` tmux session,
with the exact reviewed command staged without Enter. Approval does not make
the existing validation-only coordinator an installer.

Required implementation order:

1. Implement an explicit, durably blocked, known-origin first-install transaction
   and authenticate its finite installation actions. Bind boot, installation,
   transaction, generation and installer invocation. Unrelated invalidation must
   invalidate completion. Never restore ExternalUnknown to InitialBlocked or
   accept an environment-variable bypass.
2. Prepare and verify package-safe executable diversion, the fixed backend and
   approved startup inputs before stopping the waiter. Respect package-before-
   gate ordering and release the gate before any package action whose hook
   reacquires it. Do not deploy a partial gate or break unattended updates while
   the full supported routing/bootstrap/keeper integration is unfinished.
3. Check shutdown preparation, active upgrades and relevant jobs immediately
   before the stop. Stop only the waiter, then check completed stop job, old
   PID/birth disappearance and relevant subtree extinction. A concurrent
   shutdown, unexpected child or timeout aborts progress; no automatic kill
   escalation. A naked `systemctl restart` cannot perform these intermediate
   checks. The script's SIGTERM path can stop an already-running upgrade.
4. Start with approved fixed executable resolution and Python startup/import
   inputs; record the fresh invocation, PID/birth, interpreter and profile
   association. Recheck expected waiter state and absence of upgrade children.
   Initial `/proc/environ` alone is not fresh-launch provenance.
5. Validate the complete writer/activation/profile boundary and publish Clean
   only for the exact authenticated transaction/generation. Interrupted or
   unexplained activity stays blocked; report whether the updater helper was
   left stopped or running. Do not silently restore an unguarded route.

Tests must cover an upgrade/shutdown racing the stop, surviving descendants,
failed start, interrupted diversion, nested hook lock order, unrelated
invalidation and stale completion. Disposable fixtures first; no fault injection
against Sol's existing desktops. The present WriterPolicy still refuses after
comparison; no executable installation command is ready at this checkpoint.

Next implementation target is one fixed installation transaction and checked
sequence, not a generic installer framework: `VirtualSessionMaintenanceInstallation`
with durable phases, Guard storage integration and failure-boundary fixtures.
Its installation-complete evidence must be distinct from admission/Clean. Review
the exact phase schema and crash/recovery behavior before adding a live executor.
Preparation before stop and publication while stopped must be distinguished:
the exact diversion cutover must exclude concurrent launch; a stopped waiter
alone does not exclude other package/activation paths. Likewise, preflight
snapshots alone do not exclude an upgrade racing SIGTERM. The sequencing contract
must provide exclusion or refuse, not promise safety from repeated observations.
