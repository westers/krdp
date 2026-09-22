# Durable post-open registration completion

Status: design candidate; not approved for implementation or cleanup authority.

## Required outcome

The keeper-crash fixture9d6e/c11 lost all observed processes/scopes/login but
cleanup correctly refused initial absence without a close/completion proof.
For NEW successfully opened sessions, preserve enough durable evidence to accept
later authoritative absence without pretending PAM close ran. Interrupted startup,
unknown generations and old records remain conservative. No retrospective marker.

## Source argument now established conditionally

For one fixed-stack PAM invocation handled by the same continuously running
audited logind generation, registration calls are finite and sequential. Varlink
does not retry/fallback after submission failure. D-Bus permits one fallback on
UnknownMethod, not timeout/disconnection. Publication into sessions and
sessions_by_leader, including the launch tag, precedes session_start.

An UnknownMethod propagated from PID1 may follow allocation; it is NOT assumed
side-effect-free. Its handler queues the already-published session for GC before
returning the error. Fallback either encounters the existing leader and returns
busy, or allocates a replacement after collection and completes that request.
Neither GC nor later job callbacks allocates a new login. ListSessions includes
all hashmap entries, not only started sessions. Therefore complete conservative
post-open enumeration can account for surviving attempts in this generation.
Busy-success itself supplies no fresh PAM identity and cannot replace validation.

Anchors in exact patched systemd259.5: logind-session.c:89/254, logind-dbus.c:
939/997/1051/1249/1256/4686, logind-core.c:393, logind.c:1084,
pam_systemd.c:1122/1235/1254/1306, ListSessions logind-dbus.c:652.

## Candidate guarded-generation contract

1. Before PAM, establish current boot, system-bus GUID, unique logind owner and
   its root process pidfd/birth identity. Install a loss-detecting owner-change
   subscription with an acknowledged barrier before relying on it.
2. Independently monitor while PAM blocks. Any relevant owner transition,
   daemon death, bus disconnection, monitor failure or deadline expiry invalidates
   completion eligibility. Endpoint equality/quiet time are not substitutes.
3. After PAM success, validate returned metadata and exactly one launch-tagged
   login from complete enumeration through the pinned owner. Require exact
   keeper birth, UID, login ID, scope and supported runtime. Unreadable rows,
   duplicate tags and inconsistent snapshots refuse publication.
4. Complete an ordered monitor barrier that accounts for queued signal delivery;
   exclusive durable publication must precede Ready. Bind full immutable launch,
   keeper, login/scope, boot and guarded generation plus supported-policy identity.
   File and directory fsync failures forbid Ready. File-fsync failure forbids
   final publication; directory-fsync failure leaves publication durability
   uncertain, not the historical meaning of a complete surviving final record.
5. Recovery must still establish keeper/descendant/scope extinction and the
   recorded supported generation. Only then may complete enumeration finding
   neither the recorded login nor any matching launch candidate authorize
   reconciliation. Changed/uncertain generations remain unresolved.

The completion record is NOT ordered exit, PAM close, a cleanup result, client
consent, or authority to replay/delete. Existing dismissal still requires actual
reconciliation. Publication and retirement must preserve partial evidence.

## Transport association and supported installation

Do NOT infer Varlink server PID from a separate SO_PEERCRED probe. Logind can
inherit the listener from PID1; holding a separate connection is not pinning the
opaque PAM transport. Association needs the supported root-owned socket/service
topology and daemon lifetime, excluding overlapping workers/reexec/handoff.

Read-only Sol observations: systemd-logind-varlink.socket Accept=no, one listener
/run/systemd/io.systemd.Login, Service=systemd-logind.service; logind PID1489,
Type=notify-reload, KillMode=control-group, Restart=always, invocation
2f4dd1b1c819417fa373e851d6868274. Socket has no drop-ins; service's sole distro
dbus.conf adds executable-presence conditions. Unit files root0644.
These observations alone are not yet a runtime-enforced topology contract.

Fixed PAM/version/resource assumptions must be bound and enforced; unsupported
stacks cannot inherit emergency equivalence. User-record area directories can
exist even without an explicit selector and survive normal close; preserve them
and shared runtime/user managers. No arbitrary PAM/NSS or startup-helper claim.

## Design decisions still required before code

### Proposed bounded installation-admission profile

