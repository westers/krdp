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

Broker adoption prerequisite implemented: VirtualSessionGuardianClient performs
one bounded async exchange per Unix connection, validating private directory and
socket ownership, SO_PEERCRED UID,32-byte credential, request correlation,
session UUID, and expected guardian incarnation. Responses have strict shape,
bounded size and deadline; malformed/inconsistent liveness is not accepted.
Destructor/disconnect never sends Stop or signals a process. Explicit Stop
requires the exact expected incarnation; status is not capture readiness.
krdp-virtual-guardianctl exposes this same path with descriptor-only token input
and read-only status by default. It is a build target, not installed machinery.

Tests use an actual guardian/sleep child and show two fresh clients see the same
live child, then explicit Stop terminates it. Wrong token, stale status/stop
incarnation, wrong owner, silent peer, oversized/extra-field replies, correlation
mismatch and contradictory liveness are refused. The next integration must
adopt a trusted persisted desktop ID into a fresh manager generation only after
this handshake, then authenticate worker outputs/keyframe before attach. Control
socket failure must revoke transport readiness without killing the desktop or
silently creating a replacement; registry stop must use guardian Stop rather
than QProcess teardown. Existing HostController/Supervisor still use owned child
launches; this client alone does not change their lifetime behavior.

Adoption integration: registry can reserve a trusted stable desktop ID in a
fresh manager generation; supervisor adopts an exact guardian identity, polls
authenticated state, and requires both live guardian and fresh worker capture
before attach. Either readiness arrival order works. Destruction of this broker
only detaches adopted guardians; legacy owned-process launch remains supported.
HostController binds the authenticated worker endpoint and closes only affected
transports on supervision loss. Failed capture does not kill apps or authorize
replacement/forget; an explicit owner Stop can retry the same guardian identity.
Loss of contact during Stop reports Failed, never unproven Absent.

Per-runtime broker.lock is an exclusive0600 single-link flock retained through
endpoint teardown. After a broker crash, reclamation accepts only a socket in
the selected UID's private0700 canonical runtime, owned by that UID (or root
for a root successor), whose nonblocking connect returns ECONNREFUSED and whose
inode is unchanged. Live/uncertain sockets, symlinks and ordinary files are
refused. Cooperating brokers serialize under the lock; same UID is trusted.

Combined review found and corrected stop-after-capture-failure, root socket
ownership, crash-left socket recovery and callback lifetime issues. An added
stale-guardian-socket test actually reproduced a Qt Network synchronous-error
UAF. Guardian signals are now queued to the supervisor and resolve the full
handle at delivery; callbacks/handle arguments are copied before external
notification. Tests cover broker lifetime changes, both readiness gate orders,
owner denial, timeouts, explicit stop after failure, lost stop contact, callback
destruction including synchronous connection refusal, and live/stale/unsafe
worker socket lease cases. No live RDP recovery acceptance is inferred from them.

Sol-only virtualrdphostprobe gains --adopt SESSION INSTANCE RUNTIME with32-byte
credential on stdin. This adopts the already-running desktop into the real PAM
listener instead of launching a timed desktop; only the diagnostic listener
has its180s bound. New create requests are refused by its empty launch factory.
Durable registry/service startup and production installation are still pending.

Production recovery ordering: the trusted launcher must allocate and durably
record UID/session/launch/guardian-incarnation/token before spawning a service.
Guardian now accepts an optional canonical non-null --instance UUID from that
launcher (never RDP JSON), with the same parameter propagated by startPrepared.
Without it, diagnostic callers retain fresh random identity generation. Invalid
explicit CLI values are refused before storage/socket/child creation. A supplied
value is not permission to reuse a runtime or bypass the exclusive profile lock.
Use a fresh incarnation for every actual launch; uncertain recovery must not
silently spawn a replacement. This removes the identity-discovery crash window
but does not itself implement the durable journal or independent service launch.

