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

Live-registry implementation: VirtualSessionRegistry indexes separate opaque
desktop IDs, scopes listing/create/resume/stop/recreate/forget to authenticated
nonroot UIDs and delegates phases to VirtualSessionState. Supervisor events
carry a manager-instance UUID plus session ID and generation; old callbacks
cannot act on replacement sessions. A transport may attach only one desktop;
disconnect retains it, stopping waits for actual exit, and failed desktops
require explicit recreation. Per-user/global limits include retained, stopping
and failed entries until confirmed-terminal metadata is explicitly forgotten.
Tests cover cross-user refusal, retained reconnect, crash replacement/stale
callbacks, manager identity, capacity and stop/exit ordering. This registry is
in-memory and NOT yet wired to a supervisor, transport or persistent store.
Recovery must verify a surviving runtime identity/readiness before adoption;
do not deserialize an Attached/Retained flag and assume applications survived.

Process supervision foundation: VirtualSessionSupervisor now owns actual
QProcess leaders independently of attached clients and uses the live registry.
A trusted launch factory receives authenticated UID/internal handle; it must
supply absolute executable, explicit environment, identity setup and a
namespace leader guaranteeing descendant cleanup. Readiness is an explicit
authenticated capture event, never stdout text or successful spawn. Startup
deadline kills failed leaders, owner stop drains until exit, then escalates
TERM to KILL on the exact QProcess context. Disconnect has no process action.
Real disposable-process tests cover retention, cross-owner stop refusal,
failed spawn, rejected factory/environment, startup timeout, stale readiness,
unexpected exit and forced stop without affecting another runtime.

This class is not yet installed/runtime-wired. Its current destructor explicitly
tears down owned leaders; production service restart recovery remains required
and must not be advertised from this foundation. Next extract a reviewed
installed private Plasma launch contract from the bounded probe, connect worker
readiness/IPC and authenticated transports, and implement verified runtime
adoption rather than PID/record-only restart recovery.

Capture-worker mode separation: consoleworker now accepts exactly one of
--logind-session or --virtual-session (canonical non-null registry UUID).
The latter disables physical reclaim shortcut/cursor takeover and explicitly
rejects physical-output resize rather than silently modesetting its compositor.
Capture/input and consent-gated desktop media remain reusable; virtual resize
requires its own layout implementation. Both modes require actual real/effective
UID matching the broker-supplied nonroot UID. Existing physical launch arguments
remain unchanged. Pure mode parsing/action policy is tested; real virtual-worker
socket/capture acceptance and installed launcher integration remain pending.

2026-09-21 actual virtual-worker acceptance PASS (2b74ffa, Solp1IfWf):
new bounded --plasma-worker-nvidia mode uses freshly built real capture worker,
private desktop-service identity and ConsoleWorkerEndpoint. Broker generates
fresh UUID/token, delivers token via private-file stdin fd0 (not argv), accepts
authenticated Ready then matching output metadata and103952-byte1280x720
AVC420 keyframe. Worker explicitly refuses correlated physical Resize request;
broker Stop ends worker0, while private KWin/Plasma remain alive and output
readback stays1280x720. Bounded namespace then exits0. Decoded keyframe inspected
(wallpaper, early shell frame) at rdp/evidence/virtual-worker-keyframe.png;
raw bitstream/log alongside. Physical KWin168073/host167444/session817active
unchanged after cleanup. VirtualUser target is explicit; logind selectors never
select it and physical launcher rejects it before reading any desktop env.
Local37/37 tests pass; affected physical tests rerun after build4/4pass.
This validates the actual worker/IPC capture boundary, not supervisor integration,
authenticated RDP create-resume UI or production persistence/restart recovery.