Bind login1 owner credentials (UID0) to a pinned live PID, exact MainPID,
GetUnitByPID and service ControlGroup, nonzero unchanged InvocationID, no pending
job/control process, and active/running service. The approved effective service
must have direct audited ExecStart (no wrapper), notify-reload,
BusName=org.freedesktop.login1, KillMode=control-group, Delegate=no and
SendSIGKILL=yes. Reject unreviewed helper commands and unexplained additional
processes in that cgroup; threads are not additional workers.

Require the approved socket unit active/listening with Accept=no, exact single
AF_UNIX stream address, descriptor name and service association; root-trusted
pathname ancestry and no proxy/helper topology. Match the live listener's kernel
socket identity to a descriptor held by the pinned daemon. Filesystem socket
inode is NOT that identity. PID1 retaining the activation descriptor is expected;
do not require sole ownership. These runtime checks supplement, not replace, the
audited daemon's no-reexec/no-worker-handoff lifecycle and non-hostile-root model.

A versioned profile identifies the bounded relevant code: running daemon image,
PAM library/fixed modules and relevant shared systemd implementation. Package
labels locate source provenance but do not identify running code. Pin descriptors
and require approved content identities; reject mixed/unidentifiable relevant
versions. Do not hash unrelated distro packages or reject unrelated upgrades.
Validate the complete effective PAM include graph and options, not just the
top-level file. A separate user-record preflight cannot prove what PAM consumed.
Correction to the initial proposal: although pam_systemd caches JSON under
systemd-user-record-<username>, application-side pam_get_data is rejected by
PAM1.7.0 (pam_data.c:125). Do not access private handle layouts or presume this
cache is an application API. The source argument below replaces that inspection
prerequisite for resource teardown only, not authorization-policy enforcement.

Main inspection and independent review of pam_systemd's full successful-open/
close paths support grouping every consumed record field: identity/disposition
select the logind registration (independently validate actual UID/tag/leader/
class); umask/nice/rlimits/capabilities affect the keeper process; environment,
email/timezone/languages affect PAM memory/settings; home/defaultArea/UID/GID
may create runtime Areas subdirectories which intentionally survive normal close.
The Wayland branch skips terminal OSC creation. Cached JSON, bus connections and
session FIFO are memory/FD resources released on process extinction. No additional
per-session external teardown requirement was found in these fields. User-wide
slice limits/runtime/linger/user-manager/IPC lifetime remain logind's shared-user
responsibility, not permission to remove resources belonging to other logins.

Anchors: pam_systemd.c:181/657/922/1280/1455/1545/1635/1756/1853;
logind-user.c:426/590. Record environment is applied AFTER registration and area
setup, so it cannot retroactively request extra device access. It can overwrite
returned PAM metadata: authoritative post-open matching remains mandatory.
This establishes normal-close resource equivalence for the audited fixed stack,
not restoration of every pre-open file/setting, reproduction of close logs,
arbitrary provider behavior, interrupted-startup equivalence or a loaded-code gate.

The global nonoverlap assumption is false: service_adverse_to_leftover_processes
(service.c:2469) refuses leftovers for SendSIGKILL=no, not yes. Instead require
the original nonzero service InvocationID on causally ordered pre-PAM and
post-enumeration reads. service_start renews it BEFORE startup commands (3024);
unit_acquire_invocation_id randomizes it (unit.c:5472). A failed replacement does
not restore the old ID. This detects even a brief intervening supported restart,
subject to the ordinary random-ID collision assumption. Pin PID1's unique bus
owner/credentials (host PID1) on the same observer connection; monitor its owner
changes as well as logind's. Reject observed PID1 Reloading during certification.
Record and require the PID1 endpoint during recovery. Reexec keeps PID/birth but
replaces its API bus connection (core/dbus.c:1053), so PID/birth alone is not enough.
Startup worker/listener exclusion and audited no-handoff lifecycle still apply.

The coherent loading contract for ordinary concurrent upgrades and PAM policy
edits remains unresolved. Pre/post hashes alone cannot exclude an intervening
pathname-loaded module change. Root ownership is not a solution to upgrade races.
Do not silently declare these ordinary operations hostile or unsupported merely
to obtain implementation approval. Recovery must conservatively reject changed
recorded generations/policy until an explicit compatibility argument exists.