Launch journal contract: a root-service-only store at installer-provisioned
/var/lib/krdp/virtual-sessions (0700) contains immutable0600 single-link records
of UID/session/launch/incarnation/boot UUIDs and32-byte credentials. No command,
environment, PID or client-selected path is serialized. Every ancestor is opened
without following symlinks and must be root-owned/non-shared; the directory FD
anchors subsequent operations. Temporary exclusive files are fsynced before
atomic no-replace publication, then the directory is fsynced. Failure forbids
spawn even if publication may have occurred. Existing intents are never replaced.
A nonblocking exclusive directory flock lasts for the journal object's lifetime;
another broker must wait for the old one to exit. A256-record bound is enforced
before publication and reading; unknown/corrupt state also prevents new launches.
Unpublished .pending files are ignored, not deleted; interrupted publication
leaving an extra hardlink fails closed and requires explicit reconciliation.
Reads reject nonregular/shared/hardlinked/oversized/malformed records and never
return a partial recovery set. A record proves intent only. Startup must compare
boot identity, preserve uncertain entries, authenticate exact guardian and await
fresh capture before attachment. No automatic resurrection after reboot, no PID
signalling and no root traversal of user-supplied runtime paths is permitted.
Journal is implemented/tested as a library; production startup/write-before-spawn
and explicit terminal reconciliation remain to be wired to the service launcher.

Host recovery integration: VirtualSessionHostController::recover reads the
protected journal and kernel boot UUID before listener startup. Whole-set
validation checks identities, duplicate runtime/incarnation aliases and existing
registry admission limits before any endpoint work. Matching-boot records use
the existing authenticated guardian/capture adoption; other-boot or missing/
occupied endpoints become owner-visible Failed intents without socket probes
for old boots, process launch, Stop, recreation or forgetting. Failed adoption
that already reserved its guardian runtime keeps that runtime's stricter gates.
Recovery can run only on a fresh controller and only once after successful import.
True means imported, not attachable; the caller must keep the journal lease alive
and fail startup on false. No service entrypoint invokes this yet. Independent
launch/write-before-spawn, explicit terminal reconciliation and installation
remain required; conservative unresolved records may need operator reconciliation.

Journal-driven recovery integration regression now runs a real guardian and
bounded sleep child through two fresh host controllers and reopened journal
leases. It uses the kernel boot UUID and exclusively creates a fresh canonical
runtime, then performs token-authenticated worker Hello/Ready/Outputs/frame IPC.
Attach is refused before fresh capture on each host; after fixture capture it
succeeds under a new manager identity, and the old handle is rejected. Guardian
child PID stays unchanged across both host destructors and journal reopens;
saved incarnation remains unchanged. This is real lifecycle/socket sequencing
with synthetic encoded bytes, NOT actual Plasma decoding/audio acceptance.
The fixture owns and removes only its fresh runtime and bounded child; it never
uses a live desktop. It skips without a nonroot user runtime (local run did not
skip). Production service entrypoint and launcher remain unfinished.
Review corrected async assertions to wait for observed guardian liveness and
worker Ready/Outputs, then frame signal counts; delta and wrong-layout keyframes
are refused. Stale-handle rejection runs while Starting (where a current handle
could otherwise mark capture ready), not after attachment. Five repeats pass.

Independent service launch prerequisites: root-only readLaunchIntent(session)
loads exactly one canonical immutable record without taking the broker's writer
lease. Shared parsing retains nofollow/owner0600/singlelink/bounded/schema checks;
the internal read-only object cannot insert or release another object's lease.
The service must validate current boot and existing-runtime refusal before spawn;
reading an intent is not relaunch authority. Credentials remain memory/FD data,
never log output or argv. VirtualSessionLaunchPlan can now consume the previously
recorded launch UUID, preserving its runtime/socket paths instead of generating
a conflicting fresh UUID. Empty optional identity retains diagnostic fresh-launch
behavior; explicit invalid identity is rejected. Executor/runtime preparation
must still reject reuse. Service executable, unit and broker launch wiring remain
to implement; these interfaces alone do not start or install a service.