2026-09-21 real namespace supervisor integration PASS (99956ce, SolOPYgRT):
krdp-virtual-session-probe supplies a trusted launch factory for the bounded
private Plasma script, binds the authenticated ConsoleWorkerEndpoint outside
the namespace through its owned0700 runtime, and lets VirtualSessionSupervisor
own the namespace leader. Actual worker Ready + matching1280x720 metadata and
keyframe call captureReady, then registry client1 attaches/detaches. One second
later registry client2 reattaches the SAME generation1, and explicit owner stop
waits for namespace leader exit/Absent. Probe exits0; only physical KWin168073
remains; physical817active/host167444 unchanged. Worker log copied to
rdp/evidence/virtual-supervised-worker.log. Local focused lifecycle tests4/4pass.

These registry clients are simulated identifiers, NOT two RDP transports. The
earlier native-GUI reconnect proof remains separate. The manager now supervises
a real authenticated-capture namespace, but still through the Sol-only bounded
probe (read-only HOME, incidental render ACL); no installed persistent launcher,
PAM admission/create-resume protocol, UI or restart adoption is delivered yet.

Authenticated-identity prerequisite: RdpConnection now exposes thread-safe,
read-only authenticatedPamUid(). It is published only after successful PAM
authentication/account checks, resolution of PAM's final PAM_USER to an OS
account and successful PostConnect media initialization. Static configured
credentials never acquire a UID. Identity clears on close/reauthentication;
single-user daemons also check the resolved UID against their own account.
Every successfully started PAM transaction now calls pam_end on all returns.
Virtual-host admission must use this value, never a KRDPCTL UID/username.
Current unit coverage checks no inferred identity on a new connection/media
policy changes; real PAM success/failure/canonicalization acceptance remains
pending and no live service has been deployed with this change.

Lifecycle command dispatch (eb0226f): VirtualSessionControl implements strict
`virtual-session {v:1,id,action[,session]}` requests for list/create/attach/
detach/stop. Broker supplies authenticated nonroot UID plus unique transport ID;
JSON identity fields, unknown fields/actions, malformed UUIDs/versions and
changed transport identity are refused. List is owner-scoped, create acceptance
reports actual starting/failed state without claiming readiness, attach requires
capture readiness. Detach/socket disconnect invokes input/media-release hook
before retaining the namespace; an owner stop also releases any attached owner
transport. Correlated replies are retained for256 requests per connection;
identical retries return the original reply, altered reuse is refused. History
is never evicted into accidental repeated create; at limit reconnect is required.
This bounded policy needs suitable UI request cadence, not unlimited polling.

Five request-dispatch tests use real disposable supervised children: malformed/
claimed identity refusal, owner visibility, duplicate mutation safety, readiness,
detach/resume, owner stop/release ordering, capacity and failed launch reporting.
Full local38/38 suite passes. Dispatcher is wired into the Sol-only manager probe,
not yet a production RDP listener; real transport/client feature advertisement
and lifecycle wiring must precede delivery. Release hooks are synchronous and
must not reenter/remove dispatcher transports during dispatch.

Sol command-driven acceptance TFLiPw exits0: real namespace/worker capture,
create/attach/detach/reattach/stop all through VirtualSessionControl using the
probe's own verified UID and synthetic client IDs. Worker control generation is
revoked and audio disabled by the release hook. Same registry generation1 after
reattach; only physical KWin168073 remains after scoped stop;817active and
host167444 unchanged. This does not exercise PAM or actual RDP control messages.

RDP adapter foundation: VirtualSessionTransport connects actual RdpConnection
control/input/video signals to VirtualSessionControl and authenticated virtual
worker resolution. Every route checks PAM UID, endpoint UID and exact registry
handle. Attachment grants a new manager-wide control generation and requests a
keyframe; revoke disables/clears the video queue, revokes held worker input,
stops media, removes worker signal routes, and socket close retains the desktop.
Playback uses external worker PCM only, never broker PipeWire. Microphone/camera
and other unsupported controls are explicitly refused in this new adapter until
integrated; earlier console microphone acceptance is not virtual-adapter support.

