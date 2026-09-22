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

Accept: explicit deadlock, stale rearm, owner death, surviving worker and profile
change tests. Review installed Sol hook inventory before integration approval.

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
