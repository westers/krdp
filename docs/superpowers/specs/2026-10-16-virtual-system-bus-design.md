# Virtual desktop system bus access (OPT-075)

Status: implemented on branch `virtual-system-bus`; not deployed, not exercised on a host.

## Problem

A Virtual desktop runs in a bwrap sandbox with `--tmpfs /run` (`scripts/launch-virtual-session.sh`). There is no
system D-Bus inside, so plasma-nm shows "disconnected" and Network settings will not open, and Discover, PackageKit,
polkit, UPower, Bluetooth and printer widgets fail. Diagnosis: `.work/virtual-kcm-notes/ANALYSIS.md`.

The sandbox also runs with `no_new_privs` and a one-uid user namespace (`1000 1000 1`). Neither changes here.

## Decision summary

One new server-wide setting family, in the existing `virtual-session.conf` scope ("New Virtual Desktops", the
`VirtualSession` scope of `BrokerHostSettings`, read by `farside-virtual-session@.service`). That scope already means
"applies to newly created desktops only" and needs no broker restart, which is exactly the contract wanted here.

| Key | Env (`FARSIDE_VIRTUAL_...`) | Default | Meaning |
|---|---|---|---|
| `VirtualSystemBus` | `SYSTEM_BUS` | `filtered` | `none` (today), `filtered` (xdg-dbus-proxy), `full` (real socket) |
| `SystemBusNetwork` | `SYSTEM_BUS_NETWORK` | `true` | NetworkManager, read-only |
| `SystemBusPower` | `SYSTEM_BUS_POWER` | `true` | UPower and logind queries, read-only |
| `SystemBusPackages` | `SYSTEM_BUS_PACKAGES` | `false` | PackageKit and fwupd (Discover) |
| `SystemBusPolkit` | `SYSTEM_BUS_POLKIT` | `false` | polkit name plus an authentication agent in the session. Experimental |
| `SystemBusBluetooth` | `SYSTEM_BUS_BLUETOOTH` | `false` | BlueZ |
| `SystemBusPrinting` | `SYSTEM_BUS_PRINTING` | `false` | CUPS helper plus the CUPS socket. Experimental |
| `SystemBusExtra` | `SYSTEM_BUS_EXTRA` | empty | Advanced: extra names to `--talk` to, comma separated |

`full` is never a default, in the code, the unit, the example conf or the KCM. The KCM shows a warning under it.
The toggles only matter for `filtered`; under `none` they are ignored, under `full` the filter is gone.

## Architecture

```
farside-virtual-session@.service  (root, EnvironmentFile /etc/farside/virtual-session.conf)
  -> farside-virtual-session-entry   resolves: global values + /etc/farside/virtual-session.d/<login>.conf
       VirtualSystemBus::resolve()   per-user override, per key, else the global value
       VirtualSystemBus::plan()      proxy installed? system socket present? -> effective mode + proxy rules
       VirtualSessionLaunchPlan      --system-bus MODE, --system-bus-rule R (repeated), --polkit-agent, --cups-socket
  -> device-entry (drops to the user, no_new_privs) -> guardian -> launch-virtual-session.sh (user)
       starts xdg-dbus-proxy OUTSIDE bwrap, as the session user
       bwrap ... --ro-bind <runtime>/system_bus_socket /run/dbus/system_bus_socket
```

* **Pure code** is `server/VirtualSystemBus.h` (header-only, unit-tested): names, parsing, per-user resolution, rule
  generation, the strict name grammar, the degrade decision. Validation of each value reuses
  `BrokerHostSettings::normalize()` so the KCM helper, the entry and the tests share one grammar.
* **The proxy runs as the session user**, started by the launcher before it `exec`s bwrap, never as root. It listens on
  `$XDG_RUNTIME_DIR/system_bus_socket` (the 0700 runtime directory the launcher already validates). The sandbox gets only
  that socket, bound at `/run/dbus/system_bus_socket`, plus `DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket`.
  The proxy connects to the real bus with the user's own credentials, so the system bus's own policy still applies on
  top of the filter.