Host wiring must register the dispatcher release hook to revoke the named
transport before stop/detach and provide generation-safe worker resolution.
The adapter is currently compiled/tested only, not instantiated by an installed
listener. Test verifies a real unauthenticated RdpConnection cannot create a
desktop or enable media;39/39 complete suite passes. Positive PAM attachment,
actual wire video/audio/control, disconnect/reconnect and failure paths through
this adapter still need end-to-end acceptance before feature advertisement.

Host composition: VirtualSessionHostController owns registry/supervisor/control,
per-connection RDP adapters and generation-scoped authenticated worker endpoints.
A trusted preparation callback receives OS UID/internal handle/fresh token and
returns runtime/socket/process setup; endpoint listens before spawn. Ready alone
does not admit attach: matching output metadata plus a keyframe gates readiness.
Server::newConnectionCreated installs adapters before queued RDP initialization;
the dispatcher's release hook resolves/revokes the named adapter. Client teardown
extracts its entry before destruction to avoid callback reentry; host teardown
drains adapters and closes endpoints before destroying the supervisor.

Tests cover both host/connection destruction orders, ordinary connection creation
without desktop spawn, token delivery/VirtualUser target, stale manager/generation
resolution and owned process stop. This composition is currently test-linked;
next instantiate it in an isolated PAM listener with the private launcher, exercise
actual Buzz control/video/audio/reconnect, and only then integrate installed
hosting, client UI and recovery. No new production listener is running yet.

2026-09-21: Isolated same-user PAM host on Sol3395 now exercised through native
Buzz GUI. Actual list/create/attach/detach and fresh-client retained-session
reattach/display pass; explicit stop reaches absent and removes private KWin.
Client739626a fixes a15-second layout fallback delay on explicit unsupported
layout control (the original short reconnect probe ended before that timer).
Screenshots and logs: rdp/evidence/virtual-pam-fixed-reconnect.{png,log}.

New-host audio acceptance: after attachment, debug media-resend applies saved
playback/silenceHost consent. Private997Hz10-second tone arrives at Buzz
(7012352 sampled values,997Hz bandpass peak-21.1dB); simultaneous Solphysical
sink monitor is exactlyzero,3522560frames/channel. Seat0 default unchanged;
detach removes transient remote sink and restores private desktop sink.
RDPSND remains uncompressed44100Hz/stereo/S16. All temporary desktops stopped;
Solphysical817/KWin168073/consolehost167444 and Buzzproductionconfig unchanged.
This is bounded probe acceptance, not installed delivery. Next production
launcher and typed client create/resume UI, automatic correlated postattach
consent, app/logout retention acceptance, recovery and matching packages.

## Production launcher contract (implementation in progress)

VirtualSessionLaunchPlan constructs a launch only from a PAM UID, an OS-resolved
matching account, a server-generated session UUID and administrator configuration.
Profiles persist at HOME/.krdp-virtual/sessions/<session UUID>.
Each launch uses a fresh runtime UUID below /run/user/<uid>/krdp-virtual, so
recreation cannot overwrite a previous generation's token/socket. The executor
must create both parents privately, reject symlinks/incorrect ownership, and
atomically refuse a preexisting runtime. No reuse/adoption from path or PID alone.

The launcher receives explicit worker/support paths, even bounded capture size
and an ordered PCI render-device allow-list. This is compositor access, not an
NVENC/offloaded-encoding admission policy. A transient renderD index is refused.
No allow-list is an explicit unsupported CPU-only configuration for now, not
permission to expose every GPU. Hardware resolution must match sysfs PCI identity,
render node and driver; only selected render/driver devices may enter the namespace,
never a modesetting card or evdev. Recheck access after dropping to the PAM UID.

The environment is constructed from scratch (PATH, HOME, USER, LOGNAME, LANG).
Namespace setup must then provide private XDG/bus/audio paths and persistent
profile mounts; do not import the physical user-manager environment. No desktop
lifetime timeout is included. Retain bounded startup/stop deadlines instead.
The executable/executor is not implemented or installed by this contract slice.
The current supervisor still owns/kills its children on destruction. Production
restart retention needs a separately supervised namespace plus validated runtime
handshake and durable registry; removing die-with-parent alone is not recovery.