Independent entry implemented as krdp-virtual-session-entry plus a draft
krdp-virtual-session@.service (neither installed yet). Root-only entry reads one
immutable intent, compares current kernel boot, resolves the account via passwd,
builds device-helper→guardian→namespace arguments with recorded session/launch/
incarnation, validates root-owned nonshared canonical helper/script/support paths
and bounded support tree, and refuses an existing runtime. Credential is written
to a CLOEXEC pipe and duped to stdin only; execve supplies only PATH/LANG and
closes inherited descriptors. No passwords/credential bytes enter argv/logs.
Draft unit is independent of the broker (no PartOf/BindsTo/Requires), KillMode
control-group, Restart=no; helper does private GPU admission then UID/capability
drop. Trusted service policy accepts a comma-separated PCI allow-list; empty is
refused. This draft config is not a delivered user setting: installed KCM/config
scope, packaging and privileged launch acceptance remain required. User runtime
provisioning/PAM-session lifetime is also not implemented; missing runtime fails
in storage preparation rather than borrowing another desktop. Broker does not
yet write intent/start the unit or supervise new creates through this path.
Review follow-up: runtime absence is not permission to reuse a launch identity.
Entry now claims the exact unchanged intent via a root-owned .claimed-SESSION
marker before pipe/exec. Exclusive nofollow creation plus file/directory fsync
makes consumption one-use; even an interrupted empty marker prevents retry.
The marker is never removed automatically, including exec failure. Recovery
ignores canonical consumption markers because they are not desktop identities;
an uncertain claim cannot hide other surviving desktops. Explicit privileged
reconciliation is required for consumed/ambiguous intents. Regression tests
cover a second reader/claim after fixture runtime removal and incomplete claims.

Independent create wiring: after journal recovery, host can enable the service
create handler before admitting clients. It preflights registry capacity, writes
fresh intent durably, reserves guardian supervision, then sends exactly one async
systemd StartUnit(unit,"fail") request (unit name derived from server UUID only).
No credential crosses D-Bus. Job acceptance is not readiness; request failure or
timeout retains intent/Failed without restart or kill. Before first authenticated
Running reply only, the supervisor polls the exact expected guardian identity on
a bounded startup deadline. It never retries launch. The host creates/leases the
worker endpoint only after this handshake; fresh matching capture still gates
attach. Request-ID dedup stays in the existing control dispatcher. Host shutdown
detaches rather than owns these services. Production listener/service caller,
PAM/runtime ownership and privileged live acceptance are not delivered yet.
Any journal insert failure freezes further creates in that host: publication
may have succeeded before directory fsync failed, so continued admission could
exceed limits and make later recovery fail. Existing desktops are unaffected;
restart/reconciliation must reread authoritative records. Fault-injection coverage
simulates a published record followed by a failed commit result, verifies no
service request or further creates, and recovers the intent as visible Failed.
Create enablement is bound to the exact journal object used for successful
recovery; swapping to another store cannot bypass recovered admission accounting.
Tests inject the service request (never start real system units), including a
real guardian/sleep child plus synthetic worker frame for create→attach and
host-destruction retention. The installed systemd Manager StartUnit signature
was confirmed by read-only bus introspection; actual service dispatch is not
runtime-accepted yet.

Production broker executable now exists as krdp-virtual-host with a draft,
noninstalled krdp-virtual-host.service. Root-only, explicit readable absolute TLS
files, validated numeric address/nonzero port (default3395, separate fromconsole),
PAM account authentication, journal lease/recovery then independentcreates BEFORE
listener startup. SIGINT/SIGTERM are blocked before worker threads and consumed
via signalfd on Qt's event loop so normal shutdown detaches transports/guardians
and releases the journal last. There is no physicalseat/layout adapter or bounded
desktop-lifetime timer. Draftservice supplies private StateDirectory and broker
restart policy but does not own independent session units. Neither executable nor
unit is installed yet: live root startup, PAM/runtime lifetime, settings/KCM and
matching packages remain acceptance gates. Nonroot invocation is tested to fail
before journal/listener access; unit syntax validation is not live acceptance.

