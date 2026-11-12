# Unit hardening of the two root brokers (REVIEW-S2)

Scope: `server/krdp-console-host.service.in` (installed as `farside-console-host.service`, root, 0.0.0.0:3391),
`server/krdp-virtual-host.service.in` (`farside-virtual-host.service`, root, :3395) and the retained-desktop template
`server/krdp-virtual-session@.service.in`. Both brokers run the FreeRDP server stack as root before the client has
authenticated, and PAM runs in-process (`src/RdpConnection.cpp:678` `pam_start("login")`, `:688` `pam_authenticate`,
`:695` `pam_acct_mgmt`), so passwords and the TLS key sit in broker memory.

**Nothing here was run against a live unit.** The directives are justified from the code and checked offline
(`systemd-analyze verify`, `systemd-analyze security --offline=yes` on the generated files). Section 8 is the live checklist for Sol.
Hal is never a target for this change before Sol has passed it.

Result in one line: both brokers get seccomp/privilege directives and a capability **deny-list**; only the virtual broker (which
starts no child processes) also gets the mount-namespace directives; neither gets an allow-list, device policy, W^X or a syscall filter.
Both stop leaving core files (the console host via `PR_SET_DUMPABLE`, the virtual host also via `LimitCORE=0`).
Offline exposure score: console host 9.4 -> 6.8, virtual host 9.4 -> 5.8.

## 1. The key constraint: the console worker is a child of the console host

`ConsoleWorkerLauncher::startProcess` (`server/ConsoleWorkerLauncher.cpp:380`) forks the capture worker with `QProcess`,
then `setgroups`/`setgid`/`setuid` to the logged-in user (or the greeter user) and execs `farside-console-worker`. The worker is
therefore in the host's cgroup and inherits, across fork and exec: seccomp filters, `NoNewPrivs`, the capability bounding set,
rlimits (`RLIMIT_CORE`!), `umask`, the mount namespace, the UTS namespace and the device-cgroup policy. (The dumpable flag is
**not** inherited: `execve` resets it.) Every directive on the console host is judged by what it does to a user-uid desktop
capture process that cannot be tested here. The virtual host starts no child (it asks systemd to start
`farside-virtual-session@<id>.service` over D-Bus, `server/VirtualSessionHostController.cpp:244`, and `host(&server, {})` in
`server/virtualhost.cpp` has no process factory), so none of this applies to it.

## 2. Privileged operations and written paths

### Console host (`consolehost.cpp`, links `ConsoleHostController`, `ConsoleWorkerLauncher`, `PhysicalInputWatcher`, libKRdp, libudev)