VirtualSessionStorage now implements actual profile/runtime preparation after
real/effective UID drop: descriptor-relative mkdir/open, no symlink components,
owned non-shared home directories, private runtime/profile trees, exclusive
0600 regular single-link profile.lock plus nonblocking flock, and new0700 runtime
with O_EXCL0600 token. Existing runtimes are never overwritten or auto-adopted.
Tests use disposable directories and prove retained file contents, concurrent
profile refusal, symlink/shared-permission/identity/path/token-size refusal and
hardlink-lock refusal. Destruction releases locks but deletes nothing.

The eventual independent namespace supervisor must own this storage object
for the desktop's entire lifetime, not one RDP transport or restartable broker.
It must keep the lock across desktop startup (CLOEXEC FDs cannot simply be
lost in an exec). Storage preparation is not yet wired to the probe or an
installed launcher. No test has launched a production namespace with it yet.

VirtualSessionGuardian and the standalone krdp-virtual-guardian executable now
own one trusted child independently of broker socket clients. The private Unix
control socket accepts only root or the same OS UID, then requires a32-byte
credential for status/stop. Requests are bounded/versioned/correlated; stop
also requires the guardian's fresh incarnation UUID. Old-runtime requests cannot
stop a replacement. No child command/environment comes from control JSON.
Socket disconnect only drops that connection; explicit stop terminates then
kills after5s. Status distinguishes starting/running/exited/failed and does NOT
assert capture readiness. No desktop lifetime timer exists.

Tests exercise the real executable with a disposable sleep child, destroy the
first broker socket, reconnect through a second socket to the same incarnation,
then explicitly stop and observe exit0. Negative tests cover bad token, stale
incarnation, existing socket and inherited environment. CLI token travels by
descriptor, never argv; nonroot real/effective UID must match. This binary is
not installed yet and is NOT full restart recovery: still wire storage-lock
ownership, actual namespace/device executor, independent service placement,
durable registry/worker reconnection and capture-generation validation. Ordinary
guardian shutdown still owns descendant teardown; only broker loss preserves it.

Guardian storage ownership is now connected: startPrepared takes exclusive
ownership of VirtualSessionStorage and checks the storage UID/session identity.
The lock outlives child exec and teardown, and is never held by a client socket.
The executable's --launch-id mode resolves the real account with getpwuid_r,
prepares runtime/profile storage and starts guardian.sock there. It excludes
the lower-level --socket mode. HOME/USER/LOGNAME are OS-derived; private paths
are explicit KRDP_VIRTUAL_RUNTIME/KRDP_VIRTUAL_PROFILE environment entries for
the trusted namespace helper. Tests use disposable directory anchors, verify a
second profile launch is refused while the real child runs, verify release after
guardian shutdown, and reject a different profile's session ID before spawn.
Actual Plasma launch, allowed-device resolution, independent service placement,
capture-worker reconnection and durable recovery remain unimplemented here.

2026-09-21 production executor follow-up: scripts/launch-virtual-session.sh and
virtual-session-desktop.sh now implement the private namespace/Plasma launch,
ordered PCI/sysfs render-device selection, private audio graph, retained profile
defaults, and capture-worker restart while keeping the desktop alive. This is
source implementation, not installed production acceptance or broker recovery.
Guardian captures helper diagnostics in its private runtime/launcher.log.

Sol runtime attempt with d6a8169 verifies profile creation at
HOME/.krdp-virtual/sessions (0700), avoiding the existing group-writable .local
and share ancestors without changing their permissions. Session
da3464e6-41ed-4333-b1a2-c63685ba1d43, launch
204c49fa-a95a-41e0-8e2e-9034c493075e exited1 with
"No permitted render GPU is accessible; no fallback to physical DRM."
Sol was at SDDM954; westers lacked renderD128 access. No desktop launched,
profile lock was released, only greeter KWin200016 remained, and existing
console host167444 still listened on3391. Three focused launch/storage/guardian
tests pass. Profile/runtime evidence retained; the runtime UUID must not be
reused for another launch.