Startup also parses bounded PEM certificate/key contents and checks their public
keys match before recovery/listening. Encrypted keys are rejected without an
interactive password prompt. Tests cover a valid generated pair and mismatched,
reversed, encrypted, empty, malformed, missing, directory and oversized inputs.
This validates initial configuration, not later replacement of the TLS files or
certificate trust/expiry policy; installed credential ownership remains an
installation responsibility.

## Login-session lifetime implementation contract

The independent service entry currently execs the privilege-dropping helper;
it cannot yet own a PAM session through desktop exit. Add a privileged lifetime
owner, separate from the RDP broker, retaining its PAM handle until the guardian
and namespace descendants terminate. Do not open PAM in the broker, borrow a
physical session, enable linger, or exec/drop privilege in the handle owner.
Opening PAM may migrate the calling process into a logind session scope: service
cgroup kill policy alone is NOT an adequate cleanup proof. Before integration,
design/review parent-child ownership, bounded signal forwarding, scope cleanup,
crash behavior and PID-reuse-safe termination. Keep the guardian independent of
broker death; distinguish desktop-owner death from broker death.

VirtualSessionPam now supplies the initial nonintegrated lifecycle component:
fixed service name; account check for journal-bound OS account; no prompts or
password persistence; reject identity remapping; background/wayland metadata;
reject absent or wrong-UID runtime metadata; close attempted session stacks even
when a later open module fails, then pam_end exactly once. Its draft Ubuntu PAM
policy requires pam_systemd (common-session makes that module optional) and denies
authentication, which remains the broker's separate boundary. Local-account
session management only: Kerberos/homed credential establishment is not claimed.
Tests replace PAM symbols and cannot mutate real login state. The component is
not linked into the service entry or installed yet.

Mandatory caller gates before enabling: validate trusted installed PAM policy
(missing service must not fall back to `other`), root clean startup context,
resolve journal UID to canonical OS account; verify returned logind session's
owner, leader, no physical seat and matching runtime using authoritative logind
properties; validate runtime directory ownership without symlinks. PAM success
and environment strings alone do not prove a new session. Never export PAM's
DBUS_SESSION_BUS_ADDRESS or physical display environment into the private graph.
Privileged acceptance must create a desktop with no seat0 login dependency,
retain it across broker restart and physical logout, then prove explicit logout
ends only its own login session and descendants. Do not log Steve out to test.

References: Linux-PAM pam_open_session/pam_close_session manuals and systemd259
pam_systemd implementation:
https://www.man7.org/linux/man-pages/man3/pam_open_session.3.html
https://www.man7.org/linux/man-pages/man3/pam_close_session.3.html
https://github.com/systemd/systemd/blob/v259/src/login/pam_systemd.c

### Runtime-owner direction and authoritative login gate

Architecture review supports a separate PAM keeper child plus a sibling desktop
guardian under the independent service parent. The service parent never opens
PAM; only the keeper may migrate into the new logind scope. Guardian/namespace
remain in the desktop service cgroup. This is a runtime lifetime registration,
not a claim that desktop PIDs belong to the keeper's logind session: PAM limits,
audit identity, credentials and keyrings in the keeper do not propagate to its
sibling. Preserve the local-account/private-bus restriction and do not export a
misleading XDG_SESSION_ID to desktop applications. No implementation of this
supervisor/keeper has landed yet.

Before enabling that design, require: audited supported PAM stack preserving
parent-death protection through startup; exec-before-PAM (no shared handles);
launch-generation-bound bounded Ready/control pipes with no sibling FD leaks;
unchanged parent cgroup and verified keeper membership; keeper failure/session
removal stops desktop; desktop descendant/namespace extinction precedes normal
PAM close; bounded open/close/stop with independent kill fallback. Guardian's
current default SIGTERM does not execute its C++ destructor; integrate graceful
stop, and do not infer descendant extinction from guardian exit. Existing
KillMode=control-group is fallback, not ordered teardown. Test parent/keeper/
guardian crashes, hung PAM, service stop and broker restart separately.