| Operation | Evidence |
|---|---|
| Must be uid 0; reads `/etc/farside/authentication.json` (read-only, `O_NOFOLLOW`) | `server/consolehost.cpp:93`, `server/BrokerAuthentication.cpp:155` |
| Creates/renews TLS cert + key (`QSaveFile` in the same directory, explicit modes, `chown`/`chmod` of the directory), default `/etc/farside/console.{crt,key}`, **configurable** | `consolehost.cpp:134,206`, `server/ServerCertificate.cpp:117,350-404` |
| Listens TCP 0.0.0.0:3391 (FreeRDP server) | `consolehost.cpp:198` |
| PAM `login` in-process as root (pam_unix reads shadow, faillock writes `/run/faillock`, audit netlink) | `src/RdpConnection.cpp:678-695` |
| Reads a user's `~/.config/farsideserverrc` with `setfsuid`/`setfsgid` | `server/UserConfiguration.cpp:67-74` |
| System bus: logind `ListSessions`, `GetAll` on session objects, `PropertiesChanged` | `server/ConsoleSeat.cpp:96,107,133` |
| Opens `/dev/input/event*` of seat0 read-only, udev netlink monitor | `server/PhysicalInputWatcher.cpp:44,71` |
| Reads other users' `/proc/<pid>/environ` and `/proc/<pid>/cgroup`; `connect()` to the user's Wayland socket | `server/ConsoleWorkerLauncher.cpp:37,64,113` (needs CAP_SYS_PTRACE: `PTRACE_MODE_READ_FSCREDS` on another uid) |
| `getpwuid`, group list, then `setgroups`/`setgid`/`setuid` in the child | `ConsoleWorkerLauncher.cpp:329,380` |
| Signals the worker (`kill`) | `ConsoleWorkerLauncher.cpp:194` (CAP_KILL across uids) |
| Creates `<runtime>/<id>` 0700 and `chown`s it to the worker's uid; removes it afterwards | `server/ConsoleHostController.cpp:894-895,765` |
| Worker socket in that directory, mode 0777 (`WorldAccessOption`) | `server/ConsoleWorkerEndpoint.cpp:77` |
| Writes the world-readable public snapshot `/var/lib/farside-public` (0755, atomic rename); ExecStartPre `farside-host-settings-helper --publish console` does the same | `server/BrokerHostPublicSnapshot.cpp:121`, `consolehost.cpp:195` |
| Encoder probe opens `/dev/dri` render nodes (VAAPI trial encodes) and may load Mesa/NVIDIA userspace drivers | `consolehost.cpp:181`, `src/EncoderSupport.cpp:91-99` |
| `RuntimeDirectory=farside-console` (0711), watchdog pings over `NOTIFY_SOCKET` | unit file |

Worker, as the **user**, in the same cgroup: Wayland/D-Bus (AF_UNIX), PipeWire, DRM render nodes, v4l2 loopback, `kscreen-doctor`
(only helper it execs: `server/consoleworker.cpp:1529` and four more `QProcess` sites, none setuid), reads whatever files the
user copied as `text/uri-list` (`server/KSystemClipboardBackend.h:32`), and writes staged copies with explicit 0700/0600 modes
(`server/HostFileStaging.h:405,449,609,732`).

### Virtual host (`virtualhost.cpp`, links `VirtualSessionHostController`, `VirtualSessionSupervisor`, `VirtualSessionJournal`, libKRdp)

| Operation | Evidence |
|---|---|
| Must be uid 0; same authentication policy, certificate, listener (:3395), PAM and `setfsuid` reads as above | `server/virtualhost.cpp:71,108,176,172` |
| Journal of retained desktops under `/var/lib/farside/virtual-sessions` (`openat` with `O_NOFOLLOW`, `renameat2`, `fsync`); unit has `StateDirectory=farside/virtual-sessions farside/virtual-host`, `HOME=/var/lib/farside/virtual-host` | `server/VirtualSessionJournal.cpp:117,452`, unit file |
| Connects (as root) to each desktop's guardian/worker sockets under `/run/user/<uid>/farside-virtual/<launch>/` | `VirtualSessionJournal.cpp:104-112` |
| Starts and queries `farside-virtual-session@<id>.service` over the system bus (`StartUnit`, `GetUnit`, `ActiveState`) | `VirtualSessionHostController.cpp:244,308` |
| Reads `/proc/sys/kernel/random/boot_id` (read-only use) | `VirtualSessionHostController.cpp:117,197` |
| Public snapshot `/var/lib/farside-public`, encoder probe on `/dev/dri` | `virtualhost.cpp:164,150` |

The retained desktop (`farside-virtual-session@.service`, a **separate** unit and cgroup) does the heavy privileged work:
`unshare(CLONE_NEWNS)` + tmpfs `/dev` (`server/virtualdeviceentry.cpp:129`), `bwrap` (`scripts/launch-virtual-session.sh:14`),
PAM keeper, setuid to the user, cgroup reads. It is **not** sandboxed here (section 6).

## 3. Directive table

`C` = console host, `V` = virtual host, `S` = `farside-virtual-session@`. Rollback is a drop-in
(`systemctl edit <unit>`, section `[Service]`); remember `systemctl restart` (never while a client is connected).