* **Lifetime (no orphaned helper).** The proxy is started with `--fd=N` on a pipe whose read end the launcher holds.
  xdg-dbus-proxy writes one byte to it when ready (the launcher waits for that byte, 5 s) and stops when the read end is
  closed. The read end is inherited by bwrap and everything in the sandbox, so the proxy ends exactly when the last
  process of the desktop ends. A launch that fails before `exec` closes the fd and the proxy stops. The proxy is also a
  descendant of the desktop's dedicated system unit, so the root owner's existing scope teardown
  (`VirtualSessionServiceScope::signalDescendants` and `descendantsGone`) is the backstop. No new privilege is needed:
  the proxy needs no capability, only to connect to the system bus socket as the user.
* **Degradation.** `filtered` with no `/usr/bin/xdg-dbus-proxy`, or with no `/run/dbus/system_bus_socket`, becomes
  `none`: the entry logs one Info line (`Virtual system bus: filtered requested, xdg-dbus-proxy is not installed; this
  desktop has no system bus`) and the launcher repeats the check in the user context and writes the same line to
  `desktop.log`. A proxy that does not become ready in 5 s degrades the same way (the process is stopped).
  `full` with no socket also becomes `none`.
* **Applies to new sessions only.** The plan is computed at launch. Running desktops keep what they have until they are
  recreated. The KCM help says so.

## Rules (what the proxy allows)

xdg-dbus-proxy levels: `--see` makes a name visible, `--talk` allows every call, `--call=NAME=RULE` allows only the
listed methods (`METHOD@PATH`, METHOD = fully qualified method, `Interface.*` or `*`; PATH exact or `/*`),
`--broadcast=NAME=RULE` lets signals from NAME through. Calling a method on a name with a rule is allowed only if a rule
matches. The builder emits `--filter` first, then per enabled service:

* **Network (read-only):** `--see` and `--call` for `org.freedesktop.DBus.Properties.Get/GetAll`,
  `org.freedesktop.DBus.Introspectable.Introspect`, `org.freedesktop.NetworkManager.GetDevices/GetAllDevices/
  GetDeviceByIpIface`, `...Settings.ListConnections/GetConnectionByUuid`, `...Settings.Connection.GetSettings`
  (no secrets) and `...Device.Wireless.GetAllAccessPoints/GetAccessPoints`, each limited to the path
  `/org/freedesktop/NetworkManager` and its subtree; `--broadcast` for the same subtree so the tray receives
  `PropertiesChanged`. Not allowed: `Set`, `ActivateConnection`, `AddAndActivateConnection`, `DeactivateConnection`,
  `AddConnection`, `Update`, `Delete`, `RequestScan`, `CheckConnectivity`, `Enable`, `Sleep`, secrets agents.
* **Power (read-only):** UPower `Properties.Get/GetAll`, `EnumerateDevices`, `GetDisplayDevice`, `GetCriticalAction`;
  logind `Properties.Get/GetAll`, `Manager.GetSession/GetSessionByPID/GetSeat/GetUser/GetUserByPID/ListSessions/
  ListSeats/ListUsers` and the `Manager.Can*` queries. Not allowed: `Inhibit`, `PowerOff`, `Reboot`, `Suspend`,
  `Hibernate`, `SetXxx`, `Terminate*`, `Lock*`, `Activate*`.
* **Packages:** `--talk=org.freedesktop.PackageKit`, `--talk=org.freedesktop.fwupd`.
* **Polkit:** `--talk=org.freedesktop.PolicyKit1`, and the session starts `polkit-kde-authentication-agent-1`.
* **Bluetooth:** `--talk=org.bluez`.
* **Printing:** `--talk=org.opensuse.CupsPkHelper.Mechanism`, and `/run/cups/cups.sock` is bound into the sandbox
  (`--bind-try`), because the printer widgets mostly speak IPP on that socket.
