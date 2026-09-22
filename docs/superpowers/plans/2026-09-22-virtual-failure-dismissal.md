# Virtual-session failure dismissal implementation sequence

Binding design: `../specs/2026-09-22-virtual-failure-dismissal-design.md`.
All work is isolated from deployed services until combined review and user-run
installation. The final feature requires all slices, not just journal primitives.

1. Journal foundation: distinct dismissed outcome, writable broker-only instance
   API, reconciled prerequisite, exact safe marker validation, durability-establishing
   idempotent retry/read, companion enumeration, wrapped-fsync fault tests.
2. Dispatch lifetime prerequisite: reserve in-flight requests before mutation;
   prevent recursive execution, use stable transport objects/identity across hash
   mutation, and guard control/supervisor/host destruction. Copy callbacks before
   invoking them. Never reuse hash iterators or references after a callback.
   Review release/disconnected and stop traversal, not just create. Transport's
   `request` and control-record delivery lambda also need QPointer guards because
   a nested controller call can destroy the transport before bind/reply delivery.
   Regression tests must call through Control for same-ID recursion, insertion
   of another transport, removal/replacement of the issuing transport, and owner
   destruction. Include reentrant release callbacks and another owner's session.
3. Host/protocol: authenticated dismiss eligibility and acknowledgement callbacks,
   strict request/reply schemas, owner-only historical retry, durable retirement
   predicate shared by recovery/timer/create. Defer retirement while dispatch is
   active; do not infer that a timer is always outside nested event processing.
   Regression tests cover an uncertain write followed by all retirement entry
   points; valid ordered evidence stays independently sufficient.
4. Client: typed capability parsing, correlated action and debug hook, distinct
   confirmation, list refresh and old-server fallback. No unsupported setting is
   presented as usable. Build and run server/client suites, then combined review.
5. Commit/push reviewed sources; isolated Sol build/install via existing single
   tmux session and user privilege. Native Buzz acceptance removes only proven
   crash row, preserves old unproven failures and all journal history, verifies
   capacity reuse and broker-restart persistence, normal client config unchanged.

Baseline: existing Journal/Control/HostController tests passed 3/3 in 2.52s before
implementation. These tests do not yet cover dismissal or the new reentrancy
requirements. Review identified both dispatch-reference and fsync/readback gaps;
neither is considered resolved by merely writing this plan.
