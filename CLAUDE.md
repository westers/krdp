# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Standing project instructions (imported)

Canonical project memory (hosts, live service, safety rules, gotchas, roadmap), imported:

@~/dev/rdp/CLAUDE.md

Also read `~/dev/rdp/HANDOFF-CODEX.md` §3 "Steve's rules" (override everything), §2 (keeping shared
files in shape) and §7 "Log" (newest first = latest state). `AGENTS.md` just points to these.

## How this repo fits the workflow

- This is Steve's fork of KDE KRdp. Remotes: `github` (Steve's GitHub, push here), `origin` (upstream
  invent.kde.org, fetch only). Work on `master`. No amend/rebase/stash on shared branches.
- `research.md` is the `OPT-###` tracker. Each item has a status (`PLANNED | IN PROGRESS | DONE <date> |
  OPEN | PARTIAL`), its design decisions, measured numbers, what was not exercised, and follow-ups.
  Newer notes go above older ones and history is never deleted. The "Status Snapshot" list at the top
  gets one dated line per slice.
- Commits use an imperative subject tagged with the item, e.g. `OPT-044 Test three-screen selected
  virtual creation` (older ones put `(OPT-044)` at the end), and a body that says what changed and why.
- When a slice lands, also add a dated line under "Fixed" in `~/dev/rdp/CLAUDE.md` and a newest-first
  entry in the HANDOFF "Log". Report honestly: what passed, what was not exercised, and numbers.
- For design-risk work, write a spec in `docs/superpowers/specs/YYYY-MM-DD-<topic>-design.md` and a
  plan in `docs/superpowers/plans/YYYY-MM-DD-<topic>.md`. Task ledgers go in
  `.superpowers/sdd/<plan>/progress.md`, which is git-ignored. Small items don't need either.
- `TODO_UPSTREAM_PR.md` and `UPSTREAM_MR_DRAFT.md` are OUTDATED (pre-2026-09-15 clean-up).

## Build

Dependencies (see `CMakeLists.txt`): Qt >= 6.9.2 (Core Quick Gui Network DBus WaylandClient Qml, plus
GuiPrivate on Qt >= 6.10), KF6 >= 6.22 (Config DBusAddons KCMUtils I18n CoreAddons StatusNotifierItem
Crash GuiAddons GlobalAccel), ECM, FreeRDP/WinPR/FreeRDP-Server >= 3.1, KPipeWire, XKB, Qt6Keychain,
PAM, libpipewire-0.3, libavcodec/libavutil, and PlasmaWaylandProtocols (for `BUILD_PLASMA_SESSION`).
Systemd is optional. C++20.

```bash
cmake --build ~/dev/krdp/build -j16          # main dev tree: Debug, BUILD_TESTING=ON, Unix Makefiles
~/dev/krdp/scripts/build-kpipewire.sh        # rebuild private KPipeWire (~/dev/kpipewire) into .deps/, reconfigure+relink build/
~/dev/krdp/scripts/check-kpipewire-link.sh   # verify krdpserver resolves libKPipeWire* from .deps/kpipewire
~/dev/krdp/scripts/check-stock-build.sh      # verify krdpplasmastreamer still builds against system KPipeWire
```

- `build/` points `KPipeWire_DIR`/`CMAKE_PREFIX_PATH` at `.deps/kpipewire` (exact flags in
  `build-kpipewire.sh`; back to system lib: `-UKPipeWire_DIR -DCMAKE_PREFIX_PATH=`). That script
  installs over the library the live service maps (see imported rules).
- Options: `BUILD_EXAMPLES`, `BUILD_PLASMA_SESSION` (ON); `INSTALL_DIAGNOSTIC_PROBES` (OFF: probes built, not installed); `KRDP_BUILD_{CONSOLE,VIRTUAL}_TEST_PACKAGE`
  (OFF, set only by packaging scripts). KDE clang-format is enforced by the `.git/hooks` pre-commit.

## Tests

The autotests are QtTest/ECM executables registered by name with `add_test` in
`autotests/CMakeLists.txt`. There are no labels. Currently 69 tests; the first one is `appstreamtest`.

