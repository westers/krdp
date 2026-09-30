# Farside console and virtual desktops — consolidation

Status: active design, 2026-09-29. Steve's acceptance: a Farside connection is either **Console** (the existing desktop, including its sign-in and lock screens) or **Virtual** (a separate desktop). The user-facing behavior must not depend on which implementation path first provided the feature.

## Current gap

Farside presently ships three server paths: a per-user desktop service on :3389 (`farside-server`), a system console broker on :3391 (`farside-console-host` plus a worker in the logged-in session), and a virtual broker on :3395. The per-user service owns complete RDPECAM camera redirection and live camera `on`/`off`/`reselect`. Both brokers advertise `camera.toggle=false` and `camera.reselect=false`; their worker wire has microphone but no camera records. The client therefore says Sol cannot switch its camera during a Console session.

Sol's Discover password failure is separate evidence: on the unlocked active physical session 429, `polkit-kde-authentication-agent-1` crashed with SIGSEGV three times just after requesting the password. The virtual lock-screen workaround is loaded only by `launch-virtual-session.sh`, and session 429's `LockedHint` was `no`. No RDP input fault or causal relationship to screen locking has been demonstrated. A temporary GNOME PolicyKit agent also crashed, but in its X11-only time lookup on Wayland, so that experiment does not identify the KDE fault. The KDE service is restored; Sol has a runtime-only `KDE_DEBUG=1` drop-in to allow the next reproducible crash to produce a core and backtrace.

## Target

1. **Console:** the system console host is Farside's only route to the existing desktop. Keep :3391 during migration so installed clients and the separately installable stock KRDP :3389 do not collide. Retire Farside's per-user :3389 service after its settings and saved client connections migrate to Console. A saved connection's mode is explicit, never inferred only from a port or from the old `physicalConsole` toggle. Existing saved entries are migrated without losing credentials or monitor layouts; no in-place mutation of the active client session.
2. **Virtual:** the retained-desktop broker remains the only route to separate desktops, currently :3395. It shares the same device-control semantics as Console, subject only to ownership/consent and whether a logged-in worker is ready.
3. **Camera:** RDPECAM enumeration and samples stay on each broker's RDP connection. An authenticated, bounded extension of the paired worker wire sends the camera media description and compressed samples into that desktop's worker. The worker owns the PipeWire camera source and optional V4L2 loopback, so a root broker never joins a user's PipeWire socket. Worker demand opens the client camera only while an app is using it. The broker closes the RDPECAM channels on `off`, `reselect`, control loss, worker loss, or desktop switch, then reports state via the existing KRDPCTL `device` contract. One controlling client can share a camera at a time; viewers cannot.
4. **Authorization:** PolicyKit stays a desktop service. Console/virtual migration must not bypass authentication, export the host password, or disable PolicyKit. Fix or replace the crashing KDE agent based on a reproducible stack; verify Discover and a second PolicyKit requester from Console and Virtual. The virtual desktop's lock limitation is a separate project decision and must not be used as a workaround for PolicyKit.

## Legacy feature inventory and destination

The per-user server is a separate implementation, not just a third name. Its `main.cpp` reads `farsideserverrc`, and its `SessionController` implements several controls that the broker paths do not yet expose. Removing its unit or binary before this inventory is implemented would discard working behavior. The user-visible connection type is independent of the monitor-capture mode.

| Per-user feature | Console destination | Virtual destination | Current broker gap |
|---|---|---|---|
| Password/PAM access, TLS certificate, listen address/port | System broker and its per-user authorization/settings scope | Virtual broker and per-user policy | KCM currently targets the per-user unit; broker ports and certificates are CLI/service settings |
| Video quality cap and adaptive quality | Per-connection stream and worker config | Per-connection stream and worker config | Both paths hard-code quality 80 and adaptive quality off; worker quality follows the owner, but no user setting |
| Codec preference, software encoding, AV1 tiles, AVC444 timing, audio priority, VAAPI driver | Shared video policy with mode-specific worker | Same policy | Some CLI flags exist; remaining settings and KCM bindings do not |
| Physical monitors, one monitor, whole workspace, multi-monitor capture | Console capture selection | Not applicable to a separate desktop | Console currently captures its own fixed physical-output layout; its experimental topology operations are separate from capture selection |
| Per-client virtual output in the existing desktop, replace/extend policy, fallback size | Console's virtual-output path; preserve restore journal and local takeover | Not applicable to a separate desktop | Existing experimental Console virtual-output path is narrower than the old `MonitorMode=virtual` policy |
| Separate retained desktop selection, creation, and topology | Not applicable | Virtual broker | Already present; preserve standard-client attach-or-create policy |
| `layout` query/apply, topology query/preview/apply, physical resize, pointer/cursor, clipboard | Console, with lease and ownership rules | Virtual, with retained-desktop rules | Per-user `layoutApply` is available; broker Console currently advertises `layoutQuery` and a separate topology path |
| Playback, microphone, camera, live device switching, standard RDP client media | Console's logged-in worker | Virtual desktop worker | Camera absent from both brokers; standard media policy and loopback path must be migrated |
| Wake/inhibit display while streaming | Console seat/desktop | Virtual desktop's own activity policy | Per-user `WakeDisplayOnConnect` does not have a broker equivalent |

The shared configuration model must identify Console and Virtual explicitly, validate every setting, state its scope and restart requirement in the Farside KCM, and migrate existing per-user settings without overwriting the original file. Capabilities must reflect actual supported behavior, not a requested mode. Keep the per-user service installed but hidden from new profile creation until every row has an implementation and a migration test; retire it only after saved connections and settings can roll back.

## Gates

- Pure protocol tests: malformed/oversized camera records, generation checks, backpressure, stale frame rejection, ownership changes, worker loss, repeated on/off/reselect, multi-client contention.
- Isolated live Sol/Buzz test: camera appears on the correct desktop, LED only while a consumer uses it, switching changes source, and the node disappears promptly when off/disconnected. Test both Console and Virtual, with a real camera and standard RDP client where possible.
- Discover authorization test in Console and Virtual; check the authentication agent stays alive and PackageKit transaction completes. Record any external KDE-agent workaround separately from Farside source.
- Migrate :3389 per-user Farside connections and package/KCM settings only after both broker routes pass; check saved connection hashes/backup and package apt holds. Hal is last, with zero ESTAB to its :3389 server before stopping that service; do not modify Hal's live client window or work desktop.

## 2026-09-30 implementation checkpoint

Broker/worker wire v6 and both broker camera controllers are in `dd39649`, `a0aedb3`, and `bceeefd`; the client has an isolated `--camera-device` test selector in `5ac629f`. Console on Sol accepted Buzz's webcam and delivered a JPEG sample to a 20-frame PipeWire consumer. Virtual published the source inside its private PipeWire runtime; a 20-frame consumer ended too quickly to prove JPEG delivery and exposed start/stop reply misordering. `6cb4526` serializes those replies; its Sol package and a longer consumer gate are pending. The :3389 per-user service and saved profiles remain in place while the other inventory rows are implemented.