Candidate loading route, NOT approved: pam_start_confdir on a private captured
approved policy graph, preserving control expressions/order/options/substacks,
and approved primary-module FDs retained through pam_end. Capture/validate `other`
as well; reject or rewrite absolute includes that bypass confdir. Accept only
complete approved file combinations, never arbitrary mixtures. Do not set
PAM_SERVICE afterwards (it invalidates cached handlers). Exact PAM1.7.0 calls
_pam_init_handlers before pam_start returns, caches handlers (pam_handlers.c:418),
and uses RTLD_NOW (pam_dynamic.c:37). A private account-only real-PAM experiment
(rdp/evidence/pam-config-cache-probe.cpp) confirms old handle retains permit after
its private config changes to deny; fresh handle denies. No login was opened.

This route is NOT complete dependency pinning: FD identity is not immutable bytes,
FD paths change derived PAM module names, constructors execute before post-load
inspection, and pam_start can succeed with faulty handlers. There is a concrete
late transient dlopen/dlclose of libnss_systemd in userdb.c:1979. A post-open maps
comparison alone cannot detect that. Resolve these boundaries before claiming
upgrade-safe supported-policy certification.

Superseded as a sole proof mechanism after cutoff review: approved supported-OS baseline
plus conservative loss-detecting mutation invalidation during registration,
instead of privately bundling libc/NSS. Install watches BEFORE baseline reads;
any relevant write/attribute/link/name change, overflow, ignored/lost watch,
unmount or monitor failure poisons the attempt. Never clear that latch after
hashing or after the files return to their old contents. Keep watches through
PAM return, enumeration and final event-queue drain. On invalidation, withhold
completion evidence; ordinary conservative cleanup still applies. This detects
change but does NOT stop changed constructors/dependencies executing first.

Before implementation specify an explicit footprint of policy/include files,
module/dependency search directories, loader cache/configuration, NSS/provider
configuration and approved code identities. Include parent/name and actual inode
watches as needed for aliases; directory watches alone do not prove complete
coverage. Watch setup races and unexpected subdirectories invalidate; recursion
must be deliberate. Supported package mutation tests must include replacement,
in-place writes, rename-away/back, transient insertion/removal, overflow and loss.
Remote provider behavior remains a supported-OS trust assumption, not verified
by inotify. Mount/overlay substitution and unobserved alias mutation need explicit
profile exclusion or stronger observation; do not silently claim immutable code.
Claim only approved baseline with no observed mutation/monitoring loss over the
closed interval, combined with the other registration proofs. Exact footprint,
kernel event-drain boundary and API/schema still require review.

### Cutoff correction: notifications are not writer exclusion

Independent review identified a concrete non-hostile race: Linux vfs_write
changes bytes before calling fsnotify_modify; a writer descheduled in between
can let another process observe the bytes and finish PAM before any notification
is queued. An EAGAIN drain certifies queue emptiness at its check, not absence of
earlier-visible mutations with delayed notifications. Async bus fences cannot
flush filesystem notifications. Main verified the upstream v7.0 write ordering:
https://github.com/torvalds/linux/blob/v7.0/fs/read_write.c#L627
This is a counterexample, not an audit of Ubuntu's complete patched kernel.

Do NOT grant completion authority from the inotify proposal alone. It remains
useful conservative detection. The certificate additionally needs supported-writer
exclusion or an acknowledged completion barrier covering already-running writers.
Candidate: shared POSIX fcntl locks against both dpkg frontend/backend exclusive
locks, held from baseline to combined certificate cutoff, with nonblocking refusal
if busy. Actual apt/dpkg maintainer-script/lock lifetime remains to audit; no host
locks have been acquired. Locks do not cover arbitrary editors/config managers,
mount replacement, aliases or mmap. Those need coordinated maintenance or an
immutable captured-policy/loading mechanism, not a claim that root ownership or
quiet time solves them. Preserve the delayed-notification case in the fault matrix.

### Sol loading-footprint observations (not yet admission enforcement)