Production admission must grant scoped access to only administrator-allowed
render devices independently of seat0's logind ACL. Asking the user to log into
seat0 again would unblock a probe but would not satisfy logged-out virtual
hosting. Do not silently grant permanent render/video membership or broaden
device permissions. Privileged deployment remains user-run through host tmux;
independent guardian placement and durable broker/worker adoption remain open.

Scoped-device helper design: a root-service-only executable (never setuid and
never invoked directly by RDP JSON) validates an ordered PCI allow-list, resolves
sysfs driver identity and character-device numbers, and validates an installed
root-owned guardian path. It creates a new mount namespace, makes propagation
recursively private BEFORE mounting anything, overlays /dev with private tmpfs,
and recreates only basic character devices plus the selected render/driver nodes.
GPU nodes are mode0600 owned by the authenticated OS UID only within that mount.
No host ACL/chown, group database edit, modesetting card, evdev, or other GPU.
NVIDIA requires selected /dev/nvidiaN plus shared control/UVM interfaces; this
is not a hostile-same-UID or GPU-driver security boundary.

It drops supplementary groups, all bounding/ambient capabilities, real/effective/
saved gid and uid, enables no-new-privileges, closes extra FDs, and execs the
guardian with a clean environment. Token input is FD0; no credential argv.
Errors after unshare terminate the process and its private mount state, not
repair host mounts. Readiness still requires authenticated capture frames.
The helper does not establish a PAM session, user runtime directory, durable
registry or independent system service; those are required integration steps.
Acceptance must include logged-out Sol, unchanged host device metadata/ACL,
private node identity/permissions, empty effective capabilities, selected-GPU
capture, and full cleanup. Unprivileged tests cannot prove these mount gates.

Combined review hardening: render st_rdev must match both sysfs dev contents
and reverse /sys/dev/char identity; NVIDIA node major/minor must match kernel
/proc/devices registration and the selected GPU's information file (ctl255,
uvm0). The root caller must launch the helper itself with a clean environment
and installed root-controlled executable/libraries, before its dynamic loader
runs; cleaning only the guardian environment is insufficient. FD0 must be the
token pipe and FD1/2 safe log destinations. No privileged acceptance is claimed
by the nonroot refusal test. Review found no blocking mount/drop ordering defect.

2026-09-21 logged-out device-entry runtime PASS: user executed the prepared
Sol tmux command. Private nodes mode0600 westers:westers; identity1000:1000,
all five capability sets zero, NoNewPrivs1, render open succeeds, no card0/card1.
Host nodes remain renderD128 root:render0660 with sddm-only user ACL and NVIDIA
nodes root:root0666. Physical SDDM954/KWin200016 and consolehost167444:3391
remain unchanged. Guardian201320 owns sessionc3fc28e6-a437-41d3-9747-4841cc7e05b0,
launchb236be9a-dd0d-4047-a814-6c8285a78f5b; private KWin201386 reports OpenGL
RTX2070, one1280x720 output and no PipeWire hardware Devices. Full Plasma panel
and icons visibly confirmed in decoded capture rdp/evidence/virtual-device-b236be9a.png.

Probe7b1c367 adds --managed-session: only creates broker endpoint in an owned
private runtime with no existing socket, reads existing0600 single-link token
without following symlinks, and lets guardian's loop launch the worker. Two
separate invocations each received authenticated116763-byte1280x720 keyframes,
verified physical-resize refusal, stopped only the worker, and exited0. Same
guardian PID/starttime and private desktop survived both endpoint lifetimes.
This is capture/worker-reconnect acceptance, NOT full RDP/client reattachment,
broker restart recovery, fresh audio-tone acceptance or final namespace cleanup.
Guardian remains running in the user's tmux for the next integration test.