VirtualSessionLogin supplies the next read-only gate, not yet integrated:
GetSessionByPID followed by strictly typed Session/User GetAll snapshots on the
system bus (three calls bounded to3s each). Match journal UID, actual keeper PID,
PAM ID and exact runtime, fixed service, background/wayland, active-or-online,
empty seat/TTY/display, zero VT and session scope. Missing properties and wrong
types cannot become empty-seat/zero-VT defaults. This is a point-in-time identity
check, not a runtime-directory ownership check or continuing liveness monitor.
Private D-Bus fixture tests exercise actual wire decoding without contacting
host logind; pure checks mutate every identity/console field. Read-only host
busctl inspection confirms User(uo), Seat(so), Leader/VTNr(u), Scope/Service(s)
and User UID(u)/RuntimePath(s) signatures against the installed service.
Reference: https://github.com/systemd/systemd/blob/v259/man/org.freedesktop.login1.xml

Guardian service termination now consumes SIGTERM/SIGINT with signalfd on its
event loop and requests the same idempotent TERM→5s→KILL child stop used by an
authenticated explicit stop. It does not exit until QProcess observes child
termination, then retains the existing brief terminal-status window and runs
normal cleanup. The child explicitly unblocks those signals before exec; it must
not inherit the guardian's signal mask. Broker socket loss is unchanged and does
not call local stop. Executable-level fixtures cover TERM and INT with a graceful
child and forced escalation with a TERM-ignoring child, including socket cleanup
and observed child exit. This establishes direct-child shutdown only: actual
Plasma/bubblewrap namespace extinction and ordered PAM close still need proof.

2026-09-21 retained/adopted audio acceptance PASS: existing managed desktop
c3fc28e6-a437-41d3-9747-4841cc7e05b0 adopted on isolated3395; native Buzz GUI
attached and acknowledged playback1/silenceHost1. Played8s997Hz through its
private Pulse socket. Concurrent44100Hz stereoS16 monitor captures: Buzz1081344
samples, mean-26.7dBFS/peak-20.1dBFS (997Hz30Hzbandpass peak-21.1dBFS); Sol
physical chat/game each1081344samples and SPDIF1048576samples, all exactlyzero.
Disconnect restored private default to krdp.virtual-session.audio and removed
temporary remote sink. Physical defaults/processes, retained guardian/KWin and
production Buzz PID/configSHA unchanged. TestGUI exited0, no3395peers; exact
probe stopped,3395closed; guardian authenticated running afterwards. No unlocking,
physical-output changes or privileged operation. Evidence and limitations:
`~/dev/rdp/evidence/adopt-audio.qs1FEn/RESULT.md` plus logs/raw captures. This proves
audio payload and host digital silence after explicit adoption, not yet the new
journal/systemd/PAM-lifetime launch path or acoustic/conferencing performance.

### PAM keeper executable (not installed or integrated yet)

krdp-virtual-pam-keeper implements the independent session-owner half: explicit
root invocation, strict PATH/LANG-only environment before Qt, private stdin/stdout
pipes, actual-parent PID checked around pidfd_open, journal session/launch/current
boot, canonical OS account lookup, existing root-owned PAM policy (no `other`
fallback), PAM open, authoritative logind UID/leader/service/seat/type/runtime
checks, descriptor-relative nofollow root `/run/user` chain plus UID0700 leaf,
and cgroup-v2 membership in the expected session scope. Emits one bounded Ready
JSON carrying launch/session/login/UID/leader, never credentials; accepts only
`stop\n` or owner-channel loss and closes PAM before exit. The sibling desktop
must be launched/contained by the service parent, not by this keeper.