| Directive | C | V | Decision and evidence | Rollback line |
|---|---|---|---|---|
| `NoNewPrivileges=yes` | ADDED | ADDED | Brokers are root, so a setuid-root exec gains nothing it lacks (`unix_chkpwd` is run by root, or not at all because pam_unix reads shadow directly as root). The only programs started below the host are the non-setuid worker and `kscreen-doctor` (`consoleworker.cpp:1529`). The worker inheriting the flag blocks setuid-root escalation from a compromised worker. Unprovable: a site-specific PAM module that execs a setuid helper from a non-root context (none can run as non-root here: PAM is in the root process). | `NoNewPrivileges=no` |
| `LockPersonality=yes` | ADDED | ADDED | Nothing calls `personality(2)`; the worker is native. | `LockPersonality=no` |
| `RestrictRealtime=yes` | ADDED | ADDED | No direct `sched_setscheduler`/`SCHED_FIFO` in `src/` or `server/` (grep). The worker's PipeWire threads (`src/PipeWireMicrophone.cpp:169` etc.) get realtime, if at all, through rtkit over D-Bus, which this filter does not touch; the worker has `RLIMIT_RTPRIO` 0 anyway (no pam_limits for a broker-spawned process). | `RestrictRealtime=no` |
| `RestrictSUIDSGID=yes` | ADDED | ADDED | No code creates setuid/setgid files: modes in `ServerCertificate.cpp`, `HostFileStaging.h`, `ConsoleHostController.cpp:894`, `VirtualSessionBrokerLease.h:41` are 0600/0644/0700/0755. `unix_chkpwd` is executed, not created. | `RestrictSUIDSGID=no` |
| `RestrictNamespaces=yes` | ADDED | ADDED | No `unshare`/`setns`/`CLONE_NEW*` in the brokers, the console worker or libKRdp. The only users are `virtualdeviceentry.cpp:129` and `bwrap` in `launch-virtual-session.sh`, both started by the **separate** session unit (S), never from a broker's cgroup. | `RestrictNamespaces=` (empty resets) |
| `SystemCallArchitectures=native` | ADDED | ADDED | Everything is x86-64/native; the worker never runs 32-bit code. | `SystemCallArchitectures=` |
| `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK` | ADDED | ADDED | AF_UNIX: D-Bus, worker/guardian sockets, `sd_notify`, signal self-pipe. AF_INET/6: RDP. AF_NETLINK: libudev monitor (`PhysicalInputWatcher.cpp:71`), `getifaddrs`, libaudit from PAM. Nothing opens AF_PACKET, AF_ALG, AF_VSOCK, AF_BLUETOOTH. Not provable: a PAM module that talks to a directory service over another family (none known; AF_INET/UNIX cover LDAP/Kerberos/SSSD). | `RestrictAddressFamilies=` (empty resets) |
| `CapabilityBoundingSet=~CAP_SYS_MODULE CAP_SYS_RAWIO CAP_SYS_BOOT CAP_SYS_TIME CAP_SYS_PACCT CAP_SYS_TTY_CONFIG CAP_LINUX_IMMUTABLE CAP_MKNOD CAP_WAKE_ALARM CAP_BLOCK_SUSPEND CAP_SYSLOG CAP_BPF CAP_PERFMON CAP_MAC_ADMIN CAP_MAC_OVERRIDE` | ADDED (deny-list) | ADDED (deny-list) | Each dropped capability is one nothing uses: no module load, `iopl`/`ioperm`, reboot, clock set (`adjtimex`, `settimeofday`), accounting, VT config, `chattr`, `mknod` (sockets are `bind`, devices are not created), alarms/suspend blocking, kmsg, BPF/perf, MAC policy (grep over `src/` and `server/` for these calls is empty; the dropped set is also what `ProtectKernelModules`, `ProtectKernelLogs`, `ProtectClock` would drop, without their mount namespace/device policy). **Allow-list NOT added**: the code needs CAP_SETUID/SETGID (`ConsoleWorkerLauncher.cpp:380`, `UserConfiguration.cpp:67`), CAP_CHOWN (`ConsoleHostController.cpp:895`, `ServerCertificate.cpp:354-404`), CAP_KILL (`:194`), CAP_SYS_PTRACE (`:37`), CAP_DAC_OVERRIDE/READ_SEARCH (evdev, user sockets, shadow), CAP_AUDIT_WRITE (PAM), and PAM stacks and GPU drivers (NVIDIA, Mesa) are site specific, so a tight set cannot be proven. `CAP_SYS_ADMIN` is deliberately kept. The bounding set is inherited by the console worker, but a process that has done `setuid(user)` has no effective capabilities to lose. | `CapabilityBoundingSet=~` (resets to full) |
| `ProtectSystem=full` + `ReadWritePaths=-/etc/farside` | NOT ADDED | ADDED | V writes only `/etc/farside` (certificate), `/var/lib/...` and `/run/...` (nothing under `/usr`, `/boot`, `/efi`; `full` leaves `/var` and `/run` writable). `-` tolerates a missing directory. Known limit: a certificate path configured under `/etc` but outside `/etc/farside` can no longer be created or renewed. C: the worker shares the mount namespace; with no capability allow-list a root exploit could remount anyway, so the benefit is small and not worth a live-only risk. | `ProtectSystem=no` |
| `ProtectKernelTunables`, `ProtectKernelLogs`, `ProtectControlGroups` | NOT ADDED | ADDED | V: only reads `/proc/sys/kernel/random/boot_id` and `/proc/self/uid_map`; no sysfs/cgroup/kmsg access in the broker (cgroup helpers `VirtualSessionServiceScope.cpp` belong to the session-cleanup binary, not linked into the broker: `server/CMakeLists.txt:79`). C: mount namespace shared with the worker; the same capability effect is already covered by the deny-list. | `...=no` |
| `ProtectKernelModules=yes` | NOT ADDED | ADDED | As above; also hides `/usr/lib/modules`. Kernel autoload on behalf of a process still works. | `ProtectKernelModules=no` |
| `PrivateTmp=yes` | NOT ADDED | ADDED | V: no `/tmp` use (no `QTemporaryFile`/`mkstemp`/`/tmp` in broker code; `QSaveFile` writes next to its target). C: **would break** host-to-client file copy of anything under `/tmp` or `/var/tmp`: the worker reads the paths the user copied as `text/uri-list` (`KSystemClipboardBackend.h:32`) and would see a private empty `/tmp`. | `PrivateTmp=no` |
| `LimitCORE=0` | NOT ADDED | ADDED | C: the worker inherits the limit; worker cores (Buzz abort analysis, `docs/issues/`) must stay, so C uses `PR_SET_DUMPABLE` instead (section 5). V: no child; the ExecStartPre helper already sets its own. | `LimitCORE=infinity` |
| `UMask=0077` | NOT ADDED | already present | C: inherited by the worker, which creates the user's files (staging, journals). The brokers set every mode explicitly, so C gains nothing. | n/a |
| `ProtectClock=yes` | NOT ADDED | NOT ADDED | Implies `DeviceAllow=char-rtc r`, which switches the unit's device cgroup to a closed policy and would risk `/dev/dri` (encoder probe, worker VAAPI), `/dev/input` (`PhysicalInputWatcher`) and v4l2 loopback. The clock capabilities are dropped through the deny-list instead. | n/a |
| `ProtectHostname=yes` | NOT ADDED | NOT ADDED | No code calls `sethostname`; it would freeze a copy of the hostname (a rename after start would not reach the broker or worker). | n/a |
| `PrivateDevices`, `DevicePolicy`, `DeviceAllow` | NOT ADDED | NOT ADDED | Render nodes (`EncoderSupport.cpp:91`), evdev (`PhysicalInputWatcher.cpp:44`), camera loopback, the GPU for the worker. | n/a |
| `MemoryDenyWriteExecute=yes` | NOT ADDED | NOT ADDED | Both mains run `EncoderSupport::probe()` (`consolehost.cpp:181`, `virtualhost.cpp:150`), which loads Mesa (LLVM/ACO) and NVIDIA userspace via FFmpeg/libva; these map writable+executable memory. The worker inherits it. | n/a |
| `SystemCallFilter=` | NOT ADDED | NOT ADDED | Needs a strace inventory of the broker, FreeRDP, PAM, libva and the worker on Sol first. | n/a |
| `ProtectHome`, `ProtectProc`, `ProcSubset` | NOT ADDED | NOT ADDED | The launcher reads other users' `/proc/<pid>/environ` and `/proc/<pid>/cgroup` (`ConsoleWorkerLauncher.cpp:37,64`); the worker reads user files. | n/a |

