# Persistent virtual-session implementation plan

Binding outcome: `../specs/2026-09-21-console-and-session-hosting-design.md`.
Physical Sol handoff has user acceptance over three login/logout cycles;
virtual sessions are not implemented. A retained virtual output in seat0 is
not a virtual session and cannot satisfy this plan.

## Discovery and isolation gate

Read-only Sol discovery on 2026-09-21 confirms `kwin_wayland --virtual`,
`--socket`, `--width`, `--height`, `--output-count`, and
`--exit-with-session` are available, as are startplasma-wayland and
dbus-run-session. No compositor was launched by this discovery.

Plasma 6.6 startplasma.cpp normally selects systemd boot via startkderc,
imports/restores the user-manager environment and manipulates shared user units.
Installed plasma-plasmashell.service owns the singleton org.kde.plasmashell
name and participates in graphical-session.target. Therefore merely choosing
a different Wayland socket is insufficient isolation for the same Unix user.

Source: https://github.com/KDE/plasma-workspace/blob/Plasma/6.6/startkde/startplasma.cpp

Before running a desktop, review/prove the following launch contract:

- Authenticate through the host's PAM boundary; bind session identity to the
  authenticated UID, never a client-supplied user or arbitrary command.
- Give each retained session a private bus, named Wayland socket, supervised
  process group and generation. Never import its environment into the physical
  user's manager or start/stop that manager's graphical-session.target.
- Verify the non-systemd Plasma startup path and its shutdown behavior from
  source. Do not assume dbus-run-session alone prevents systemd activation.
- Keep runtime credentials/sockets owned by the session user, with an explicit
  allow-list of inherited environment variables. Do not borrow WAYLAND_SOCKET,
  QT_WAYLAND_RECONNECT or display/bus variables from seat0.
- Isolate session audio as well: same-UID applications otherwise share PipeWire
  and its default sink. Virtual-session silence must never reroute seat0 apps.
  Prove a separate graph or equivalently strict per-session routing before
  claiming audio isolation; do not reuse physical global-default mutation.
- Launch only on Sol for initial acceptance. Do not use Hal's desktop, restart
  SDDM or log out an existing user for this work.

## Implementation sequence and gates

1. Review the launch/environment/audio contract and add testable construction
   of bounded arguments and paths. Reject physical DRM/windowed fallbacks.
2. Add a UID-scoped persistent session registry with explicit creating, ready,
   attached, disconnected-retained, stopping and failed states. Scope async
   callbacks to a generation; a stale callback must not adopt/stop a successor.
   Disconnect releases input/media transport but never terminates applications.
   Reattach requires the same authenticated owner and real process readiness.
3. Add a supervisor/launcher that owns the isolated compositor and Plasma
   lifetime independently of each RDP connection. Bind actual readiness to the
   authenticated worker, capture layout and first keyframe, not process spawn.
   Logout is terminal; freezing is future work, not implied by retention.
4. Reuse stable host transports/worker IPC without ConsoleSeat's seat0 selection.
   Expose an explicit client create/resume session choice, separate from
   physical-console attach and ordinary virtual-monitor layout controls.
5. Wire private audio playback and per-client consent. Test a known non-silent
   PCM source at Buzz while both the virtual-session and seat0 physical outputs
   are observed. Keep microphone/camera capability reporting truthful.
6. Real native Buzz GUI acceptance on Sol: create a desktop, open an app with
   unsaved content, disconnect, verify processes remain, reconnect to the same
   session and content, then explicitly log out and verify scoped cleanup.
   Repeat with seat0 active and prove its output geometry, apps, bus names and
   audio routing unchanged. Add crash/restart reconciliation and cross-user
   denial tests; never use a PID alone as retained-session identity.
7. Deliver matching client/server packages and documentation. Physical-mode
   regression includes SDDM handoff, Fit, control transfer and audio restoration.

Completion requires those runtime proofs. Unit tests or a headless compositor
with no Plasma desktop do not establish virtual-session support.

## Implementation evidence

2026-09-21: Source review of startplasma-wayland.cpp and startPlasmaSession()
finds environment synchronization, ResetFailed/Reload and logout cleanup even
around the classic-boot path. A private bus with a verified activation policy
is required; merely setting systemdBoot=false is not the isolation boundary.
No experimental compositor is launched until that boundary is verified.

VirtualSessionState.h now supplies the pure lifecycle primitive: authenticated
UID-scoped creation/attach/explicit stop, single controller, disconnect retains
the current generation, stop waits for exit, and late events cannot touch a
replacement. Unexpected exit invalidates retention instead of silently making
an empty replacement. Four unit scenarios cover repeated reconnect, ownership,
stopping/late callbacks and crash invalidation; full server suite is 26/26.
It is not yet a persistent registry, process supervisor, authentication layer,
or runtime desktop implementation, and is not wired into the console host.

2026-09-21 full Plasma probe: private PID/mount namespace (bubblewrap), host
runtime sockets and devices hidden, isolated configuration and restricted
autostart. AppArmor's world-writable permission-query file must remain writable
inside the read-only root for D-Bus mediation; do not disable AppArmor.
KWin and private PipeWire worked, but classic Plasma did not reach readiness.
`ksmserver/main.cpp` explicitly forces the xcb Qt platform even in a Wayland
desktop, so omitting Xwayland causes its startup to abort. This is distinct
from using an X11 RDP client or changing the compositor to X11.
Source: https://github.com/KDE/plasma-workspace/blob/Plasma/6.6/ksmserver/main.cpp