A separate pidfd-watching thread self-terminates the keeper if its parent dies
or startup exceeds30s; successful Ready removes that startup deadline. All PAM
cleanup paths, including partial-open failure, arm a fresh10s close deadline via
VirtualSessionPam's beforeClose callback. This avoids relying solely on
PDEATHSIG, which credential-changing PAM modules can clear. Unit fixtures use
real pidfds and disposable processes to test parent death, hung startup/close
and retained lifetime. Executable negative tests cover contaminated environment
and nonroot invocation without opening PAM. Positive privileged behavior remains
UNTESTED; no service entry invokes it and no install rule enables it.

Emergency SIGKILL intentionally bypasses PAM callbacks; it is not cleanup proof.
Required before enabling: service-parent process/namespace extinction tracking,
audited PAM module/helper behavior, external logind/scope crash reconciliation,
session removal monitoring, generation-bound Ready handling and ordered normal
desktop shutdown before keeper stop. Root-owned policy contents/includes are an
administrator installation responsibility, not verified by an ownership check.
The keeper's trusted logind snapshot does not prove subsequent runtime inode
continuity. No passwords or inherited host bus variables reach desktop children.
Reference: https://man7.org/linux/man-pages/man2/pidfd_open.2.html

### Dedicated service containment gate

VirtualSessionServiceScope now pins only the calling root launcher's exact
`/system.slice/krdp-virtual-session@UUID.service` cgroup on the cgroup-v2 kernel
filesystem. Entry validates this before consuming its launch claim. The draft
template explicitly sets Slice=system.slice and Delegate=no (instanced services
otherwise default to a per-template slice). No arbitrary root shell, broker unit,
physical login scope or client-supplied cgroup path can pass admission.

Snapshots require the parent still in that group, root-owned/non-group/world-
writable migration/control files, no nested cgroups, bounded complete PID lines,
and inclusion of the parent PID. Duplicates are allowed/deduplicated per kernel
semantics. Absent/malformed/truncated/oversized data is uncertainty, not proof of
extinction. No live non-parent members observed is distinct from reaped zombies:
the kernel omits zombies from cgroup.procs. Signal helper preflights pidfds and
membership and only permits TERM/KILL to non-parent members; submission is not
exit proof. Root signaling is NOT exercised by current tests.

Before the parent may use this cleanup helper, serialize its own PAM migration
and resolve keeper lifecycle separately: never include a migrating keeper as an
ordinary desktop cleanup target. Require no external privileged migration during
teardown. Pidfds prevent PID-reuse targeting, NOT atomic cgroup membership; mode
checks establish only a nondelegation snapshot. Normal caller must repeat the
extinction check after signaling and keep PAM while state is uncertain. Full
parent/keeper integration and privileged cgroup signaling remain unimplemented
or unaccepted; this adds entry admission plus the next teardown building block.
Tests cover pure PID parsing and temporary-directory missing/nested/symlink/empty
cases, nonroot/random-unit refusal and draft slice directives, not root runtime.
References: https://docs.kernel.org/admin-guide/cgroup-v2.html and
https://github.com/systemd/systemd/blob/main/man/systemd.service.xml

### Persistent service owner integration (not installed; root acceptance pending)

The entry now remains the dedicated unit's root parent instead of execing the
desktop chain. Its environment is PATH/LANG-only before Qt, inherited descriptors
above stderr are closed at entry, and TERM/INT are blocked before any threads.
It validates the installed keeper path, launches that separate executable with
private owner pipes, and admits exactly one bounded Ready record matching the
journal session/launch/UID and the actual child PID. It independently checks the
keeper's logind metadata and session-scope membership, then requires its own
service group to contain only itself before launching the sibling desktop.
Validation time counts toward the opening deadline; cancellation during opening
does not race keeper migration with service-group cleanup.

Both children use clean environments, working directory `/`, and Qt's
CloseFileDescriptors; their child modifiers unblock TERM/INT. The desktop token
is prefilled into an anonymous CLOEXEC pipe before fork and duplicated to stdin
in the modifier, not queued after exec (the guardian reads immediately in
nonblocking mode). Neither sibling receives the other's control/credential FDs.
The parent keeps no PAM handle. It polls authoritative keeper/login membership
while running; keeper loss or failed login checks stop the desktop.

