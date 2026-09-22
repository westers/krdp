# Explicit dismissal of reconciled virtual-session failures

Status: implemented and reviewed; isolated native acceptance and deployment pending.

## Problem and scope

The four-session admission limit currently counts failed sessions indefinitely.
Sol's guardian-crash acceptance produced a failed row with trusted cleanup proof;
two older failures have no such proof. Only the first can safely be dismissed.
Failure is not evidence that applications, the PAM keeper, or logind are gone.
This change is owner acknowledgement of a terminal failure, not Stop, deletion,
relaunch, or proof of orderly exit. The immutable journal's 256-record hard cap
remains unchanged; journal compaction is outside this change.

## Authority and durable evidence

The authenticated nonroot PAM UID, never JSON identity, selects the owner.
Eligibility requires an owner-visible Failed row, matching immutable intent and
claim, current boot, and valid exact-record `.reconciled-UUID` evidence. Missing,
malformed, unsafe, mismatched or prior-boot evidence refuses the action. Historical
logs and PID absence cannot substitute. No markers may be manufactured for old
failures.

The root broker writes exclusive `.dismissed-UUID`, domain/version separated and
bound to every Record field using the existing outcome encoding. Require trusted
directory, no-follow, regular root-owned 0600 single-link file, bounded exact bytes,
file fsync and directory fsync. Preserve partial markers on failure. Dismissal
does not create `.ordered`, alter intent/claim, or authorize launch replay.

Retirement/recovery requires current-boot reconciled AND (ordered OR dismissed)
proofs, and still performs duplicate-identity preflight over ALL history. Invalid
alternative evidence must not override a separately valid ordered proof. Old-boot
records retain existing conservative behavior. Removal affects live registry and
admission only, never audit records or application/profile files.

## Protocol and execution ordering

Add strict v1 `action: dismiss` with canonical `session` UUID and existing request
correlation. List rows may include boolean `dismissible`; absent means false.
Only Failed rows may advertise true. Server revalidates ownership, state, exact
record and cleanup proof at mutation time; stale UI capability is not authority.
Unknown/wrong-owner targets use the same generic refusal, without metadata.

Successful reply includes matching session and `state: dismissed`, meaning durable
acknowledgement accepted, not synchronous registry removal. Persist proof first;
schedule existing reconciliation for the next event-loop turn. Never synchronously
call forgetReconciled from dispatch: its notification can revoke transports and
invalidate VirtualSessionControl's Transport reference before reply caching.
Existing QPointer guards alone are NOT sufficient: create already invokes
synchronous reconciliation while Control retains a transport reference. Protect
all dispatch paths with stable transport identity/lifetime and an in-flight
request reservation before invoking callbacks. Recursive same-ID requests must
not perform a second mutation. No reference into the transport hash may survive
a callback; host/control destruction must also be detected without accessing a
destroyed object. Defer retirement until the outermost control dispatch unwinds;
before-create reconciliation must observe the same dispatch guard. Test this
through Control with callback-driven disconnect, host destruction and reentry.

Identical request-ID retries retain existing cached-response semantics. A fresh
request for an already dismissed exact owner record may return the same successful
acknowledgement after validating both proofs, even if its live row is absent.
Resolve the immutable record and authorize its UID first. An absent live row
permits ONLY re-acknowledgement of an existing valid dismissal, never first-time
dismissal. It must not reveal another owner's historical record. Existing complete markers
must be revalidated and file/directory fsynced before success on retry, so a prior
fsync failure is not silently converted to a durability claim. Partial/corrupt
markers remain refused; never unlink-and-retry. An uncertain write reports an
uncertain outcome and requests refresh; no claim that nothing changed.
Cached uncertain replies remain unchanged for the same request ID; retry uses a
fresh ID. Every dismissal-based retirement path (timer, before-create and recovery)
must establish durability by validating and fsyncing the exact marker file and
directory, not merely reading complete bytes. A failed original fsync can leave
valid-looking bytes. No successful durability check means no dismissal-based
retirement. Enumeration recognizes the `.dismissed-` companion namespace, including
partial markers, without treating it as a launch intent or discarding audit data.

## Client

Validate optional capability as an actual JSON bool; true on a non-Failed row is
malformed. Old servers omit it and never enable dismissal. Add a distinct
"Dismiss failure" button and confirmation: cleanup verified, hide failure/free
slot, preserve audit history. Do not reuse End's warning about killing apps.
Use the typed session model and correlated replies for UI and debug action alike.
Refresh the list after acknowledgement; allow the row to remain briefly until
asynchronous reconciliation. No automatic attach/create, no client-side hiding
before server acknowledgement, and no credentials in logs.

## Verification and delivery gates

- Journal: exact record binding, wrong owner/boot/claim, missing/corrupt/truncated
  outcomes, symlink/hardlink/mode failures; idempotent durable retry and no removal
  of partial evidence. Use the existing wrapped-fsync fault seam for file and
  directory failure, followed by retry and retirement/recovery attempts.
- Controller: eligible failed row only; stale eligibility, wrong UID, live and
  stopping rows rejected; asynchronous retirement, callback destruction safety,
  reclaimed slot, restart exclusion, immutable history and 256 cap unchanged.
- Protocol/client: strict schemas, duplicate correlation, absent/invalid capability,
  matching response identity, old-server compatibility, confirmation/debug path.
- Full server/client test suites and independent combined review before deployment.
- Sol: update only isolated 3395 broker through user-run tmux command; native Buzz
  GUI dismisses the proven guardian-crash row, old unproven rows remain, fresh
  session can be created/stopped, and dismissed row stays absent after restart.
  Keep physical console3391, Hal3389, Buzz normal configuration and held libraries
  untouched. Preview build success is not production/package acceptance.