Not touched on purpose: `krdp-console-host-test.service` (isolated `/opt` test package, built only by the packaging script),
`app-org.kde.krdpserver.service.in` (a user unit, not a root listener), the presets.

## 4. Session unit `farside-virtual-session@.service` (S)

Only `TasksMax=100000` was added. A desktop with a browser and an IDE holds a few tens of thousands of threads; the default is
15% of `pid_max` (about 629145 here), so the cap exists to stop a runaway fork loop inside one desktop from exhausting the host,
and fork then fails with EAGAIN instead of anything being killed. **Not** shipped: `MemoryMax` (a cgroup OOM would kill the user's
whole desktop), `CPUQuota`, and every sandbox directive (the unit runs `bwrap`, `unshare`, PAM, setuid helpers and the user's
programs). Recommended per-host drop-in if memory pressure from one desktop is a problem (not enabled):

```
# /etc/systemd/system/farside-virtual-session@.service.d/memory.conf   (soft limit first, measure, then decide on a hard one)
[Service]
MemoryHigh=<about 75% of RAM>
```

## 5. Core dumps

`consolehost.cpp` and `virtualhost.cpp` call `KRdp::enterBrokerExecutionContext()` (`server/PrivateExecutionContext.h`) as the
first statement of `main`: `prctl(PR_SET_DUMPABLE, 0)`, fail closed (the broker refuses to start if it cannot). `RLIMIT_CORE`
is **not** lowered in the process, because the worker inherits rlimits (it does not inherit dumpable: `execve` resets it), and
worker cores stay wanted. The virtual host additionally has `LimitCORE=0` in its unit (no child below it).

