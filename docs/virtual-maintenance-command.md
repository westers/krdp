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

## Fixed unattended-upgrade boundary (build only)

`krdp-unattended-upgrade-guard` is also built without installation. Its only
backend is `/usr/libexec/krdp/unattended-upgrade.real`; no argument, environment
variable or PATH lookup can select a different backend. It requires real,
effective and saved UID 0, durably invalidates the maintenance state, releases
the gate, and calls `execv`. Backend argument boundaries and the environment are
preserved; argv[0] is the fixed backend path. It invokes no shell and never
rearms, including after a successful backend exit. Failed invalidation prevents
exec; failed exec leaves the external-unknown state intact.

This closes the **future-launch** boundary before upgrader plugin imports and
direct dpkg repair. It does not account for already-running descendants or
effects before the wrapper's own main function. Installing it requires reviewed
package-safe diversion/update/removal handling, exact backend identity, and
verification that the existing waiter's actual PATH and loader environment select
the wrapper safely. Those installation conditions are not implemented here.
There is no instruction to manually replace a system executable or restart the
running waiter at this checkpoint.
