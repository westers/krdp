# Virtual-session maintenance guard

Status: user authorized maintenance-guard direction. Independent review approves
isolated storage/lease core only; production integration remains under review.

## Outcome and scope

Reject NEW virtual desktops during coordinated changes to the supported login
runtime. Keep existing virtual desktops, reconnect/list/stop operations, physical
console access and audio streams operating. Admission is not a cleanup proof:
the separate registration-completion protocol remains required for keeper-crash
recovery. This guard supplies a coherent maintenance boundary, not arbitrary
root-mutation detection.

## Protocol

An installer-owned root:root0700 directory `/var/lib/krdp/maintenance` contains
a stable root0600 single-link regular `lock` inode and bounded state record.
Never unlink/replace the lock during normal operation. Check pathname ancestry,
no symlink traversal, ownership/modes/types/link counts and lock inode association.
Missing/untrusted/malformed state denies admission; a missing directory is NOT an
implicit "feature disabled" success in production.

New-session admission takes a nonblocking shared flock on its own descriptor,
validates clean state with current boot and approved profile revision/digest,
and retains that descriptor from before PAM loading to registration cutoff.
Descriptors are CLOEXEC and must not escape to the desktop. The keeper is the
mandatory enforcement point. Broker create preflight supplies a friendly error
before consuming a session slot but cannot replace the keeper's held lease.

Maintenance acquires exclusive flock, waiting only for bounded in-flight startup
leases (never retained desktop lifetime). BEFORE any relevant writer runs it
durably publishes blocked state. Short write, rename/fsync failure or uncertain
publication aborts the maintenance operation. Death after publication leaves
blocked state; process/lock disappearance is never evidence of clean maintenance.

State publication uses a same-directory exclusive temporary file, exact bounded
versioned bytes, file fsync, atomic rename and directory fsync. Temporary files
are not admission records. A complete clean record describes a validated past
cutoff; it does not assert its producer returned successfully. Reboot requires a
new baseline: a persisted old-boot clean record cannot admit a new session.

Reopening admission requires exclusive lock plus a validator run UNDER that lock:
approved actual policy/code/topology, relevant writers quiesced, and package locks
covering the coherent baseline interval. Publish a fresh unpredictable epoch,
boot ID and profile digest only after validation. No CLI `--force-clean`, generic
successful exit, clean dpkg status or quiet timer substitutes for validation.
Failure/crash leaves admission blocked until deliberate validated recovery.

### Core API contract (approved isolated implementation scope)

Shared/exclusive leases are move-only owners of pinned directory and stable lock
descriptors. Every read/publication requires the appropriate held lease and checks
the named lock still matches its pinned inode. Clean and blocked records have
strict distinct schemas binding boot, epoch/transaction and profile where present.
Rearm must match the blocked transaction; a stale transaction cannot reopen it.
No public markClean convenience API: clean publication is private to a future
validator and test fixtures. Core callers cannot claim validation by passing a
digest. Missing/untrusted state never bootstraps automatically.

Admission must fsync the SAME validated clean file and directory before success,
including when a complete file survived its publisher's failed final sync. Any
failure denies admission. This does not manufacture writer quiescence; it only
resolves storage durability for an already validated historical record.

Step1 has no production caller, installed state, hooks or cleanup authority.
Tests use ordinary-user temporary directories via a private friend seam. Production
entry points enforce root ownership and fixed paths. Keep package locks separate
until the integration order and lifetime contract are implemented and tested.

## Maintenance integration

Default installed apt/dpkg pre-hooks must acquire the gate and durably invalidate
before returning; failure aborts that package operation. Post-hooks are not an
automatic re-enable: they do not prove detached workers stopped. Maintenance
wrapper/validator ownership must identify the transaction and all permitted writer
lifetimes; no naked arbitrary-command wrapper that clears state on exit zero.

Existing package frontend/backend lock ordering matters: a hook can run while apt
already holds frontend. Never take package locks while holding the exclusive gate
if a package hook can hold those locks while waiting for that gate. Establish one
order (package exclusion BEFORE gate for validation/startup) or fail nonblocking
and release partial acquisitions. Resolve this before production call sites.

Chosen acquisition order for startup/revalidation: package frontend shared,
backend shared, then gate shared/exclusive; nonblocking and complete unwind.
Wrapper execution cannot keep gate EX while calling apt whose hook reacquires EX.
It must durably invalidate then release that lease before the writer, retaining
separate authenticated transaction/lifetime ownership. Hook return/zero exit does
not finish a transaction. An environment-variable bypass is not authentication.
Outside-lock hooks and surviving workers prohibit rearm even when both package
locks become available. Exact ownership/quiescence protocol remains an integration
gate. The core's transaction comparison is necessary, not sufficient, for rearm.

Integration review requires an invalidation generation independent of the claim
that a caller belongs to a transaction. A coordinator captures transaction plus
generation before writing; unrelated/unclassified invalidation advances generation
and makes its pending rearm stale. Only hooks authenticated by exact coordinator
unit invocation and permitted process lineage may acknowledge the same transaction
idempotently. Root UID or a caller-supplied transaction UUID is not membership.
The core represents freshness in its blocked record; future integration must not
reset that freshness on status reads or reconstruct it from an environment flag.

A dedicated coordinator unit records boot/InvocationID and accounts for its writer
subtree separately from itself. Activated services and other permitted detached
work require explicit classification/lifetime tracking. Coordinator death leaves
blocking; a restarted coordinator cannot adopt/rearm the old transaction merely
because its main child exited. Generic direct apt remains conservatively blocked
after its pre-hook until explicit validated recovery; operations with relevant
pre-hook mutations must use the coordinator's earlier boundary.

The initial finite supported profile must classify installed hooks/services and
relevant package maintenance. Unclassified changes keep admission disabled.
Manual PAM/module/config maintenance uses the same pre-writer boundary. Ordinary
uncoordinated root edits cannot be made safe merely by inotify; they remain outside
the approved operating contract. Profile installation and recovery instructions
must say this explicitly.

## Implementation sequence / gates

1. Reviewed storage/lease core with temporary-directory subprocess tests; no
   production caller or auto-clean path until the full integration is validated.
2. Concrete baseline validator, package/guard lock order, supported hook/writer
   inventory, maintenance CLI and startup-only keeper lease; friendly broker gate.
3. Installer-owned state/bootstrap, conservative apt/dpkg hooks, maintenance
   recovery UI/status documentation and matching package updates.
4. Fault tests then user-run isolated Sol installation. Demonstrate new creates
   refused during maintenance while an existing unsaved desktop reconnects and
   retains audio; success re-admits only after validated baseline; crashed update
   remains blocked across broker restart. No Hal3389/physical3391 disruption.

Core tests: simultaneous shared leases, exclusive conflict, separate-process
contention/release, holder crash, missing/malformed/wrong-boot state, unsafe paths
and links, short writes/fsync faults, interrupted publication and stable lock
identity. Integration tests additionally cover package-lock inversion, hook error
abort, startup/maintenance race, stale rearm transaction, surviving writer and
unclassified profile. Whole-goal completion still requires live acceptance of the
registration proof and remaining physical/virtual/audio deliverables.