What changes even when everything is right: a crash or watchdog abort (`WatchdogSec=90s` -> SIGABRT) of a broker leaves **no core**.
The terminate handler still logs the escaped exception and the stall detector still logs what the main thread was blocked in; those
journal lines are the remaining diagnostics. To capture a broker core for a diagnosis on a test host:

```
install -m 0600 -o root /dev/null /etc/farside/allow-broker-core-dumps      # root-owned regular file, not group/other-writable
systemctl restart farside-console-host                                      # logs a WARNING that cores are allowed
# virtual host also needs:  [Service] LimitCORE=infinity   (drop-in)
# afterwards: rm /etc/farside/allow-broker-core-dumps and restart; delete the core (it contains passwords and the TLS key)
```

## 6. Ranked risks (what a user could notice if a directive is wrong), most likely first

1. **Broker cannot renew a certificate whose path is under `/etc` but outside `/etc/farside`** (V, `ProtectSystem=full`): the
   listener starts with the old certificate; renewal at 12 h fails with a logged error. Fix: add the directory to `ReadWritePaths`.
2. **Login refused / PAM error for one account type** (C, V): a PAM module of this host needing a capability from the deny-list,
   an address family other than the four, or a setuid exec from a non-root context. Symptom: RDP login fails for everyone, or for
   LDAP/SSSD/Kerberos users only. Log: `pam_*` failure in the journal.