* **Extra:** `--talk=NAME` for each name. Grammar per name: at least two dot-separated components, each
  `[A-Za-z_][A-Za-z0-9_-]*`, optional final `.*`; at most 16 names, 255 bytes each, no duplicates. The bus itself,
  `org.freedesktop.systemd1`, `org.freedesktop.machine1` and bare wildcards are refused. Anything else makes the whole
  setting invalid (it is not partly applied).

The same grammar is checked a second time in the launcher (`^--(see|talk|call|broadcast)=NAME(=RULE)?$`), so a corrupt
argument cannot reach the proxy command line or add a second option.

## Per-user override

The broker settings have no per-name mechanism (`BrokerUserSettings` is the account's own preference file, with a pinned
key set that deliberately excludes host policy). Options considered: (a) add system-bus keys to account preferences:
rejected, a user must not be able to widen their own sandbox; (b) a table inside `virtual-session.conf`: rejected,
the EnvironmentFile grammar and the settings helper edit one flat key map; (c) a root-owned drop-in per user.

Chosen: `/etc/farside/virtual-session.d/<login>.conf`, same `FARSIDE_VIRTUAL_SYSTEM_BUS*` keys, any subset. Read by the
root entry only. Rules:

* Precedence per key: drop-in value, else the global value. An absent file means no override.
* The login must match `^[a-z_][a-z0-9_.-]{0,31}$`; otherwise no lookup is done (global values apply).
* The directory and file must be root-owned, regular, not a symlink, not group/other writable, at most 64 KiB, and
  parse with the host settings grammar. Any other key in the file, or any invalid value, **fails closed**: that user
  gets `none` for the new desktop and the entry logs a warning. A broken override never widens access.
* The KCM does not edit it. It lists the users that have a drop-in (read-only summary with the file path and the mode)
  on the Virtual page, with a note that the files are edited by an administrator.

## KCM

Virtual page, "New Virtual Desktops" area: a "System services" section with one choice for the mode and a switch per
service; the extra list and the polkit/printing rows are under Advanced. Help text on every row, validation through
`BrokerHostSettings::normalize()` (the helper refuses invalid saves), "Takes effect for new desktops only" on the group.
When `/usr/bin/xdg-dbus-proxy` is missing the "Filtered" choice reads "Filtered (unavailable: xdg-dbus-proxy not
installed)", the service toggles are hidden with the reason, and the page says so. `full` shows a warning text.

## Packaging

`xdg-dbus-proxy` goes into `Recommends` of `farside-server` (not `Depends`): without it `filtered` degrades to `none`.
`scripts/package-farside.sh` asserts the Recommends line. The unit and `virtual-session.conf.example` ship the
documented defaults; `virtual-session.d/` is created by the package (empty, 0755 root).

## Open questions (with recommended answers; none block the work)

1. **Default `filtered`?** Recommend yes, with only Network and Power on. It changes behavior on upgrade (a desktop gets
   a read-only window onto two services). Alternative: ship `none` and let the admin opt in. Steve asked for `filtered`.
2. **Is `--see` + method rules enough for plasma-nm?** Unknown until tried on a real desktop. If plasma-nm needs an
   unlisted call, the symptom is a refused call in the proxy log; the fix is one more rule in the builder. Recommend
   verifying on ace first (checklist in the report).
3. **Polkit prompts in the namespace.** polkitd identifies the caller by pid/uid and session. Inside a one-uid user
   namespace with no logind session in the environment the subject may not resolve, so prompts can fail or be refused.
   Recommend shipping the toggle as experimental, off by default.
4. **Printing needs the CUPS socket**, which grants full IPP access as the user. Recommend off by default, labelled
   experimental. CUPS change notifications are unnamed bus signals and cannot be matched by name, so live queue updates
   may not arrive.
5. **snapd.** Discover's snap backend uses `/run/snapd.socket`, not D-Bus. Not covered. Recommend a later separate
   toggle if wanted.
6. **Per-user override edited from the KCM?** Recommend no (read-only) until a privileged helper action for it exists.
7. **CheckConnectivity.** Left out of the read-only set because it triggers a network probe. If the tray shows a stale
   connectivity state, add it.