Read-only 2026-09-22: x86_64/glibc2.43; passwd/group/shadow/gshadow NSS order is
files then systemd. No account contents or secrets were read. ld.so.conf includes
/etc/ld.so.conf.d/*.conf, whose search paths include /usr/local/lib, local and
system x86_64/i386/i686 multiarch directories, and x86_64 libfakeroot. Do not infer
an x86_64-only search footprint merely from the keeper executable architecture.
The x86_64 glibc-hwcaps directory and ld.so.preload were absent in this observation;
future creation must be accounted for, not silently ignored.

pam_systemd's ELF needed entries are pam_misc, pam, libm, libc and ld-linux;
there is NO separate libsystemd-shared DT_NEEDED entry in this build. Relevant
systemd behavior is compiled into the module. pam_unix additionally resolves
libcrypt/libselinux, with audit/cap-ng/pcre2 among transitive dependencies.
The effective `other` file includes common-auth/account/password/session, adding
loaded candidates pam_cap, pam_gnome_keyring and pam_umask beyond the called
account/session stack. PAM parses `other` with PAM_T_ANY and loads its handlers
even when the dedicated service defines the invoked stack (pam_handlers.c:519/747).
Loaded constructors and invoked session callbacks are separate coverage questions.
Observed logind remains PID1489/invocation2f4dd1b1c819417fa373e851d6868274; no
live configuration, package, permission or service changes were made.

### Proposed ordered observation protocol

One dedicated thread exclusively owns a private sd-bus connection. No Qt queued
callback, second D-Bus connection or blocking main-thread read participates in
the ordering argument. Install the exact login1 NameOwnerChanged subscription
asynchronously and dispatch its successful AddMatch reply before baseline reads.
Observe the bus GUID, unique owner, owner credentials, daemon pidfd identity and
service invocation on this connection. A relevant transition at any point after
subscription installation permanently poisons this attempt, even if the name
later returns to the same owner. Never reset the poisoned flag on a successful
read. PAM starts only after the complete baseline is acknowledged to the caller.

The monitor continues dispatching while the caller blocks in PAM. Post-open
enumeration and all endpoint checks are requests to this thread, directed to the
pinned unique owner with activation disabled. Complete enumeration means every
listed session is inspected: unreadable/nonconforming rows are uncertainty, not
nonmatches. The final fence is an asynchronous round trip to the bus daemon,
dispatched on this same stream. Its callback checks accumulated poison, deadline,
connection state and daemon lifetime before handing back an immutable result.
The implementation must validate the bus implementation's ordering guarantee;
the fence does not assert that other senders' unreceived messages were processed.
Daemon birth and source-level synchronous completion are separate requirements.
Disconnect, processing failure, resource exhaustion and timeout permanently fail
the attempt; no reconnection or retry can rehabilitate that registration.

Verified library-side ordering: patched libsystemd259 appends received messages
to the queue tail (bus-socket.c:1372) and dispatch_rqueue removes index zero
(sd-bus.c:2127). process_running dispatches that single message; process_reply
before process_match is per-message precedence, not a later-reply search
(2996/3052). Match callbacks execute inline. A synchronous sd_bus_call DOES scan
ahead for its reply (2442), so it is forbidden on the monitor connection after
subscription. Timeout callbacks precede queue dispatch (3040) and MUST fail,
never count as fences. AddMatch install callback runs before install_slot clears
(3522): return to normal dispatch before subsequent processing; no nested waits.
Retain match slot, use no consuming filters/competing handlers, and poison errors.

Executable evidence: rdp/evidence/registration-bus-order-probe.cpp launches only
a private dbus-daemon and two explicit-address sd-bus connections. Sixteen
acknowledged release/reacquire cycles accumulate with observer dispatch paused.
Async GetNameOwner sees all 32 transition callbacks before completing despite
identical final owner. Synchronous negative control sees zero callbacks at its
return, reproducing the unsafe pattern. This verifies the local implementation,
not every bus daemon, a production monitor, AddMatch rejection, timeout handling,
or topology/registration association.

### Proposed historical certificate and publication boundary

The certificate describes a CLOSED interval: successful baseline through the
dispatched final fence after successful PAM open and validated enumeration. It
does not assert continuity after that fence, Ready, successful storage sync, or
successful PAM close. Its immutable bytes may be constructed only after all
interval gates pass. A later owner transition can forbid Ready but cannot change
a fact about that already-completed interval. Recovery independently evaluates
current generation and resource extinction; the certificate alone authorizes
nothing. This distinction is necessary to avoid a commit-success-bit regress.

Proposed publication: create an exclusive root-only temporary file in the pinned
journal directory, write the complete bounded/versioned payload, fsync the file,
rename without replacement to the final certificate name, then fsync the
directory. Ready requires every step to succeed and a final live admission check.
Retain uncertain files; never overwrite, repair or backfill them. A failed file
fsync must not publish the final name. A failure after rename may leave a complete
final file. Such a file is still a truthful historical certificate, but is NOT
evidence that Ready happened or that the publishing call succeeded. A recovered
valid final file may contribute only that historical fact under the independent
recovery gates. Temporary files, missing files, partial bytes, extra fields,
wrong ownership/mode/link count and mismatched identities supply no proof.

Payload binds schema/policy revision, immutable launch intent identity (a
domain-separated digest of the canonical FULL intent, including its token,
without copying that secret into the certificate), keeper PID/startticks/pidfd inode, exact login ID and
scope, UID/runtime/tag, boot ID, bus GUID, unique owner, daemon PID/startticks/
pidfd inode, systemd service invocation and accepted installation-policy digest.
All strings and numbers have explicit bounds; parser rejects duplicate keys and
noncanonical representations. A checksum detects accidental payload damage;
root ownership and exact identity matching, not the checksum, supply authority.
The final API/schema and integration with journal listing still need review.
Journal enumeration must explicitly recognize the reserved temporary prefix and
final certificate filename, neither treating them as launch intents nor granting
authority from their presence. Certificate validation belongs to its typed reader.

Independent review approved the historical-publication argument in principle on
2026-09-22, with the durability distinction and full-intent digest above. This is
NOT approval of the complete recovery contract or implementation.

Required publication fault cases: kill before fence; transition queued before
fence callback; kill before/during write; short write; file-fsync failure;
rename collision/failure; kill after rename; directory-fsync failure; transition
after fence/before Ready; kill after Ready. In every case distinguish historical
registration completion, launch readiness, present resource extinction and
durable reconciliation. None implies another automatically.

### Recovery interval and bounded source invariant

Require the recorded boot, bus GUID, unique owner, daemon birth and supported
installation identity, then establish a fresh monitored cleanup interval.
Do not claim monitoring persisted between certificate publication and recovery.
Allowing that gap requires the audited invariant that the same surviving daemon
does not reload/recreate/reset session identities or allocate delayed attempts
after the certified registration completed. A changed daemon/bus or unverifiable
identity remains unresolved; do not substitute matching numeric PID/name.

Main inspection plus independent source audit of patched systemd259.5 supports
the bounded invariant for this executable: run() creates one Manager, calls
startup once and enters manager_run (logind.c:1377). Session import is in startup
(538/1247); SIGHUP only resets/parses configuration (1188; logind-core.c:37), not
session maps/counter. PID1 Reloading(false) queues existing sessions for GC
(logind-dbus.c:4514). Bus setup/name acquisition happens once (logind.c:785),
with no normal release/reacquire/reconnect path. Varlink server initialization
uses the existing Manager as inherited userdata (logind-varlink.c:339); accepting
a new connection does not replay a completed request. Runtime allocation is the
CreateSession path (logind-dbus.c:997); manager_dispatch_delayed (2060) handles
inhibited sleep/shutdown, not deferred registrations.

Limits: bare login IDs are NOT permanently unique in one daemon. Audit-derived
IDs can be reused once absent, and fallback cN has no explicit counter-overflow
guard (logind-dbus.c:850). Therefore exact launch/keeper identity and conservative
collision handling remain mandatory. PID/birth alone also does not universally
exclude exec; supported executable/topology validation cannot be omitted. This
audit establishes no cross-daemon equivalence, arbitrary administrative mutation
tolerance, or remaining-resource extinction.

- Specify exact topology/version/policy validation and daemon process association.
- Specify the monitor's ordered subscription/fence protocol, queue-overflow and
  failure behavior, and how recovery rejects generation uncertainty. Qt queued
  signal delivery plus an endpoint read is not automatically such a barrier.
- Specify publication API/schema and all pre/post-Ready failure boundaries.
- Review fault coverage: owner release/reacquire, bus replacement/reused owner,
  daemon death, queued transitions, registration fallback and duplicate objects,
  publication fsync failures, proof corruption, initial absence and old records.

Keep physical console3391/Hal3389 untouched. No new fault fixture or live install
before combined design/code review and tests. Existing9d6e remains failed with
history intact. This candidate does not complete the full project goal.