3. **Black or missing Console at the greeter or session** (C): a worker needing a family/capability/privilege escalation we did not
   foresee: worker exits right after start ("Console worker exited" in the journal), client sees no picture. Check
   `grep -E 'Seccomp|NoNewPrivs|CapBnd' /proc/<worker>/status`.
4. **GPU encoder missing** (both): only if the deny-list or seccomp filter hit a driver (`Host video encoders:` line in the log
   shows software only). Not expected: no device policy or W^X is set.
5. **Virtual desktop list empty / create fails** (V): `ProtectControlGroups`, `ProtectKernelTunables` or `PrivateTmp` hid something
   the broker needs. Not expected from the code; the checklist's Virtual create/attach/detach/stop step is the test.
6. **Desktop cannot start more threads** (S): `TasksMax=100000` reached; only by a runaway process.

Changes even when all is right: no broker core dumps (section 5); `systemctl status` shows the sandbox; the console host journal
may show a one-time warning if the core marker exists.

## 7. Offline checks run (no live unit touched)

`systemd-analyze verify --man=no` on the generated `krdp-console-host.service`, `krdp-virtual-host.service` and
`krdp-virtual-session@.service` prints only an unrelated warning about `system-xfs_scrub.slice` (`CPUAccounting=` is ignored).
`systemd-analyze security --offline=yes`: console host 9.4 UNSAFE -> 6.8 MEDIUM, virtual host 9.4 UNSAFE -> 5.8 MEDIUM.
Unit policy: `SystemdUnitPolicyTest` (encodes this table); core flag: `PrivateExecutionContextTest`.

## 8. Sol live checklist (all on Sol, no client attached for the restart; Hal only after this passes)

Record before/after in an evidence folder. Take before-values first with the old units:
`systemd-analyze security farside-console-host.service farside-virtual-host.service | tail -2` (pass: after is lower; expect about
6.8 / 5.8 or better; live numbers can differ from the offline ones).

