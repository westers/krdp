# Consistent virtual-login lookup prerequisite

Status: implemented and reviewed; full server build and55/55 tests pass (40.91s).
Isolated code/tests only, no live deployment.

VirtualSessionLogin::read currently performs several independent calls to the
well-known logind name. Pin its unique owner and system-bus GetId, direct all
login/user object calls to that owner with activation disabled, and reject any
observed owner/bus-ID mismatch or disconnection during the lookup. Endpoint
checks do not prove uninterrupted well-known-name ownership or exclude release
and reacquisition between checks. Use one bounded
overall deadline rather than multiplying unrestricted per-call waits. Validate
exact reply types, session/leader/path consistency and user identity. Expose the
observed generation as diagnostic snapshot identity only; this is NOT durable
registration-completion evidence, an atomic snapshot of all mutable fields, or
authority to retire a failure on initial absence.

Qt's public reply API does not expose an independent sender identity here. This
implementation relies on correlated calls to pinned destinations and endpoint
checks; it does not claim an additional reply-sender validation layer.

Keep the existing read(pid) entry point. Provide a narrow explicit-bus/test-call
seam if needed to exercise the real parsing and generation guards without any
host bus mutation. Tests must cover stable lookup, owner replacement between
steps, bus ID replacement (including reused unique name), disconnected bus,
malformed/absent identity, wrong leader/path and bounded timeout. Avoid tests
that merely implement a copy of the algorithm. Existing recovery/dismissal and
PAM startup behavior must not acquire any new cleanup authority.

One combined independent review plus focused and full server tests before
commit/push. Preserve research.md and source archives. No deployment, new fault,
PAM/config change or retrospective journal marker. This prerequisite leaves
registration-peer association, durable publication, supported-stack enforcement
and the demonstrated emergency-cleanup gap open for the subsequent design.