Normal shutdown sends TERM to the guardian, allows8s for its5s escalation and
terminal-status window, then requests verified service-member SIGKILL if needed.
Guardian exit alone never authorizes PAM close: the dedicated scope must report
no live non-parent members. Only then does the parent send `stop\n` to the keeper
and wait for normal completion. Keeper close is bounded12s, followed by kill;
if it still cannot exit5s later, emergency teardown is requested. Unknown desktop
extinction requests emergency at20s. Emergency is latched once: ask systemd to
stop this exact UUID unit, then failed `_exit(1)` after5s if still alive, avoiding
QProcess destructor waits. This makes internal failures initiate a stop job too,
not just administrator-initiated stops. Draft KillMode=mixed/TimeoutStopSec=60
lets the parent handle normal ordering and systemd handle emergency leftovers.

Emergency termination is NOT proof of PAM close or cleanup of the migrated
logind scope. External crash/session reconciliation remains UNIMPLEMENTED and
mandatory before installation. Live root admission/migration, actual namespace
extinction, runtime inode continuity and systemd emergency behavior still need
privileged acceptance. The production checks may block the event loop for up to
9s per logind snapshot; asynchronous monitoring remains a responsiveness follow-up.

Disposable nonroot sibling-process tests cover ordering, prefilled credentials,
inheritable-FD exclusion, inherited blocked-signal reset, fragmented/wrong/large
Ready, authoritative gate failure, cancellation during opening, keeper loss,
login removal, hung open/close, failed executables, descendant-state uncertainty,
one-shot emergency request, slow-validation timeout, real pidfd KILL of a
TERM-ignoring fixture, and active-owner destruction callback safety. Scope/logind
checks are injected in these tests; they do not claim actual root cleanup.
Qt child modifier runs before UnixProcessParameters/descriptor closure:
https://doc.qt.io/qt-6/qprocess.html#setChildProcessModifier

### Durable keeper identity before PAM and recovery design constraints

Before opening PAM the keeper now publishes an immutable root-owned0600
`.keeper-<desktopUUID>` companion in the existing0700 launch journal. It binds
session/launch/boot/UID, keeper PID, canonical decimal start ticks and the64-bit
pidfs inode identity. The existing committed intent and consumed launch marker
must both match. Exclusive creation plus file/directory fsync must all succeed
before PAM may run; partial/uncertain files are preserved and block replay.
Missing and malformed records are distinct. Companion files do not prevent the
broker from recovering unrelated desktops. No companion contains credentials.

The pidfs identity requires modern64-bit Linux pidfs, refusing anonymous-inode
pidfds and32-bit identity truncation. Kernel inode identity, not just potentially
same-tick PID/start-time reuse, determines whether a reopened pidfd belongs to
the original process. VirtualSessionKeeperProcess pins BEFORE reading procfs,
checks observed exit around start-time lookup, refuses conflicting metadata,
and treats a different kernel identity as original-gone without signaling or
waiting for the replacement. Its kill method targets only the retained pidfd;
submission is not exit proof. Caller MUST establish matching current boot and
desktop extinction before use. This helper currently has no production caller.

PAM receives `XDG_SESSION_DESKTOP=krdp-<launchUUID>` before registration. Strict
Desktop property parsing and `matchesLaunch()` are used by both keeper and
parent, alongside the existing UID/leader/service/no-console checks. The marker
is keeper metadata only; it is not exported to private desktop applications.
The draft PAM stack no longer supplies a generic desktop fallback. Fixtures
exercise marker rejection, exact environment writes, journal safety/replay,
file/directory fsync failure, unusual proc comm delimiters, reopening pidfds,
generation mismatch and real exit/signaling of a disposable child. Changed
inode fixtures simulate reuse; they do not force actual kernel PID reuse.

The following recovery constraints were reviewed but the logind reconciler and
ExecStopPost integration are NOT IMPLEMENTED yet:

- Validate the exact dedicated unit and absence of desktop descendants. A
  verified original keeper may temporarily still be in that service before PAM
  migration; no arbitrary extra processes/nested cgroups may be accepted.
- Missing birth record means no PAM only after establishing that no live keeper
  can still publish it. Wait/kill the exact original keeper via pidfd, observing
  exit before logind reconciliation. Errors remain uncertain.
- Enumerate marker-bearing sessions first; duplicate markers or contradictory
  UID/leader/service/type/class/seat/TTY/display metadata are failure, not silent
  filtering. Recovery must accept opening/closing identities without depending
  on an already-removed user runtime; Ready's liveness predicate is unsuitable.
- Pin logind's unique bus owner across listing, revalidation, termination and
  disappearance checks. A path alone is not generation-bound. Support only
  source-verified non-reused counter session IDs within that owner lifetime, or
  provide an equivalent conditional operation; reject other identity schemes.
- Keeper exit does not cancel an already submitted registration request. An
  empty list/quiet interval/Ping is not a completion barrier across PAM's
  Varlink and D-Bus transports. Without a source-backed registration-completion
  guarantee, bounded zero-match recovery must report unresolved failure, keep
  journals and never authorize relaunch/deletion. Successful Terminate submission
  also needs authoritative disappearance before claiming reconciliation.

No reconciliation can claim that skipped PAM close callbacks ran. Fixed-stack
resource audit, crashes at registration boundaries, bus-owner changes, ambiguous
markers and the actual systemd post-stop path remain enablement gates. Upstream
v259 uses `.control` for delegated control processes; Delegate=no keeps this
draft's ExecStopPost in its exact service cgroup, subject to installed validation.
Primary implementation references:
https://raw.githubusercontent.com/torvalds/linux/v6.17/fs/pidfs.c
https://raw.githubusercontent.com/systemd/systemd/v259/src/login/pam_systemd.c
https://raw.githubusercontent.com/systemd/systemd/v259/src/core/execute.c

### Bounded post-stop reconciliation implemented (not installed)

The draft unit now invokes `krdp-virtual-session-cleanup` through ExecStopPost.
This supersedes the preceding unimplemented status: KeeperProcess now has a
production-code caller, but privileged runtime acceptance remains outstanding.
Cleanup validates immutable intent/current boot and the exact service cgroup,
proves desktop extinction, pins the original keeper, waits briefly for natural
exit and if necessary kills only its retained pidfd, then observes extinction
again before touching logind. A missing birth record is reread AFTER verified
service extinction, covering publication/migration between the initial read and
the cgroup check. Unknown evidence never means absence.

The keeper writes a root-owned exclusive `.closed-<desktopUUID>` companion only
after successful PAM close and end. It binds launch, boot and pidfs identity to
the immutable birth record, with file/directory fsync and no overwrite. This
permits already-closed recovery; without that evidence, an initially empty
logind list remains unresolved, not proof that registration cannot arrive later.

LoginRecovery uses one bounded deadline and pins logind's unique bus owner.
It strictly enumerates and checks launch markers, refuses ambiguous or
contradictory identities, revalidates before terminating the exact session, and
requires authoritative disappearance afterward. Only canonical counter IDs
(`cNN`) are supported, under the supported logind owner's non-reuse contract
(no 64-bit counter wrap); audit-derived IDs are refused. Owner replacement,
method errors, changed identities and incomplete replies remain failures.
All launch/birth/closed records are preserved; success does not authorize replay
or claim that emergency-skipped PAM callbacks executed.

Private-bus tests cover opening/active/closing sessions, physical-login
preservation, duplicate markers, malformed and contradictory properties,
revalidation races, delayed registration, owner replacement, timeouts and
termination acknowledgements without disappearance. Journal tests cover closed
evidence safety, fsync failures and the missing-birth migration race. These are
not evidence of actual root/system-bus cleanup. Installed PAM policy/resource
audit, supported logind/kernel validation, actual ExecStopPost cgroup placement
and Sol crash-boundary acceptance remain required before enablement.