```bash
ctest --test-dir ~/dev/krdp/build --output-on-failure                  # all
ctest --test-dir ~/dev/krdp/build -R '^VirtualSessionControlTest$' --output-on-failure   # one test
ctest --test-dir ~/dev/krdp/build -R 'Console' -N                      # list by regex
~/dev/krdp/build/bin/VirtualSessionControlTest <testFunction>           # one QtTest function directly
```

- Many tests link with `-Wl,--wrap=` (fsync, write, renameat, flock, execv, read, WTS*) to inject
  failures. Several have explicit TIMEOUTs.
- `VirtualSessionBusTest` and the PipeWire/audio tests (`PipeWireMicrophone*`, `VirtualSessionAudio*`)
  are only registered when their tools (dbus-run-session, pipewire, pw-dump, jq, wireplumber, ffmpeg,
  pactl…) are found; the audio shell tests exit 77 (skip) when the environment can't run them.
- `GuardianSignalChild`, `OwnerWatchProbe` and `ServiceOwnerChild` are test helpers, not tests.

## Smoke test, harnesses and probes

- `./smoke-test.sh` runs `cmake --build build`, then **restarts `plasma-xdg-desktop-portal-kde` and
  the live `app-org.kde.krdpserver`**, then follows the logs. Agents must pass `--no-restart`,
  because restarting the live service is Steve's call. Other flags: `--no-build`, `--no-watch`,
  `--watch-seconds N`, `--assert-encoder vaapi|software`.
- `examples/`: `plasmastreamer` (`krdpplasmastreamer`, the no-client pipeline harness described in the
  imported file), `streamer` (portal variant), `avc444probe` (`krdpavc444probe`, decodes AVC444 output
  like a client and scores PSNR), and `krdpctl-probe` (a minimal libfreerdp client for the private
  `KRDPCTL` channel: `--query`, `--apply f.json`, `--apply-seq`, `--gfx`).
- `server/` also builds `krdp-*-probe` tools; they and `scripts/probe-*.sh` are for explicitly
  authorized disposable runs on Sol only.

## Packaging (non-activating test debs; never installs anything)

```bash
scripts/package-console-test.sh   # build-console-package/, Ninja, prefix /opt/krdp-console, paired krdp-console-host+worker
scripts/package-virtual-test.sh   # build-virtual-package/, prefix /opt/krdp-virtual-service-test, paired broker/worker/helpers
```

- Both bundle the private KPipeWire and stamp the git short revision into the version
  (`6.6.80-git.<sha>`). The units they ship are inactive drafts.
- Installing is user-run: stop, install, start, typed into tmux for Steve without pressing Enter.
  Never install only the broker or only the worker, because the internal worker wire version is paired.
- `docs/virtual-session-test-staging.md` covers `DESTDIR` staging (choose the prefix before building;
  draft units embed `CMAKE_INSTALL_PREFIX`). `scripts/check-virtual-session-install.sh` is a
  read-only diagnostic.

## Architecture

**`src/` → `libKRdp`** is the reusable RDP server library, built on FreeRDP 3 server APIs.
- `Server` (a QTcpServer) creates one `RdpConnection` per client. `PeerContext` wraps the
  `freerdp_peer`.