Before proceeding, resolve whether the user's Wayland-only requirement permits
Plasma's internal Xwayland compatibility dependency. Do not silently enable it
or call a standalone plasmashell a complete managed Plasma session. Also disable
irrelevant kded hardware modules in the isolated profile: BlueDevil repeatedly
reactivated obexd without the system bus, producing an activation loop during
the bounded probe. Entire PID namespace exited; Sol greeter/KWin/host unchanged.
Probe logs now go to the evidence directory rather than flooding SSH output.

The disposable profile now sets kded5rc Module-bluedevil/autoload=false,
plus Bolt, browser host-wrapper integration and Welcome Center startup off.
Kded6 source still reads kded5rc for this setting; load-on-demand comes from
plugin metadata, so these are autostart controls, not a security boundary.
Source: https://github.com/KDE/kded/blob/master/src/kded.cpp
Configuration parsing checked; runtime acceptance awaits the Xwayland decision.

Steve approved normal internal Xwayland compatibility on 2026-09-21. Keep the
compositor and RDP client Wayland; the isolated compatibility server is for
Plasma's session-manager dependency. The --plasma probe now starts the private
wrapper with --xwayland and passes its own child's display/authority path to
startplasma (the prestarted wrapper's earlier launch-environment notification
cannot update a plasma_session that did not exist yet). No authority cookie is
read or logged. Runtime acceptance still required.

2026-09-21 capture diagnosis: Sol probe lBNNLl reports KWin6.6.6,
VirtualBackend, active QPainter compositor. Upstream Plasma/6.6
`src/backends/virtual/virtual_backend.cpp` enables OpenGL only when a DRM
device opens; `src/plugins/screenshot/screenshot.cpp` requires EglBackend,
and `src/plugins/screencast/screencastmanager.cpp` explicitly rejects
non-OpenGL composition. Thus another screenshot timeout or switching to the
RDP worker cannot repair this backend limitation. Probe now records support
information and fails early for QPainter in Plasma capture mode.

Sol's renderD128 ACL grants root/render/sddm, not westers; its node is also
intentionally hidden by the probe. Next establish scoped rendering access
without exposing the physical modesetting card or changing seat0. Any
privileged ACL/device setup must follow Steve's existing tmux/password flow;
do not silently add permanent video/render group membership. CPU-only support
also remains a separate requirement: stock headless QPainter cannot provide
the required screencast path. No GPU access or production configuration has
been changed by this diagnosis.

2026-09-21 19:34 GPU rendering gate PASS: after Steve logged into physical
Sol817, logind granted westers renderD128 access automatically. No sudo,
group membership or ACL mutation was required. Ran existing Sol-only
--plasma-nvidia probe; private1280x720 compositor reports OpenGL/NVIDIA
GeForceRTX2070, private Plasma reports one desktop, Spectacle wrote a valid
1280x720 wallpaper screenshot (inspected), graph has no hardware devices,
probe exited0 and cleaned its namespace. Physical KWin168073 and console
host167444 remained unchanged and session817 stayed active. Evidence runtime
/run/user/1000/krdp-headless.V4BrDB, persistent copies under
~/dev/rdp/evidence/virtual-plasma-nvidia*.

This proves a separately rendered/capturable Plasma desktop, not an RDP
session or retained applications. Screenshot captured early wallpaper without
panel readiness proof. Next gate is actual worker capture/first keyframe and
native Buzz RDP view on this private compositor, then disconnect/reattach to
an unsaved application. CPU-only capture remains unresolved. Current render
permission depends on the physical login; production session ownership must
establish its own rendering admission rather than rely on that incidental ACL.

2026-09-21 RDP first-frame gate PASS: Mpv5Yt probe with policy-only private
WirePlumber renders the full Plasma desktop (panel and icons) in the native
Buzz GUI. KPipeWire AUTOCONNECT had no policy manager to link streams in the
earlier probe; adding it fixed frame delivery despite the remaining EGL
DMA-BUF warning. AVC420/libx264 emitted a116772-byte1280x720 keyframe and the
client decoded it. Screenshot: rdp/evidence/virtual-rdp-policy.png. Both client
and bounded probe exited0; only physical KWin168073 remained,3394 closed.
This is still a disposable listener using test credentials, not the registry,
PAM admission, persistent supervisor or client create/resume implementation.

2026-09-21 bounded reconnect gate PASS: --plasma-retention-nvidia starts Kate
with stdin in an anonymous session, displaying an unnamed modified document
containing unique runtime identity krdp-headless.xoK8SN. Two separate native
Buzz GUI processes connected, took screenshots and exited0. Between clients,
ss showed no3394 peers while Kate PID334/starttime62237832 stayed alive.
Second connection decoded a fresh keyframe and displayed the same unsaved
text, verified visually (rdp/evidence/retention-before.png and
retention-after.png). Physical Sol817 stayed active, console host167444
unchanged. This proves application retention across transport disconnect in
the bounded isolated runtime, not persistence across supervisor restart,
production create/resume admission, or explicit logout cleanup.

2026-09-21 virtual playback/host-silence gate PASS in bounded runtime8eyAFp:
synthetic10s997Hz stereo tone played by paplay using private Pulse default
after explicit media consent. Client acknowledged playback1/silenceHost1;
private default was krdp.remote-audio.62dedf894750. RDPSND negotiated PCM
44100Hz16bit stereo. Buzz physical sink monitor captured the tone (997Hz
bandpass max-21.1dB); concurrent Sol physical sink monitor remained silent.
Sol physical default SteelSeries analog-chat,817active and host167444 stayed
unchanged. First attempt disabled KRDPCTL via video-only test hook, so it
sent no consent/tone and was NOT accepted; retry enabled the channel in the
isolated compositor and GUI exited0. Client/server logs copied to
rdp/evidence/virtual-audio-{client,server}.log. This is actual private app →
RDPSND → Buzz speaker-sink proof, not production supervisor integration,
microphone/camera acceptance or acoustic latency measurement.