| # | Check | Pass | Fail action |
|---|---|---|---|
| 1 | Install the candidate, `systemctl daemon-reload`, restart both brokers | both `active (running)`, `NRestarts=0`, no `status=226/NAMESPACE` or `status=218/CAPABILITIES` | drop the whole set (section 9) and report the status line |
| 2 | Listeners: `ss -tlnp \| grep -E ':(3391\|3395)'` | both present, owned by the right `farside-*-host` | as above |
| 3 | Sandbox as applied: `P=$(systemctl show -p MainPID --value farside-console-host); grep -E 'NoNewPrivs\|Seccomp\|CapBnd' /proc/$P/status` | `NoNewPrivs: 1`, `Seccomp: 2`, `CapBnd` lacks bits 16 (SYS_MODULE), 17 (RAWIO), 25 (SYS_TIME), 38 (PERFMON), 39 (BPF) etc.; same for the virtual host | drop-in per directive |
| 4 | Greeter capture (no local input): connect the Farside client to :3391 while SDDM shows the greeter | picture present, worker process runs as `sddm` | black = release blocker: find the denial in step 14 |
| 5 | Locked-session capture (no local input) | picture present, unlock works remotely | same |
| 6 | Login + worker launch: log in over RDP as a normal user; `journalctl -u farside-console-host -n 80` | "console worker started", no "Console worker exited" loop; `grep -E 'NoNewPrivs\|Seccomp\|CapBnd' /proc/<worker>/status` shows the inherited values | black screen: drop-in `NoNewPrivileges=no`, then `RestrictAddressFamilies=`, then the deny-list, retest after each |
| 7 | Worker helpers: Replace/Fit once (runs `kscreen-doctor`) | layout applies, no `Cannot launch` in the journal | as above |
| 8 | Virtual: create, attach, detach, re-attach, stop a desktop on :3395 | each succeeds, `systemctl is-active farside-virtual-session@<id>` follows, journal under `/var/lib/farside/virtual-sessions` updates | drop-ins for `ProtectSystem`, `ProtectKernelTunables`, `ProtectControlGroups`, `PrivateTmp` in that order |
| 9 | Hardware encoding: `journalctl -u farside-console-host -u farside-virtual-host \| grep 'video encoders'` | same encoders as before the change | drop-in for the deny-list, then `RestrictAddressFamilies=` |
| 10 | Microphone and camera (Virtual and Console) | present and working, as before | worker-side: see step 6 order |
| 11 | Clipboard text/image and file copy both directions, including a file from `/tmp` and from the home folder (Console) | all arrive | a `/tmp` miss means a private tmp crept in: `PrivateTmp=no` |
| 12 | Desk-input detection (Console): touch the physical keyboard | takeover/`deskInput` event logged | `PhysicalInputWatcher` denied: check step 14 for evdev or netlink |
| 13 | Certificates: `ls -l /etc/farside/*.crt` then `touch`-free renewal test: temporarily move `console.crt`/`.key` aside on Sol only and restart | a new pair is created, mode 0600/0644 | add the path to `ReadWritePaths` |
| 14 | Denials: `journalctl -u farside-console-host -u farside-virtual-host -u 'farside-virtual-session@*' --since <install time> \| grep -E 'EPERM\|EACCES\|Operation not permitted\|avc\|seccomp\|apparmor="DENIED"'` and `journalctl -k --since <install time> \| grep -iE 'seccomp\|audit.*SECCOMP'` | none attributable to the new directives (an unrelated AppArmor line is not ours) | the line names the syscall or capability: drop-in for the matching directive |
| 15 | Broker core: with no client, `kill -SEGV $(systemctl show -p MainPID --value farside-virtual-host)`; `coredumpctl list --since -5min` | **no** entry for `farside-virtual-host` and the broker restarts (`NRestarts=1`) | an entry means the dumpable call is not effective: report |
| 16 | Worker core still produced (Console, test user session): `kill -SEGV <worker pid>`; `coredumpctl list --since -5min` | an entry for `farside-console-worker`; remove it afterwards (`coredumpctl` entries are our own test core; never open or print it) | the worker lost its core: a limit leaked, report |
| 17 | Final: `coredumpctl list --since -1h`, `systemctl --failed` | nothing new except our own test cores (removed) | report |

## 9. Rollback

Per directive: the drop-in lines in section 3 (`systemctl edit farside-console-host.service`, `systemctl restart ...`). All at once for a
unit:

```
[Service]
NoNewPrivileges=no
LockPersonality=no
RestrictRealtime=no
RestrictSUIDSGID=no
RestrictNamespaces=
SystemCallArchitectures=
RestrictAddressFamilies=
CapabilityBoundingSet=~
# virtual host only:
ProtectSystem=no
ProtectKernelTunables=no
ProtectKernelModules=no
ProtectKernelLogs=no
ProtectControlGroups=no
PrivateTmp=no
LimitCORE=infinity
```

or install the previous package (rollback set `/root/farside-deploy-rel-*-backup-<host>/ROLLBACK.txt`). The `enterBrokerExecutionContext`
call is in the binary, not the unit; the core marker file (section 5) is its only switch.

## 10. Input devices in the Console broker: decision 2026-11-12

### What the broker reads and delivers

- `PhysicalInputWatcher` (`server/PhysicalInputWatcher.cpp:30-59`) opens every seat0 keyboard, mouse, touchpad and touchscreen
  evdev node with `open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC)` (`:44`). Which nodes qualify is decided by `isDeskDevice`
  (`server/PhysicalInputWatcher.h:27-33`): the node must be `/dev/input/event*`, not under `/devices/virtual/input/` (so
  uinput devices are excluded) and on seat0.
- Of each event only `isActivity` (`PhysicalInputWatcher.h:16-20`: key down, relative move, absolute event) is evaluated. Key
  codes are not stored or logged (comment at `PhysicalInputWatcher.cpp:57`). The only output is a bare `activity()` signal
  (`:58`), connected at `server/consolehost.cpp:166`.