- `AbstractSession` is the capture/input backend: `PortalSession` (xdg-desktop-portal) or
  `PlasmaScreencastV1Session` (`--plasma`: KWin `zkde_screencast_unstable_v1` + `fake_input`, granted
  only to the exact executable path in the `.desktop` file's `X-KDE-Wayland-Interfaces`).
- KPipeWire does capture and H.264 encoding (VA-API `h264_vaapi` or libx264). `VideoStream` sends the
  encoded frames over RDPGFX as AVC420, AVC444 or AVC444v2, handling caps negotiation, `ResetGraphics`
  and surfaces. It uses `FrameQueuePolicy`, `AdaptiveQuality`/`NetworkDetection` for QP steering and
  `VideoCodecSupport`. `SurfaceLayout` and `RemoteMonitorGeometry` handle multi-monitor.
- `InputHandler`, `Cursor`, `Clipboard`, and PipeWire audio playback, microphone and camera round out
  the session. `LayoutControl` is the private `KRDPCTL` static virtual channel that the own client
  (`~/dev/krdp-client`) uses (OPT-044).
- `src/kcm/`: System Settings KCM. The `krdpserverrc` schema is `server/krdpserversettings.kcfg`
  (the only copy; the KCM generates its settings class from it).

**`server/` → `krdpserver` plus a family of helper daemons.**
- `krdpserver` (`main.cpp`, `SessionController`) is the user-service daemon. It maps `MonitorMode`
  (`workspace|primary|specific|multi|virtual`) to sessions.
  Helpers: `PhysicalOutputGuard` (`kscreen-doctor` snapshot/restore, crash recovery), `HostLayoutExecutor`/
  `LayoutOwner`/`LayoutSessionDiff` (`KRDPCTL` layouts), `TakeoverDetector`, `DisplayWakeGuard`.
- **Physical console** (`krdp-console-host` + `krdp-console-worker`, `Console*`) is a system-service
  broker. It owns the TLS/RDP transport across the SDDM greeter to seat0 Plasma handoff. A per-session
  worker captures and injects input over an authenticated Unix socket (`ConsoleWorkerWire`, versioned).
  There is one controller at a time; other connections are viewers. `ConsoleTopology*` and
  `RemoteTopology*` implement revisioned preview/apply of monitor layout (Fit, Match, Add/Remove,
  physical lease). The experimental paths stay default-OFF behind `KRDP_EXPERIMENTAL_CONSOLE_TOPOLOGY`
  and `KRDP_EXPERIMENTAL_CONSOLE_VIRTUAL`.
- **Virtual sessions** (`krdp-virtual-host` broker + `VirtualSession*`) are persistent,
  monitor-independent Plasma desktops per PAM-authenticated user. Each one runs a private
  `kwin_wayland --virtual`, bus and PipeWire, launched by `scripts/launch-virtual-session.sh` and
  `virtual-session-desktop.sh` under `krdp-virtual-guardian` (helpers `krdp-virtual-session-entry`,
  `-pam-keeper`, `-session-cleanup`, `-device-entry`). `VirtualSessionControl` is the
  list/preview-create/create/attach/detach/stop protocol (one-use preview tokens), persisted via
  `VirtualSessionJournal`/`Registry`/`Supervisor`. `VirtualInitialLayout`/`VirtualInitialBootstrap`
  = selected-screen first layout (default-OFF `--experimental-initial-layout`); `VirtualResize*` = Fit.
- **Maintenance guard**: deleted 2026-09-27 (AUD-C1); the full stack is preserved under the git tag
  `archive/maintenance-guard`. The generic create-admission hook (`VirtualSessionHostController::
  CreateAdmission`, `Refusal::Maintenance`) remains but nothing in production sets it.
- Most `server/*.h` files are pure, unit-tested planners/parsers (often header-only); the
  executables wire them together.

**Other directories**: `docs/` has feature notes; `docs/superpowers/{specs,plans}` has the binding
designs (start with `2026-09-21-console-and-session-hosting-design.md` and
`2026-09-22-remote-monitor-layout-design.md`). `patches/kpipewire/` holds one unused legacy patch; the
live KPipeWire patches are in `~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/`. `po/` is translations.

## Repo-specific gotchas

- README's CLI examples use `kwriteconfig6 ... --notify`. The standing rule is **never** `--notify`,
  because it makes every running krdpserver reload, including the live one. Never write
  `~/.config/krdpserverrc`. Test instances get their own port and `XDG_CONFIG_HOME`.
- Hosts: Hal (hal9000, live :3389) is protected. Sol is the disposable test host (Console :3391,
  virtual broker :3395). Buzz is the client laptop. OPT-044 topology work is source-only unless the
  HANDOFF Log says otherwise.
- The untracked `kpipewire_6.6.3*` tarballs in the repo root are not part of the tree. Leave them alone.
