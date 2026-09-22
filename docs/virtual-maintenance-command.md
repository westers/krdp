# Virtual maintenance command (integration build only)

`krdp-virtual-maintenance` is built but not installed or enabled by this slice.
It is not yet a host-maintenance deployment procedure. Complete bootstrap,
approved writer routing and keeper admission are required before installation.

Only two operations exist, both requiring real, effective and saved UID 0:

- `status`: take the shared maintenance gate nonblocking and read a V2 record.
  Exit 0 means the diagnostic was read and printed, **not that admission is
  allowed**. JSON includes `phase`, `currentBoot`, `boot`, `generation`, `profile`
  and `admission: "not-evaluated"`. Blocked and old-boot records remain readable.
  Status takes no package locks, does not fsync, and makes no state change.
- `invalidate`: take the exclusive gate nonblocking and durably publish a fresh
  external-unknown generation. Exit 0 means durable invalidation; exit 1 means
  refusal/failure, and exit 2 means publication may have happened but durability
  is uncertain. A caller must not start a writer after either failure. Success
  alone does not approve an arbitrary writer or account for its lifetime.

Invalid arguments exit 64. Missing/unsafe/busy state is not repaired. If a valid
gate exists but the state file is absent, explicit invalidation can only produce
external-unknown, never first-provisioning authority. Malformed state is refused
and preserved. No alternate paths, commands, profiles, transaction overrides,
bootstrap retry or force-clean operation are accepted. There is no post-hook
that clears the maintenance state on successful process exit.

Tests use temporary ordinary-user fixtures. Do not run privileged commands from
an agent: isolated deployment remains a user-run operation in the existing host
tmux session, after the integration gates are complete.