- `ConsoleHostController::physicalInputActivity` (`server/ConsoleHostController.cpp:2542`) uses that signal for two things:
  desk-activity notices to clients (paced to at most 4 Hz by `DeskActivityTracker`), and ending a live Replace with reason
  `deskInput` (`:2560`, `:646`). The broker needs one bit: "the desk was touched now".
- The descriptors stay open for the life of the broker. They are not dropped when no client is connected.

### Threat statement

The broker runs the FreeRDP server stack and PAM as root, before the client is authenticated. An attacker with code
execution in that pre-authentication path is root and holds these descriptors, so can read everything typed at the desk,
including passwords at the greeter and lock screen. The same attacker can also ptrace KWin or the greeter (the broker keeps
`CAP_SYS_PTRACE`, section 2), `setuid` to any user, read the shadow file and read the TLS key. The input devices are the
cheapest route to typed passwords, not the only one. The unit directives in section 3 and the non-dumpable flag
(`PR_SET_DUMPABLE`) do not change this: they limit core dumps and some kernel interfaces, not file descriptors the process
already holds. A systemd `DeviceAllow` list cannot be used, because the capture worker is in the broker's cgroup
(section 1) and needs GPU, video4linux and FUSE devices.

### Options compared

| Option | What it does | Protects against | Does not protect against |
|---|---|---|---|
| (a) Non-root helper in group `input` that sends only rate-limited ticks; AppArmor profile denies the broker `/dev/input/**` | Moves the open descriptors out of the root broker; the broker receives ticks only | Reading typed passwords through evdev from a compromised broker | Other root routes to the same passwords (ptrace of KWin/greeter, `setuid`, shadow, TLS key) unless the broker's ptrace capability is also dropped; adds a daemon, a system user and an AppArmor profile on every host |
| (b) Take the signal from KWin idle events in the worker | Removes the watcher from the broker | The same evdev reading as (a), with no new daemon | Risk of counting remote (injected `fake_input`) input as desk activity, which would end Replace and flag the desk as busy wrongly; not verified (see below) |
| (c) Split into an unprivileged per-connection network front end and a small root broker for PAM and worker launch | Pre-authentication code no longer runs as root | The whole class: code execution in the network path no longer gives root | Nothing is protected until it is built; very large change; not planned |
| (d) Status quo, documented | Keeps the watcher in the broker | Nothing new | Everything above remains possible for a root attacker in the broker |

### Decision (Steve, 2026-11-12: "go with your recommendation"; the recommendation was the assistant's)

Option (d) now: keep the watcher in the broker, document it here, and fix the contradiction between the 2026-09-21 hosting spec
and the 2026-10-14 desk-activity spec (both now point to this section). Reasons:

- Option (a) adds a daemon, a system user and an AppArmor profile on every host, while a root attacker would take another
  path to the same passwords. If (a) is ever done, it should be paired with dropping the broker's `CAP_SYS_PTRACE`.
- Option (b) is not safe to assume: it is unverified whether injected input is counted (see below).
- Option (c) is the only option that changes the threat class. It is a separate, later decision and is not planned.

### Unverified points a future attempt must settle on Sol first

1. Whether AppArmor is active on every fleet host, and whether the capture worker (a child of the broker) inherits the
   broker's profile, so that a `/dev/input/**` denial for the broker would also bind the worker.
2. Whether KWin's idle detection counts `fake_input` events. If it does, option (b) cannot tell the desk from the remote user.
3. Whether a process holding `CAP_SYS_ADMIN` (kept, section 3) can leave the cgroup or mount namespace restrictions that
   a `DeviceAllow` or AppArmor approach relies on.
4. Whether PAM still works for the broker without `CAP_DAC_READ_SEARCH` (needed if the capability set is ever reduced).

Nothing in this section was run on a live unit; it records code reading and a design study only.
