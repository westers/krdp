// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX8: end to end, the real krdp-console-worker against the real broker endpoint.
//
// A private headless session in a scratch HOME and runtime directory - its own D-Bus daemon
// without service activation (nothing is auto-started: no ksecretd, no portal), PipeWire with
// the virtual desktop's config, WirePlumber's policy and `kwin_wayland --virtual` - runs in one
// bwrap sandbox shaped like launch-virtual-session.sh's: a fresh /dev with only the render node,
// a private PID namespace. Inside it, as virtual-session-desktop.sh does, a worker starts once
// the broker's socket exists. It must authenticate, report its encoders (hardware when this host
// has it: B3), confirm capture (Ready: B1) and deliver H.264 keyframes of the virtual output.
// Nothing here touches the user's own session, bus, PipeWire, keyring or port 3389. Skipped when
// a component (bwrap, kwin_wayland, pipewire, wireplumber, dbus-daemon) is missing.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTcpSocket>
#include <QUuid>
#include <QHash>
#include <QSet>

#include <functional>
#include <map>
#include <tuple>

#include <unistd.h>

#include "ClientStyleDecoder.h"
#include "ConsoleWorkerEndpoint.h"
#include "EncoderSupport.h"
#include "H264KeyframeSize.h"
#include "RenderNodes.h"
#include "SurfaceChain.h"
#include "VideoCodecSupport.h"
#include "ConsoleVirtualOutputRestore.h"
#include "ConsoleVirtualOutputMutation.h"

using namespace KRdp;

namespace
{
/// The worker under test; KRDP_E2E_WORKER runs another build's (e.g. a released one) for comparison.
QString workerProgram()
{
    return qEnvironmentVariable("KRDP_E2E_WORKER", QStringLiteral(KRDP_CONSOLE_WORKER));
}

/// The user server under test; KRDP_E2E_SERVER runs another build's (the pre-K4 red run).
QString serverProgram()
{
    return qEnvironmentVariable("KRDP_E2E_SERVER", QStringLiteral(KRDP_E2E_SERVER));
}

// The virtual desktop's session, reduced to what capture needs (virtual-session-desktop.sh).
constexpr const char *SessionScript = R"SH(
set -eu
R=$XDG_RUNTIME_DIR
children=
trap 'kill $children 2>/dev/null || true' EXIT
wait_for() { for i in $(seq 1 200); do [ -e "$1" ] && return 0; sleep 0.05; done; echo "timed out waiting for $1" >&2; return 1; }
dbus-daemon --config-file="$R/bus.conf" --nofork --nopidfile >"$HOME/dbus.log" 2>&1 & children="$children $!"
wait_for "$R/bus"
env PIPEWIRE_CONFIG_DIR="$KRDP_SERVER_DIR" PIPEWIRE_CONFIG_NAME=virtual-session-pipewire.conf pipewire >"$HOME/pipewire.log" 2>&1 & children="$children $!"
wait_for "$R/pipewire-0"
env WIREPLUMBER_CONFIG_DIR=/usr/share/wireplumber wireplumber --profile policy >"$HOME/wireplumber.log" 2>&1 & children="$children $!"
kbuildsycoca6 --noincremental >"$HOME/sycoca.log" 2>&1
kwin_wayland --virtual --width "$KRDP_E2E_WIDTH" --height "$KRDP_E2E_HEIGHT" --output-count "$KRDP_E2E_OUTPUTS" --socket wayland-0 \
    --no-lockscreen --no-global-shortcuts --no-kactivities >"$HOME/kwin.log" 2>&1 & children="$children $!"
wait_for "$R/wayland-0"
sleep 1
# OPT-055 K4: the user server (farside-server) under a Restart=on-failure loop, like its systemd unit.
# server-args/server-env (kept between runs) start it; server-stop ends it and the loop.
server_loop() {
    set +e
    while true; do
        while [ ! -f "$R/server-args" ]; do sleep 0.05; done
        mapfile -t sargs <"$R/server-args"
        senv=()
        if [ -f "$R/server-env" ]; then mapfile -t senv <"$R/server-env"; fi
        env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland "${senv[@]}" "$KRDP_E2E_SERVER" "${sargs[@]}" >>"$HOME/server.log" 2>&1 &
        spid=$!
        while kill -0 "$spid" 2>/dev/null; do
            if [ -f "$R/server-stop" ]; then kill "$spid" 2>/dev/null; fi
            sleep 0.1
        done
        wait "$spid"
        echo $? >"$R/server-exit"
        if [ -f "$R/server-stop" ]; then rm -f "$R/server-args" "$R/server-env" "$R/server-stop"; fi
        sleep 0.3
    done
}
server_loop & children="$children $!"
# Motion, as on cray (testsrc2 at 30 fps in the desktop): every output keeps encoding delta frames.
if [ -n "${KRDP_E2E_MOTION:-}" ]; then
    env WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland ffplay -loglevel error -an -fs -f lavfi \
        -i "testsrc2=size=${KRDP_E2E_WIDTH}x${KRDP_E2E_HEIGHT}:rate=30" >"$HOME/motion.log" 2>&1 & children="$children $!"
fi
# FIX-CURSOR: a full-screen window whose cursor shape follows $R/cursor-shape.
if [ -n "${KRDP_E2E_CURSOR_CLIENT:-}" ]; then
    echo arrow >"$R/cursor-shape"
    env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland "$KRDP_E2E_CURSOR_CLIENT" "$R/cursor-shape" >"$HOME/cursor-client.log" 2>&1 & children="$children $!"
    sleep 1
fi
# OPT-060 S3: stand-in ScreenSaver service (inside the sandbox: the bus refuses outside clients); idle until told.
mkdir -p "$R/locker"
"$KRDP_E2E_FAKE_LOCKER" "$R/locker" "$HOME/greeter-alive" >"$HOME/fake-locker.log" 2>&1 & children="$children $!"
: >"$R/session-ready"
set +e
# One worker per broker socket, like the desktop loop; worker-args says which kind (console or virtual).
while true; do
    while [ ! -S "$R/worker.sock" ] || [ ! -f "$R/worker-args" ]; do sleep 0.05; done
    mapfile -t args <"$R/worker-args"
    rm -f "$R/worker-args"
    wenv=()
    if [ -f "$R/worker-env" ]; then mapfile -t wenv <"$R/worker-env"; rm -f "$R/worker-env"; fi
    env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland "${wenv[@]}" "$KRDP_CONSOLE_WORKER" "${args[@]}" --uid "$(id -u)" \
        --socket "$R/worker.sock" --token-fd 0 --desktop-media <"$R/worker-token" >>"$HOME/worker.log" 2>&1
    echo $? >"$R/worker-exit"
done
)SH";

/**
 * AUD-FIX11 R6: what the broker delivers to a client from the frames its endpoint received, and
 * whether that client decodes it. The broker's rules are applied as VideoStream applies them:
 * a frame of another codec family than the connection's is never sent (queueFrame/sendFrame),
 * a new monitor layout (the published atlas) means new surfaces (performReset), and each
 * surface's SurfaceChain lets nothing of a codec out before a keyframe of it with in-band
 * headers. GfxSurfaceCommand passes a private payload through unchanged, and AVC420's bitstream
 * whole (GfxSurfaceCommandTest). The client is krdp-client 0.5.5: ClientStyle::Decoder, from the
 * first delivered packet on, one decoder per surface.
 */
struct Delivery {
    QString error; ///< the first packet the client would reject, empty if none
    QStringList firstPackets; ///< the first few packets per surface, with their OBU/NAL types
    QHash<int, int> pictures; ///< decoded pictures per monitor index
    QHash<int, int> deltas; ///< delivered delta frames per monitor index
    int delivered = 0;
    int held = 0; ///< held back by a SurfaceChain (no keyframe of the codec yet)
};

Delivery deliverAndDecode(const QVector<VideoFrame> &frames, qsizetype from, const std::function<VideoCodec(qsizetype)> &connectionCodec)
{
    Delivery delivery;
    ClientStyle::Decoder client;
    QHash<int, SurfaceChain> chains;
    QHash<int, int> surfaceOf;
    QVector<VideoMonitor> layout;
    int nextSurface = 1;
    bool first = true;
    for (qsizetype i = from; i < frames.size(); ++i) {
        const auto &frame = frames[i];
        const VideoCodec connection = connectionCodec(i);
        const VideoCodec produced = frame.codec.value_or(connection);
        if (produced != connection) continue;
        if (first || frame.monitors != layout) {
            // A new layout: VideoStream's reset creates new surfaces (new ids, new decoders).
            first = false;
            layout = frame.monitors;
            chains.clear();
            surfaceOf.clear();
        }
        const int monitor = frame.monitors.size() > 1 ? frame.monitorIndex : 0;
        if (!surfaceOf.contains(monitor)) surfaceOf[monitor] = nextSurface++;
        const int surface = surfaceOf[monitor];
        if (chains[surface].admit(produced, frame.codec.has_value(), frame.isKeyFrame, frame.data) != SurfaceChain::Verdict::Send) {
            ++delivery.held;
            continue;
        }
        ++delivery.delivered;
        if (!frame.isKeyFrame) ++delivery.deltas[monitor];
        // krdp-client keys its private decoders by surface; FreeRDP's AVC decoder is its own.
        const int key = codecFamily(produced) == 0 ? -surface : surface;
        if (client.surfaces().value(key).packets < 3) {
            delivery.firstPackets << QStringLiteral("surface %1 (monitor %2) %3 %4 %5 bytes: %6")
                                         .arg(surface)
                                         .arg(monitor)
                                         .arg(QLatin1String(VideoCodecSupport::codecName(produced)), frame.isKeyFrame ? QStringLiteral("key") : QStringLiteral("delta"))
                                         .arg(frame.data.size())
                                         .arg(ClientStyle::unitTypes(produced, frame.data));
        }
        if (!client.feed(key, produced, frame.data)) {
            delivery.error = client.error();
            return delivery;
        }
        delivery.pictures[monitor] = client.surfaces().value(key).pictures;
    }
    delivery.error = client.missingPictures(); // every delivered frame is a picture on the client
    return delivery;
}
}

/// One private headless session (bwrap, D-Bus without activation, PipeWire, WirePlumber, KWin).
struct PrivateSession {
    std::unique_ptr<QTemporaryDir> runtime;
    std::unique_ptr<QTemporaryDir> home;
    std::unique_ptr<QProcess> process;
    QString skip;

    /// The worker log written since \a offset (a size() from before).
    QString workerLogSince(qint64 offset) const
    {
        QFile file(home->path() + QStringLiteral("/worker.log"));
        if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) return {};
        return QString::fromUtf8(file.readAll());
    }
    qint64 workerLogSize() const
    {
        return QFileInfo(home->path() + QStringLiteral("/worker.log")).size();
    }

    QString log(const QString &name, int tail = 3000) const
    {
        QFile file(home->path() + QLatin1Char('/') + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll().right(tail)) : QString();
    }
};

/// What the broker's endpoint saw from one real worker.
struct WorkerRun {
    QStringList order;
    QStringList errors;
    std::optional<ConsoleWorkerWire::EncoderCaps> caps;
    QVector<VideoFrame> frames;
    int outputs = 0;
    int keyframes = 0;
    QSize keyframeSize;
    bool payloadMatches = false;
};

class WorkerEndToEndTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void workerReachesReadyAndDeliversFrames_data();
    void workerReachesReadyAndDeliversFrames();
    void consoleCaptureSelection_data();
    void consoleCaptureSelection();
    void consoleConfiguredOutputs_data();
    void consoleConfiguredOutputs();
    void consoleReplaceRelock_data();
    void consoleReplaceRelock();
    void consoleFreshWorkerFirstFrame_data();
    void consoleFreshWorkerFirstFrame();
    void consoleReplaceFailsOpen_data();
    void consoleReplaceFailsOpen();
    void codecSwitchAtAttach_data();
    void codecSwitchAtAttach();
    void coalesceKeepsEveryOutputAlive_data();
    void coalesceKeepsEveryOutputAlive();
    void cursorShapeReachesTheBroker_data();
    void cursorShapeReachesTheBroker();
    void av1TilesAndBitrate_data();
    void av1TilesAndBitrate();
    void clientCursorReachesTheBrokerAtRest_data();
    void clientCursorReachesTheBrokerAtRest();
    // OPT-055 K4 (T-K4b): an encoder that fails for good, driven by the test-only failfilter shim.
    void encoderFailureRestartsAndRecovers();
    void persistentEncoderFailureClosesAndNextWorkerStreams();
    void encoderFailureDuringDisconnectStillExits();
    void userServerEndsWithSessionEndAndKeepsAccepting();
    void userServerRestartsItselfIdleAfterAbandonedEncoder();
    void cleanupTestCase();

private:
    /// The session with \a outputs KWin virtual outputs of \a size, started on first use (nullptr:
    /// failed). With \a motion (and ffplay installed), a test pattern plays full screen in it.
    /// With \a cursorClient, CursorShapeClient runs full screen in it (FIX-CURSOR).
    PrivateSession *session(int outputs, QSize size = QSize(1280, 720), bool motion = false, bool cursorClient = false);
    /// Starts one worker in \a session behind \a endpoint and records what it sends into \a run.
    bool startWorker(PrivateSession &session, bool virtualDesktop, ConsoleWorkerEndpoint &endpoint, WorkerRun &run);
    void stopWorker(PrivateSession &session, ConsoleWorkerEndpoint &endpoint);
    /// KEY=VALUE lines the next worker starts with (written to worker-env, consumed by the session loop).
    QStringList m_nextWorkerEnv;
    /// Starts (or leaves running) the user server in the private session; false if it cannot.
    bool startUserServer(PrivateSession &s, const QStringList &environment, int port);

    std::map<std::tuple<int, int, int, bool, bool>, PrivateSession> m_sessions;
    QString m_skip;
    QString m_renderNode;
};

void WorkerEndToEndTest::initTestCase()
{
    if (!getuid()) {
        m_skip = QStringLiteral("runs as the desktop user, not root");
        return;
    }
    for (const auto *tool : {"bwrap", "dbus-daemon", "pipewire", "wireplumber", "kwin_wayland", "kbuildsycoca6"}) {
        if (QStandardPaths::findExecutable(QString::fromLatin1(tool)).isEmpty()) {
            m_skip = QStringLiteral("%1 is not installed").arg(QString::fromLatin1(tool));
            return;
        }
    }
    // The node this host encodes on (else any); the session gets only that one, like a virtual desktop.
    m_renderNode = EncoderSupport::probeUncached().renderNode;
    if (!m_renderNode.startsWith(QStringLiteral("/dev/dri/renderD")) || !QFileInfo::exists(m_renderNode)) m_renderNode.clear();
    if (m_renderNode.isEmpty()) m_renderNode = RenderNodes::list().value(0);
    if (m_renderNode.isEmpty()) {
        m_skip = QStringLiteral("no render node: KWin needs one for OpenGL compositing and screencast");
        return;
    }
}

PrivateSession *WorkerEndToEndTest::session(int outputs, QSize size, bool motion, bool cursorClient)
{
    const auto key = std::tuple(outputs, size.width(), size.height(), motion, cursorClient);
    if (const auto it = m_sessions.find(key); it != m_sessions.end()) {
        if (!it->second.process || it->second.process->state() != QProcess::NotRunning) {
            return it->second.process ? &it->second : nullptr;
        }
        // AUD-FIX12: ended when rows of another size ran; start it again.
        if (it->second.runtime) it->second.runtime->remove();
        if (it->second.home) it->second.home->remove();
        m_sessions.erase(it);
    }
    // Sessions of another output size or motion are ended first (the rows run one size after another).
    for (auto &[other, running] : m_sessions) {
        if ((std::get<1>(other) != size.width() || std::get<2>(other) != size.height() || std::get<3>(other) != motion
             || std::get<4>(other) != cursorClient)
            && running.process
            && running.process->state() != QProcess::NotRunning) {
            running.process->terminate();
            if (!running.process->waitForFinished(5000)) {
                running.process->kill();
                running.process->waitForFinished(5000);
            }
        }
    }
    auto &s = m_sessions[key];
    const QString userRuntime = QStringLiteral("/run/user/%1").arg(getuid());
    // A short path: the Wayland and PipeWire socket names must fit sockaddr_un.
    const QString runtimeBase = QFileInfo(userRuntime).isDir() ? userRuntime : QDir::tempPath();
    s.runtime = std::make_unique<QTemporaryDir>(runtimeBase + QStringLiteral("/krdp-e2e-XXXXXX"));
    s.home = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/krdp-e2e-home-XXXXXX"));
    if (!s.runtime->isValid() || !s.home->isValid()
        || !QFile::setPermissions(s.runtime->path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        return nullptr;
    }
    for (const auto *dir : {"config", "data/applications", "cache", "state"}) {
        if (!QDir(s.home->path()).mkpath(QString::fromLatin1(dir))) return nullptr;
    }
    const QString runtime = s.runtime->path();
    const QString home = s.home->path();

    QFile bus(runtime + QStringLiteral("/bus.conf"));
    if (!bus.open(QIODevice::WriteOnly)) return nullptr;
    // No <servicedir>/<standard_session_servicedirs/>: nothing can be activated on this bus.
    bus.write(QStringLiteral("<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN\"\n"
                             " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                             "<busconfig><type>session</type><listen>unix:path=%1/bus</listen><auth>EXTERNAL</auth>\n"
                             "<apparmor mode=\"disabled\"/>\n" // inside the sandbox the bus cannot query AppArmor (read-only /sys); S3 needs to own a name
                             "<policy context=\"default\"><allow send_destination=\"*\"/><allow receive_sender=\"*\"/><allow own=\"*\"/></policy>\n"
                             "</busconfig>\n")
                  .arg(runtime)
                  .toUtf8());
    bus.close();
    // KWin grants the screencast and fake-input protocols to this exact worker executable.
    QFile desktop(home + QStringLiteral("/data/applications/io.github.westers.farside.consoleworker.desktop"));
    if (!desktop.open(QIODevice::WriteOnly)) return nullptr;
    desktop.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=KRDP Virtual Capture\nNoDisplay=true\nExec=%1\n"
                                 "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,org_kde_kwin_fake_input\n")
                      .arg(workerProgram())
                      .toUtf8());
    desktop.close();
    // The user server (OPT-055 K4 cases) likewise; written before kbuildsycoca6 runs at session start.
    QFile serverDesktop(home + QStringLiteral("/data/applications/io.github.westers.farside.server.desktop"));
    if (!serverDesktop.open(QIODevice::WriteOnly)) return nullptr;
    serverDesktop.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=Farside Server\nNoDisplay=true\nExec=%1\n"
                                       "X-KDE-Wayland-Interfaces=org_kde_kwin_fake_input,zkde_screencast_unstable_v1\n")
                            .arg(serverProgram())
                            .toUtf8());
    serverDesktop.close();

    // Built from nothing: no DISPLAY, WAYLAND_DISPLAY, session bus or PipeWire of the user's session.
    QProcessEnvironment env;
    const auto set = [&env](const char *name, const QString &value) {
        env.insert(QString::fromLatin1(name), value);
    };
    set("PATH", home + QStringLiteral("/tools:/usr/bin:/bin"));
    set("HOME", home);
    set("USER", qEnvironmentVariable("USER"));
    set("LOGNAME", qEnvironmentVariable("USER"));
    set("LANG", QStringLiteral("C.UTF-8"));
    set("XDG_RUNTIME_DIR", runtime);
    set("XDG_CONFIG_HOME", home + QStringLiteral("/config"));
    set("XDG_DATA_HOME", home + QStringLiteral("/data"));
    set("XDG_CACHE_HOME", home + QStringLiteral("/cache"));
    set("XDG_STATE_HOME", home + QStringLiteral("/state"));
    set("XDG_CONFIG_DIRS", QStringLiteral("/etc/xdg"));
    set("XDG_DATA_DIRS", QStringLiteral("/usr/local/share:/usr/share"));
    set("XDG_SESSION_TYPE", QStringLiteral("wayland"));
    set("XDG_CURRENT_DESKTOP", QStringLiteral("KDE"));
    set("XDG_MENU_PREFIX", QStringLiteral("plasma-")); // as the launcher: kbuildsycoca6 needs the applications menu
    set("DBUS_SESSION_BUS_ADDRESS", QStringLiteral("unix:path=%1/bus").arg(runtime));
    set("PIPEWIRE_RUNTIME_DIR", runtime);
    set("PIPEWIRE_REMOTE", QStringLiteral("pipewire-0"));
    set("PULSE_RUNTIME_PATH", runtime + QStringLiteral("/pulse"));
    set("PULSE_SERVER", QStringLiteral("unix:%1/pulse/native").arg(runtime));
    // Mesa, as the launcher sets for amdgpu/i915/xe (never the NVIDIA EGL vendor).
    set("__EGL_VENDOR_LIBRARY_FILENAMES", QStringLiteral("/usr/share/glvnd/egl_vendor.d/50_mesa.json"));
    set("__GLX_VENDOR_LIBRARY_NAME", QStringLiteral("mesa"));
    set("LIBVA_MESSAGING_LEVEL", QStringLiteral("1"));
    set("KRDP_RENDER_NODE", m_renderNode); // as the launcher exports the node it granted
    set("KRDP_SERVER_DIR", qEnvironmentVariable("KRDP_E2E_SERVER_DIR", QStringLiteral(KRDP_SERVER_DIR)));
    if (qEnvironmentVariableIsSet("LD_LIBRARY_PATH")) set("LD_LIBRARY_PATH", qEnvironmentVariable("LD_LIBRARY_PATH"));
    set("KRDP_CONSOLE_WORKER", workerProgram());
    set("KRDP_E2E_SERVER", serverProgram());
    set("KRDP_E2E_OUTPUTS", QString::number(outputs));
    set("KRDP_E2E_WIDTH", QString::number(size.width()));
    set("KRDP_E2E_HEIGHT", QString::number(size.height()));
    // AUD-FIX11: motion as on cray (testsrc2 at 30 fps); KRDP_E2E_MOTION=0 turns it off.
    if (motion && qEnvironmentVariable("KRDP_E2E_MOTION") != QLatin1String("0") && !QStandardPaths::findExecutable(QStringLiteral("ffplay")).isEmpty())
        set("KRDP_E2E_MOTION", QStringLiteral("1"));
    set("KRDP_E2E_FAKE_LOCKER", QStringLiteral(KRDP_E2E_FAKE_LOCKER));
    if (cursorClient) set("KRDP_E2E_CURSOR_CLIENT", QStringLiteral(KRDP_E2E_CURSOR_CLIENT));
    if (qEnvironmentVariableIsSet("KRDP_E2E_LOGGING_RULES")) set("QT_LOGGING_RULES", qEnvironmentVariable("KRDP_E2E_LOGGING_RULES"));
    if (qEnvironmentVariableIsSet("KRDP_E2E_MESSAGE_PATTERN")) set("QT_MESSAGE_PATTERN", qEnvironmentVariable("KRDP_E2E_MESSAGE_PATTERN"));

    QStringList deviceBindings;
    if (qEnvironmentVariable("KRDP_E2E_NVIDIA") == QStringLiteral("1")) {
        // Explicit acceptance fixture only: use the real NVIDIA compositor
        // devices with existing permissions, never a display/modesetting node.
        for (const auto *name : {"/dev/nvidia0", "/dev/nvidiactl", "/dev/nvidia-uvm"}) {
            if (!QFileInfo::exists(QString::fromLatin1(name))) return nullptr;
            deviceBindings << QStringLiteral("--dev-bind") << QString::fromLatin1(name) << QString::fromLatin1(name);
        }
        set("__EGL_VENDOR_LIBRARY_FILENAMES", QStringLiteral("/usr/share/glvnd/egl_vendor.d/10_nvidia.json"));
        set("__GLX_VENDOR_LIBRARY_NAME", QStringLiteral("nvidia"));
    }
    auto process = std::make_unique<QProcess>();
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(home);
    process->setStandardOutputFile(home + QStringLiteral("/session.log"));
    process->setStandardErrorFile(home + QStringLiteral("/session.log"), QIODevice::Append);
    process->start(QStandardPaths::findExecutable(QStringLiteral("bwrap")),
                   QStringList{QStringLiteral("--unshare-pid"), QStringLiteral("--die-with-parent"), QStringLiteral("--ro-bind"), QStringLiteral("/"), QStringLiteral("/"),
                    QStringLiteral("--proc"), QStringLiteral("/proc"), QStringLiteral("--dev"), QStringLiteral("/dev"), QStringLiteral("--dev-bind"),
                    m_renderNode, m_renderNode, QStringLiteral("--bind"), runtime, runtime, QStringLiteral("--bind"), home, home}
                    + deviceBindings + QStringList{QStringLiteral("/bin/bash"), QStringLiteral("-c"), QString::fromLatin1(SessionScript)});
    if (!process->waitForStarted(5000)) return nullptr;
    QElapsedTimer timer;
    timer.start();
    while (!QFileInfo::exists(runtime + QStringLiteral("/session-ready")) && process->state() == QProcess::Running && timer.elapsed() < 60000) {
        QTest::qWait(50);
    }
    s.process = std::move(process);
    if (!QFileInfo::exists(runtime + QStringLiteral("/session-ready"))) {
        qWarning().noquote() << "session.log:" << s.log(QStringLiteral("session.log")) << "\nkwin.log:" << s.log(QStringLiteral("kwin.log"));
        s.skip = s.process->state() != QProcess::Running && s.log(QStringLiteral("session.log")).contains(QStringLiteral("bwrap"))
            ? QStringLiteral("bwrap cannot create a sandbox here")
            : QStringLiteral("!the private headless session did not come up");
    }
    return &s;
}

bool WorkerEndToEndTest::startWorker(PrivateSession &s, bool virtualDesktop, ConsoleWorkerEndpoint &endpoint, WorkerRun &run)
{
    const QString runtime = s.runtime->path();
    const QString id = virtualDesktop ? QUuid::createUuid().toString(QUuid::WithoutBraces) : QStringLiteral("c1");
    QFile::remove(runtime + QStringLiteral("/worker-exit"));
    QFile args(runtime + QStringLiteral("/worker-args"));
    if (!args.open(QIODevice::WriteOnly)) return false;
    args.write((virtualDesktop ? QStringLiteral("--virtual-session\n%1\n") : QStringLiteral("--logind-session\n%1\n")).arg(id).toUtf8());
    args.close();
    QFile::remove(runtime + QStringLiteral("/worker-env"));
    if (!m_nextWorkerEnv.isEmpty()) {
        QFile workerEnv(runtime + QStringLiteral("/worker-env"));
        if (!workerEnv.open(QIODevice::WriteOnly)) return false;
        workerEnv.write(m_nextWorkerEnv.join(QLatin1Char('\n')).toUtf8() + '\n');
        workerEnv.close();
        m_nextWorkerEnv.clear();
    }
    const QByteArray token = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
    QFile tokenFile(runtime + QStringLiteral("/worker-token"));
    if (!tokenFile.open(QIODevice::WriteOnly) || !tokenFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return false;
    tokenFile.write(token);
    tokenFile.close();

    connect(&endpoint, &ConsoleWorkerEndpoint::encoderCapsReceived, this, [&run](const auto &value) {
        run.caps = value;
        run.order << QStringLiteral("caps");
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, this, [&run](const auto &) {
        run.order << QStringLiteral("ready");
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::encoderReported, this, [&run](const auto &report) {
        run.order << QStringLiteral("report:%1:%2").arg(int(report.codec)).arg(report.hardware ? QStringLiteral("hw") : QStringLiteral("sw"));
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [&run](const auto &) {
        ++run.outputs;
        if (!run.order.contains(QStringLiteral("outputs"))) run.order << QStringLiteral("outputs");
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [&run](const VideoFrame &frame) {
        run.frames.append(frame);
        if (frame.isKeyFrame) {
            ++run.keyframes;
            run.keyframeSize = frame.size;
            run.payloadMatches = h264KeyframeSize(frame.data) == std::optional(frame.size);
            if (!run.order.contains(QStringLiteral("keyframe"))) run.order << QStringLiteral("keyframe");
        }
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&run](const QString &message) {
        run.errors << message;
    });
    // The session's loop starts the worker as soon as this socket exists (virtual-session-desktop.sh).
    QString error;
    if (!endpoint.listen(runtime + QStringLiteral("/worker.sock"),
                         {virtualDesktop ? ConsoleSeat::Adapter::VirtualUser : ConsoleSeat::Adapter::PhysicalUser, id, quint32(getuid())}, token, &error)) {
        qWarning().noquote() << "listen:" << error;
        return false;
    }
    return true;
}

void WorkerEndToEndTest::stopWorker(PrivateSession &s, ConsoleWorkerEndpoint &endpoint)
{
    const QString exitFile = s.runtime->path() + QStringLiteral("/worker-exit");
    endpoint.stopWorker();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(exitFile), 15000);
    QFile exitCode(exitFile);
    QVERIFY(exitCode.open(QIODevice::ReadOnly));
    QCOMPARE(exitCode.readAll().trimmed(), QByteArray("0"));
}

void WorkerEndToEndTest::consoleCaptureSelection_data()
{
    QTest::addColumn<int>("captureMode");
    QTest::addColumn<bool>("virtualDesktop");
    using Mode = MonitorCapturePolicy::Mode;
    QTest::newRow("Console workspace") << int(Mode::Workspace) << false;
    QTest::newRow("Console primary") << int(Mode::Primary) << false;
    QTest::newRow("Console specific index1") << int(Mode::Specific) << false;
    QTest::newRow("Console independent screens") << int(Mode::Multi) << false;
    QTest::newRow("Virtual preserves retained layout") << int(Mode::Specific) << true;
}

void WorkerEndToEndTest::consoleCaptureSelection()
{
    QFETCH(int, captureMode);
    QFETCH(bool, virtualDesktop);
    if (!m_skip.isEmpty()) QSKIP(qPrintable(m_skip));
    auto *s = session(2);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    ConsoleWorkerEndpoint endpoint;
    WorkerRun run;
    ConsoleWorkerWire::Outputs outputs;
    std::optional<ConsoleWorkerWire::Topology> topology;
    connect(&endpoint, &ConsoleWorkerEndpoint::topologyReceived, this, [&](const auto &value) { topology = value; });
    connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [&](const auto &value) { outputs = value; });
    const auto logs = qScopeGuard([&] {
        if (QTest::currentTestFailed()) qWarning().noquote() << "worker.log:" << s->log(QStringLiteral("worker.log"), 12000)
            << "kwin.log:" << s->log(QStringLiteral("kwin.log"), 5000);
    });
    QVERIFY(startWorker(*s, virtualDesktop, endpoint, run));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    const auto reap = qScopeGuard([&] {
        if (!QFileInfo::exists(exitFile)) { endpoint.stopWorker(); (void)QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000); }
    });
    QTRY_VERIFY_WITH_TIMEOUT((endpoint.ready() && outputs.monitors.size() == 2) || !run.errors.isEmpty(), 45000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QVERIFY(endpoint.ready());
    QCOMPARE(outputs.monitors.size(), 2);
    const auto initial = outputs;
    const auto mode = MonitorCapturePolicy::Mode(captureMode);
    const bool selected = !virtualDesktop && (mode == MonitorCapturePolicy::Mode::Primary || mode == MonitorCapturePolicy::Mode::Specific);
    const bool workspace = !virtualDesktop && mode == MonitorCapturePolicy::Mode::Workspace;
    QString expectedName;
    if (selected) {
        if (mode == MonitorCapturePolicy::Mode::Specific) expectedName = initial.monitors[1].name;
        else for (const auto &screen : initial.monitors) if (screen.primary) expectedName = screen.name;
    }
    run.frames.clear();
    endpoint.setControlState({1, true});
    ConsoleWorkerWire::EncoderConfig config;
    config.generation = 1;
    config.settings = CodecPolicy::EncoderSettings{.hardware = false};
    config.capture = {mode, 1};
    QVERIFY(endpoint.setEncoderConfig(config));
    endpoint.requestKeyFrame();
    const auto validPacket = [&](const VideoFrame &frame) {
        return frame.isKeyFrame && frame.monitors.size() == (selected ? 1 : 2)
            && frame.size == QSize(workspace ? 2560 : 1280, 720)
            && h264KeyframeSize(frame.data) == std::optional(frame.size);
    };
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(run.frames.cbegin(), run.frames.cend(), validPacket) || !run.errors.isEmpty(), 45000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    const auto proof = std::find_if(run.frames.cbegin(), run.frames.cend(), validPacket);
    QVERIFY(proof != run.frames.cend());
    const auto proofFrame = *proof; // Event processing below may grow run.frames.
    ClientStyle::Decoder decoder;
    QVERIFY2(decoder.feed(1, VideoCodec::Avc420, proofFrame.data), qPrintable(decoder.error()));
    QCOMPARE(decoder.surfaces().value(1).lastPicture, proofFrame.size);
    QCOMPARE(outputs.monitors.size(), selected ? 1 : 2);
    if (selected) {
        QCOMPARE(outputs.monitors.first().name, expectedName);
        const auto original = std::find_if(initial.monitors.cbegin(), initial.monitors.cend(), [&](const auto &screen) { return screen.name == expectedName; });
        QVERIFY(original != initial.monitors.cend());
        QCOMPARE(outputs.compositorOrigin, initial.compositorOrigin + original->geometry.topLeft());
        QCOMPARE(proofFrame.monitors.first().geometry, QRect(0, 0, 1280, 720));
        QVERIFY(outputs.monitors.first().primary);
    }
    if (!virtualDesktop) {
        QVERIFY(endpoint.requestTopology());
        QTRY_VERIFY_WITH_TIMEOUT(topology.has_value() || !run.errors.isEmpty(), 15000);
        QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
        QVERIFY(topology);
        QCOMPARE(topology->outputs.size(), selected ? 1 : 2);
        QCOMPARE(topology->complete, !selected);
    }
    qInfo() << "Capture mode" << captureMode << "Virtual" << virtualDesktop << "outputs" << outputs.monitors.size()
            << "origin" << outputs.compositorOrigin << "decoded" << proofFrame.size;
    // Withdrawal discards the old owner's selection and restores full capture.
    run.frames.clear();
    endpoint.setControlState({2, false});
    endpoint.requestKeyFrame();
    QTRY_VERIFY_WITH_TIMEOUT((outputs.monitors.size() == 2 && std::any_of(run.frames.cbegin(), run.frames.cend(), [](const auto &frame) {
        return frame.isKeyFrame && frame.monitors.size() == 2 && frame.size == QSize(1280, 720);
    })) || !run.errors.isEmpty(), 45000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QCOMPARE(outputs.monitors.size(), 2);
    stopWorker(*s, endpoint);
}

// OPT-060 S2: a Replace attempt that cannot happen must restore, leave the host's outputs alone and
// still end in a normal Console picture; an unverified journal never means a black Console.
void WorkerEndToEndTest::consoleReplaceRelock_data()
{
    QTest::addColumn<int>("offsetMs");
    QTest::addColumn<bool>("alwaysKill");
    QTest::addColumn<bool>("realScan"); // OPT-060 D1: the worker scans a /proc-shaped tree whose comm is the kernel's 15 characters
    // The user's lock, relative to the start of the release (negative: before it, positive: after it).
    for (const int offset : {-2500, -2000, -1500, -1000, -500, 0, 500, 1000, 1500, 2000})
        QTest::newRow(qPrintable(QStringLiteral("lock %1 ms %2 the release").arg(qAbs(offset)).arg(offset < 0 ? QStringLiteral("before") : QStringLiteral("after")))) << offset << false << false;
    // Even with the release held, the greeter dies when the layout changes: only the re-lock can save the session.
    QTest::newRow("lock 500 ms before the release, greeter dies anyway") << -500 << true << false;
    QTest::newRow("lock 0 ms after the release, greeter dies anyway") << 0 << true << false;
    // D1: the same, but the greeter is found by the worker's real /proc scan (comm `kscreenlocker_g`, cmdline the real name).
    for (const int offset : {-2500, -1000, 500, 2000})
        QTest::newRow(qPrintable(QStringLiteral("real scan: lock %1 ms %2 the release").arg(qAbs(offset)).arg(offset < 0 ? QStringLiteral("before") : QStringLiteral("after")))) << offset << false << true;
    QTest::newRow("real scan: lock 500 ms before the release, greeter dies anyway") << -500 << true << true;
}

// OPT-060 S3 (OPT-049): lock around a Replace release, 10 interleavings. The lock screen is the fake above:
// the greeter dies if a lock lands within 2 s before the physical outputs come back. Pass = the session is
// locked with a live greeter at the end, the physical layout is restored and the worker exits 0.
void WorkerEndToEndTest::consoleReplaceRelock()
{
    QFETCH(int, offsetMs); QFETCH(bool, alwaysKill); QFETCH(bool, realScan);
    if (!m_skip.isEmpty()) QSKIP(qPrintable(m_skip));
    auto *s = session(2); QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    const QString runtime = s->runtime->path();
    const QString greeterFile = s->home->path() + QStringLiteral("/greeter-alive");
    // The stand-in ScreenSaver runs inside the fixture (see FakeScreenSaverHelper.cpp); commands go through files.
    struct Locker {
        QString dir; QString greeter;
        void send(const QString &command) const
        {
            // One command file at a time: wait until the helper has consumed the previous one.
            for (int i = 0; i < 200 && QFile::exists(dir + QStringLiteral("/cmd")); ++i) QTest::qWait(10);
            QFile tmp(dir + QStringLiteral("/cmd.tmp")); if (!tmp.open(QIODevice::WriteOnly)) return;
            tmp.write(command.toUtf8() + '\n'); tmp.close();
            QFile::rename(tmp.fileName(), dir + QStringLiteral("/cmd"));
        }
        QMap<QString, int> state() const
        {
            QMap<QString, int> values; QFile file(dir + QStringLiteral("/state"));
            if (!file.open(QIODevice::ReadOnly)) return values;
            for (const auto &part : QString::fromUtf8(file.readAll()).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                const auto pair = part.trimmed().split(QLatin1Char('='));
                if (pair.size() == 2) values.insert(pair[0], pair[1].toInt());
            }
            return values;
        }
        bool active() const { return state().value(QStringLiteral("active")) == 1; }
        bool greeterAlive() const { return state().value(QStringLiteral("greeter")) == 1; }
        int kills() const { return state().value(QStringLiteral("kills")); }
        int relockCalls() const { return state().value(QStringLiteral("relocks")); }
        // The user's own lock. Returns the monotonic time it was requested.
        void lockNow() { send(QStringLiteral("lock")); lockTimer.start(); }
        void killGreeter() { send(QStringLiteral("kill")); }
        qint64 lastLockMs() const { return lockTimer.isValid() ? lockTimer.elapsed() : -1; }
        QElapsedTimer lockTimer;
    } locker{runtime + QStringLiteral("/locker"), greeterFile};
    const QString exitFile = runtime + QStringLiteral("/worker-exit");
    const QString marker = runtime + QStringLiteral("/physical-baseline");
    ConsoleWorkerEndpoint endpoint; WorkerRun run;
    ConsoleWorkerWire::Outputs outputs;
    connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [&](const auto &value) { outputs = value; });
    const auto logs = qScopeGuard([&] {
        if (QTest::currentTestFailed()) qWarning().noquote() << "worker.log:" << s->log(QStringLiteral("worker.log"), 12000)
            << "fake-locker.log:" << s->log(QStringLiteral("fake-locker.log")) << "session.log:" << s->log(QStringLiteral("session.log"), 1500);
    });
    locker.send(QStringLiteral("enable"));
    QTRY_COMPARE_WITH_TIMEOUT(locker.state().value(QStringLiteral("enabled")), 1, 5000);
    locker.send(QStringLiteral("reset")); // every row starts unlocked with zero counters
    QTRY_VERIFY2_WITH_TIMEOUT(!locker.active() && !locker.greeterAlive() && locker.kills() == 0,
        qPrintable(QStringLiteral("locker state active=%1 greeter=%2 kills=%3").arg(locker.active()).arg(locker.greeterAlive()).arg(locker.kills())), 5000);
    QFile::remove(greeterFile);
    const qint64 logStart = QFileInfo(s->home->path() + QStringLiteral("/worker.log")).size(); // worker.log accumulates across rows
    if (realScan) {
        // A /proc-shaped tree: pid 4242's comm is a symlink to the greeter marker (present = alive; the fake locker writes
        // the kernel's truncated name into it) and its cmdline carries the real program name. The worker scans it like /proc.
        const QString procRoot = s->home->path() + QStringLiteral("/fakeproc");
        QVERIFY(QDir().mkpath(procRoot + QStringLiteral("/4242")));
        QFile::remove(procRoot + QStringLiteral("/4242/comm"));
        QVERIFY(QFile::link(greeterFile, procRoot + QStringLiteral("/4242/comm")));
        QFile cmdline(procRoot + QStringLiteral("/4242/cmdline"));
        QVERIFY(cmdline.open(QIODevice::WriteOnly));
        cmdline.write(QByteArray("/usr/lib/x86_64-linux-gnu/libexec/kscreenlocker_greet\0--graceTime\0" "5000\0", 71));
        cmdline.close();
        m_nextWorkerEnv = {QStringLiteral("KRDP_CONSOLE_LOCK_PROC_ROOT=") + procRoot};
    } else {
        m_nextWorkerEnv = {QStringLiteral("KRDP_CONSOLE_LOCK_GREETER_FILE=") + greeterFile};
    }
    QVERIFY(startWorker(*s, false, endpoint, run));
    const auto reap = qScopeGuard([&] {
        if (!QFileInfo::exists(exitFile)) { endpoint.stopWorker(); (void)QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 30000); }
        QFile::remove(marker);
    });
    QTRY_VERIFY_WITH_TIMEOUT((endpoint.ready() && outputs.monitors.size() == 2) || !run.errors.isEmpty(), 45000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    const auto initial = outputs;
    // The fixture's KWin names its genuine outputs Virtual-*; alias the two baseline names so the real
    // PhysicalOutputGuard disables and restores them (as consoleConfiguredOutputs does).
    const QString tools = s->home->path() + QStringLiteral("/tools");
    QVERIFY(QDir().mkpath(tools));
    QFile wrapper(tools + QStringLiteral("/kscreen-doctor"));
    QVERIFY(wrapper.open(QIODevice::WriteOnly));
    wrapper.write(R"PY(#!/usr/bin/python3
import json, os, pathlib, subprocess, sys
marker = pathlib.Path(os.environ['XDG_RUNTIME_DIR']) / 'physical-baseline'
aliases = json.loads(marker.read_text()) if marker.exists() else {}
args = []
for arg in sys.argv[1:]:
    for real, alias in aliases.items():
        prefix = 'output.' + alias + '.'
        if arg.startswith(prefix):
            arg = 'output.' + real + '.' + arg[len(prefix):]
            break
    args.append(arg)
result = subprocess.run(['/usr/bin/kscreen-doctor', *args], capture_output=True)
data = result.stdout
if '-j' in args and result.returncode == 0 and aliases:
    value = json.loads(data)
    for output in value.get('outputs', []):
        output['name'] = aliases.get(output.get('name'), output.get('name'))
    data = json.dumps(value).encode()
sys.stdout.buffer.write(data)
sys.stderr.buffer.write(result.stderr)
sys.exit(result.returncode)
)PY");
    wrapper.close(); QVERIFY(wrapper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QJsonObject aliases;
    for (int i = 0; i < initial.monitors.size(); ++i) aliases.insert(initial.monitors[i].name, QStringLiteral("DP-test-%1").arg(i));
    QFile aliasFile(marker); QVERIFY(aliasFile.open(QIODevice::WriteOnly)); aliasFile.write(QJsonDocument(aliases).toJson()); aliasFile.close();
    const auto query = [&]() -> std::optional<QByteArray> {
        QProcess command;
        auto environment = s->process->processEnvironment();
        environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-0"));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
        command.setProcessEnvironment(environment);
        command.start(tools + QStringLiteral("/kscreen-doctor"), {QStringLiteral("-j")});
        if (!command.waitForStarted(1000) || !command.waitForFinished(5000)) { command.kill(); command.waitForFinished(1000); return {}; }
        if (command.exitStatus() != QProcess::NormalExit || command.exitCode() != 0) return {};
        return command.readAllStandardOutput();
    };
    const auto baselineJson = query(); QVERIFY(baselineJson);
    const auto baseline = ConsoleVirtualOutputRestore::snapshot(*baselineJson, QStringLiteral("fixture")); QVERIFY(baseline);
    // Number of baseline (physical alias) outputs currently enabled.
    const auto physicalEnabled = [&]() -> int {
        const auto json = query(); if (!json) return -1;
        bool parsed = false;
        const auto current = OutputRestoreJournal::parseCurrent(*json, &parsed);
        if (!parsed) return -1;
        int enabled = 0;
        for (const auto &original : baseline->outputs)
            for (const auto &output : current) if (output.name == original.name && output.enabled) ++enabled;
        return enabled;
    };
    run.frames.clear(); endpoint.setControlState({1, true});
    ConsoleWorkerWire::EncoderConfig config; config.generation = 1;
    config.settings = CodecPolicy::EncoderSettings{.hardware = false};
    const ClientDisplay::Info client{QSize(1600, 900), {}};
    config.consoleVirtual = *ConsoleVirtualOutputPolicy::parse(true, QStringLiteral("replace"), QStringLiteral("client"), QSize(1600, 900), client);
    QVERIFY(endpoint.setEncoderConfig(config)); endpoint.requestKeyFrame();
    QTRY_VERIFY_WITH_TIMEOUT((outputs.monitors.size() == 1 && outputs.monitors.first().name.startsWith(QStringLiteral("Virtual-krdp-m")) && !run.frames.isEmpty())
        || !run.errors.isEmpty() || QFileInfo::exists(exitFile), 60000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QCOMPARE(physicalEnabled(), 0); // replaced: the physical outputs are off
    QTest::qWait(1500); // the worker has settled and sampled the (unlocked) screensaver

    // Timeline. T0 = the release starts (the control is withdrawn).
    QElapsedTimer sinceT0;
    if (offsetMs < 0) { locker.lockNow(); QTest::qWait(-offsetMs); }
    sinceT0.start();
    endpoint.setControlState({2, false});
    bool userLockDone = offsetMs < 0, restoreSeen = false;
    qint64 restoreAt = -1;
    QElapsedTimer total; total.start();
    while (!QFileInfo::exists(exitFile) && total.elapsed() < 90000) {
        if (!userLockDone && sinceT0.elapsed() >= offsetMs) { locker.lockNow(); userLockDone = true; }
        if (!restoreSeen && physicalEnabled() == baseline->outputs.size()) {
            restoreSeen = true; restoreAt = sinceT0.elapsed();
            // OPT-049: a lock younger than 2 s when the layout changes kills the greeter and nothing re-arms it.
            if (locker.active() && (alwaysKill || (locker.lastLockMs() >= 0 && locker.lastLockMs() < 2000))) locker.killGreeter();
        }
        QTest::qWait(60);
    }
    QVERIFY2(QFileInfo::exists(exitFile), "the worker exited");
    const qint64 exitAt = sinceT0.elapsed(); // the worker's whole release, including the lock check after the restore (D1)
    if (!userLockDone) locker.lockNow(); // the lock came after the worker was gone
    QVERIFY(restoreSeen);
    QFile code(exitFile); QVERIFY(code.open(QIODevice::ReadOnly)); QCOMPARE(code.readAll().trimmed(), QByteArray("0"));
    const auto restoredJson = query(); QVERIFY(restoredJson);
    QVERIFY(ConsoleVirtualOutputRestore::matches(*baseline, *restoredJson));
    QTRY_VERIFY2_WITH_TIMEOUT(locker.active(), qPrintable(QStringLiteral("the session is locked after the release; state active=%1 greeter=%2 kills=%3 relocks=%4")
        .arg(locker.active()).arg(locker.greeterAlive()).arg(locker.kills()).arg(locker.relockCalls())), 3000);
    QVERIFY2(locker.greeterAlive(), "the greeter is alive after the release");
    const qint64 logSize = QFileInfo(s->home->path() + QStringLiteral("/worker.log")).size();
    const QString log = s->log(QStringLiteral("worker.log"), int(logSize - logStart));
    if (qEnvironmentVariableIsSet("KRDP_E2E_SHOW_WORKER_LOG")) qInfo().noquote() << log.right(3500);
    qInfo().noquote() << "OPT-049 row: lock" << offsetMs << "ms vs release, restore at" << restoreAt << "ms, worker exit at" << exitAt << "ms, greeter kills" << locker.kills()
                      << "re-lock calls" << locker.relockCalls() << "worker re-lock lines" << log.count(QStringLiteral("Console release: re-locking"))
                      << "held" << log.contains(QStringLiteral("holding the release"));
}

// OPT-060 D2 probe: how long a FRESH worker (as after a mid-connection replacement) takes to deliver its first
// frame on a static screen, with and without the codec switch the broker sends right after Ready (avc420 starts,
// then hevc replaces it on the same node). The private compositor never repaints by itself, like an idle lock screen.
void WorkerEndToEndTest::consoleFreshWorkerFirstFrame_data()
{
    QTest::addColumn<bool>("switchToHevc");
    QTest::newRow("avc420 only") << false;
    QTest::newRow("avc420 first, then hevc (the broker's order)") << true;
}

void WorkerEndToEndTest::consoleFreshWorkerFirstFrame()
{
    QFETCH(bool, switchToHevc);
    if (!m_skip.isEmpty()) QSKIP(qPrintable(m_skip));
    auto *s = session(2); QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    QStringList timings;
    for (int trial = 0; trial < 3; ++trial) {
        ConsoleWorkerEndpoint endpoint; WorkerRun run;
        ConsoleWorkerWire::Outputs outputs;
        connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [&](const auto &value) { outputs = value; });
        QVERIFY(startWorker(*s, false, endpoint, run));
        const auto reap = qScopeGuard([&] {
            if (!QFileInfo::exists(exitFile)) { endpoint.stopWorker(); (void)QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 30000); }
        });
        QTRY_VERIFY_WITH_TIMEOUT((endpoint.ready() && outputs.monitors.size() == 2) || !run.errors.isEmpty(), 45000);
        QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
        run.frames.clear();
        QElapsedTimer sinceBind; sinceBind.start();
        endpoint.setControlState({1, true});
        ConsoleWorkerWire::EncoderConfig config{.generation = 1, .codec = VideoCodec::Avc420, .settings = CodecPolicy::EncoderSettings{.hardware = false}};
        QVERIFY(endpoint.setEncoderConfig(config));
        if (switchToHevc) {
            config.codec = VideoCodec::Hevc; config.settings = CodecPolicy::EncoderSettings{.hardware = true};
            QVERIFY(endpoint.setEncoderConfig(config));
        }
        endpoint.requestKeyFrame();
        const bool got = QTest::qWaitFor([&] { return !run.frames.isEmpty() || !run.errors.isEmpty(); }, 40000);
        timings << (got && run.errors.isEmpty() ? QStringLiteral("%1 ms").arg(sinceBind.elapsed()) : QStringLiteral("none in 40 s"));
        stopWorker(*s, endpoint);
    }
    qInfo().noquote() << "D2 probe: first frame after bind" << (switchToHevc ? "(avc420 then hevc)" : "(avc420)") << timings.join(QStringLiteral(", "));
}

void WorkerEndToEndTest::consoleReplaceFailsOpen_data()
{
    QTest::addColumn<bool>("unverifiedJournal");
    QTest::newRow("creation cannot be planned: error, nothing mutated, then normal capture") << false;
    QTest::newRow("journal unverified: no outputs created, then the lit outputs are captured, journal kept") << true;
}

void WorkerEndToEndTest::consoleReplaceFailsOpen()
{
    QFETCH(bool, unverifiedJournal);
    if (!m_skip.isEmpty()) QSKIP(qPrintable(m_skip));
    auto *s = session(2); QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    const QString journalDirectory = s->home->path() + QStringLiteral("/replace-fail-open");
    const QString journalPath = journalDirectory + QStringLiteral("/output-restore.json");
    QByteArray journalBytes;
    if (unverifiedJournal) {
        // A dead predecessor's conditional entry for an output that is not connected: it can be
        // neither restored nor discarded, so the replay keeps it ("kept").
        QVERIFY(QDir().mkpath(journalDirectory));
        OutputRestoreJournal::Entry entry;
        entry.owner = QString::fromLatin1(OutputRestoreJournal::ConsoleLeaseOwner);
        entry.pid = 2147483000; entry.session = QStringLiteral("3");
        OutputRestoreJournal::Output output;
        output.name = QStringLiteral("DP-not-connected");
        output.original.enabled = true; output.applied.mode = QStringLiteral("7");
        entry.outputs = {output};
        journalBytes = OutputRestoreJournal::serialize({entry});
        QFile journal(journalPath); QVERIFY(journal.open(QIODevice::WriteOnly)); journal.write(journalBytes); journal.close();
    }
    const auto query = [&]() -> std::optional<QByteArray> {
        QProcess command;
        auto environment = s->process->processEnvironment();
        environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-0"));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
        command.setProcessEnvironment(environment);
        command.start(QStringLiteral("/usr/bin/kscreen-doctor"), {QStringLiteral("-j")});
        if (!command.waitForStarted(1000) || !command.waitForFinished(5000)) { command.kill(); command.waitForFinished(1000); return {}; }
        if (command.exitStatus() != QProcess::NormalExit || command.exitCode() != 0) return {};
        return command.readAllStandardOutput();
    };
    const auto logs = qScopeGuard([&] {
        if (QTest::currentTestFailed()) qWarning().noquote() << "worker.log:" << s->log(QStringLiteral("worker.log"), 18000)
            << "kwin.log:" << s->log(QStringLiteral("kwin.log"), 5000);
    });
    const auto before = query(); QVERIFY(before);
    const auto beforeOutputs = OutputSnapshot::parse(*before);
    QCOMPARE(beforeOutputs.size(), 2);
    const auto environment = [&] {
        return unverifiedJournal ? QStringList{QStringLiteral("FARSIDE_OUTPUT_RESTORE_JOURNAL=") + journalPath} : QStringList{};
    };

    // Phase 1: a worker asked for Replace. It must fail before mutating anything.
    {
        ConsoleWorkerEndpoint endpoint; WorkerRun run;
        ConsoleWorkerWire::Outputs outputs;
        connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [&](const auto &value) { outputs = value; });
        m_nextWorkerEnv = environment();
        QVERIFY(startWorker(*s, false, endpoint, run));
        const auto reap = qScopeGuard([&] {
            if (!QFileInfo::exists(exitFile)) { endpoint.stopWorker(); (void)QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 30000); }
        });
        QTRY_VERIFY_WITH_TIMEOUT((endpoint.ready() && outputs.monitors.size() == 2) || !run.errors.isEmpty(), 45000);
        QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n')))); // Ready: never exit 1 on an unverified journal.
        run.errors.clear();
        endpoint.setControlState({1, true});
        ConsoleWorkerWire::EncoderConfig config; config.generation = 1;
        config.settings = CodecPolicy::EncoderSettings{.hardware = false};
        // Sixteen client monitors plus the two present outputs exceed the sixteen-output limit, so the
        // plan is unavailable before any output is created. The unverified journal refuses even a valid plan.
        ClientDisplay::Info client{QSize(1600, 900), {}};
        if (!unverifiedJournal) {
            QVector<VideoMonitor> monitors;
            for (int i = 0; i < 16; ++i) monitors.append({QRect((i % 4) * 1280, (i / 4) * 720, 1280, 720), i == 0});
            client = {QSize(5120, 2880), monitors};
        }
        const auto policy = ConsoleVirtualOutputPolicy::parse(true, QStringLiteral("replace"), QStringLiteral("client"), QSize(1600, 900), client);
        QVERIFY(policy);
        config.consoleVirtual = *policy;
        QVERIFY(endpoint.setEncoderConfig(config)); endpoint.requestKeyFrame();
        // The worker gives up before mutating anything and exits non-zero (the broker's latch then
        // starts a replacement with the policy off). Its reason is in the log; the Error frame is
        // best effort because the socket closes right behind it.
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(exitFile), 60000);
        QFile exitCode(exitFile); QVERIFY(exitCode.open(QIODevice::ReadOnly));
        QVERIFY2(exitCode.readAll().trimmed() != QByteArray("0"), "a failed Replace attempt is not a clean exit");
        const QString log = s->log(QStringLiteral("worker.log"), 60000);
        QVERIFY2(unverifiedJournal ? log.contains(QStringLiteral("output recovery is unverified"))
                                   : log.contains(QStringLiteral("requested Console output layout is unavailable")), qPrintable(log.right(3000)));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(exitFile), 30000);
    }
    const auto after = query(); QVERIFY(after);
    QCOMPARE(OutputSnapshot::parse(*after), beforeOutputs); // restored: nothing was created, enabled or moved

    // Phase 2: the replacement worker (the broker's latch turns the policy off) captures normally.
    {
        ConsoleWorkerEndpoint endpoint; WorkerRun run;
        ConsoleWorkerWire::Outputs outputs;
        connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [&](const auto &value) { outputs = value; });
        m_nextWorkerEnv = environment();
        QVERIFY(startWorker(*s, false, endpoint, run));
        const auto reap = qScopeGuard([&] {
            if (!QFileInfo::exists(exitFile)) { endpoint.stopWorker(); (void)QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 30000); }
        });
        QTRY_VERIFY_WITH_TIMEOUT((endpoint.ready() && outputs.monitors.size() == 2) || !run.errors.isEmpty(), 45000);
        QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
        run.frames.clear();
        endpoint.setControlState({1, true});
        ConsoleWorkerWire::EncoderConfig config; config.generation = 1;
        config.settings = CodecPolicy::EncoderSettings{.hardware = false};
        QVERIFY(endpoint.setEncoderConfig(config)); endpoint.requestKeyFrame();
        const auto picture = [&](const VideoFrame &frame) {
            return frame.isKeyFrame && frame.monitors.size() == 2 && frame.size == QSize(1280, 720)
                && h264KeyframeSize(frame.data) == std::optional(frame.size);
        };
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(run.frames.cbegin(), run.frames.cend(), picture) || !run.errors.isEmpty(), 45000);
        QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
        const auto frame = *std::find_if(run.frames.cbegin(), run.frames.cend(), picture);
        ClientStyle::Decoder decoder;
        QVERIFY2(decoder.feed(1, VideoCodec::Avc420, frame.data), qPrintable(decoder.error()));
        QCOMPARE(decoder.surfaces().value(1).lastPicture, frame.size);
        stopWorker(*s, endpoint);
    }
    if (unverifiedJournal) {
        QFile journal(journalPath); QVERIFY(journal.open(QIODevice::ReadOnly));
        QCOMPARE(journal.readAll(), journalBytes); // kept untouched for the next attempt
        const QString log = s->log(QStringLiteral("worker.log"), 40000);
        QVERIFY2(log.contains(QStringLiteral("Output recovery remains unverified")), "the unverified journal was reported");
    }
}

void WorkerEndToEndTest::consoleConfiguredOutputs_data()
{
    QTest::addColumn<int>("count");
    QTest::addColumn<bool>("virtualDesktop");
    QTest::addColumn<bool>("physicalAliases");
    QTest::addColumn<bool>("replace");
    QTest::addColumn<bool>("withdraw");
    QTest::addColumn<int>("codecId");
    QTest::newRow("Console single extend with foreign outputs, withdraw") << 1 << false << false << false << true << int(VideoCodec::Avc420);
    QTest::newRow("Console two extend with physical fixture, stop") << 2 << false << true << false << false << int(VideoCodec::Avc420);
    QTest::newRow("Console two replace with physical fixture, withdraw") << 2 << false << true << true << true << int(VideoCodec::Avc420);
    QTest::newRow("Console single replace with physical fixture, stop") << 1 << false << true << true << false << int(VideoCodec::Avc420);
    QTest::newRow("Virtual ignores Console temporary-output policy") << 2 << true << false << true << false << int(VideoCodec::Avc420);
    QTest::newRow("HEVC Console single extend, resize Fit withdraw") << 1 << false << false << false << true << int(VideoCodec::Hevc);
    QTest::newRow("HEVC Console two extend, resize Fit stop") << 2 << false << false << false << false << int(VideoCodec::Hevc);
}

void WorkerEndToEndTest::consoleConfiguredOutputs()
{
    QFETCH(int, count); QFETCH(int, codecId);
    const auto codec = VideoCodec(codecId); QFETCH(bool, virtualDesktop); QFETCH(bool, physicalAliases); QFETCH(bool, replace); QFETCH(bool, withdraw);
    if (!m_skip.isEmpty()) QSKIP(qPrintable(m_skip));
    auto *s = session(2); QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    ConsoleWorkerEndpoint endpoint; WorkerRun run;
    QVector<ConsoleWorkerWire::EncoderReport> reports;
    connect(&endpoint, &ConsoleWorkerEndpoint::encoderReported, this, [&](const auto &report) { reports.append(report); });
    const auto verifiedHardware = [&] {
        return codec != VideoCodec::Hevc || (std::any_of(reports.cbegin(), reports.cend(), [&](const auto &report) {
            return report.event == ConsoleWorkerWire::EncoderReport::Event::Backend && report.codec == codec && report.hardware;
        }) && std::none_of(reports.cbegin(), reports.cend(), [&](const auto &report) {
            return report.event == ConsoleWorkerWire::EncoderReport::Event::Backend && report.codec == codec && !report.hardware;
        }));
    };
    ConsoleWorkerWire::Outputs outputs;
    std::optional<ConsoleWorkerWire::Topology> topology;
    std::optional<ConsoleWorkerWire::PhysicalLeaseReleased> released;
    connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [&](const auto &value) { outputs = value; });
    connect(&endpoint, &ConsoleWorkerEndpoint::topologyReceived, this, [&](const auto &value) { topology = value; });
    connect(&endpoint, &ConsoleWorkerEndpoint::physicalLeaseReleased, this, [&](const auto &value) { released = value; });
    const auto logs = qScopeGuard([&] {
        if (QTest::currentTestFailed()) qWarning().noquote() << "worker.log:" << s->log(QStringLiteral("worker.log"), 18000)
            << "kwin.log:" << s->log(QStringLiteral("kwin.log"), 5000);
    });
    QVERIFY(startWorker(*s, virtualDesktop, endpoint, run));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    const QString marker = s->runtime->path() + QStringLiteral("/physical-baseline");
    const auto reap = qScopeGuard([&] {
        if (!QFileInfo::exists(exitFile)) { endpoint.stopWorker(); (void)QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 30000); }
        QFile::remove(marker);
    });
    QTRY_VERIFY_WITH_TIMEOUT((endpoint.ready() && outputs.monitors.size() == 2) || !run.errors.isEmpty(), 45000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QVERIFY(endpoint.ready()); const auto initial = outputs;
    // The private compositor has genuine KWin outputs, but names them Virtual.
    // Alias only its two baseline connector names in this fixture so the real
    // PhysicalOutputGuard can disable/restore them. Creation, KScreen commands,
    // encoded streams and release remain real. No production classification is
    // changed; this is compositor lifecycle evidence, not hardware acceptance.
    if (physicalAliases) {
        const QString tools = s->home->path() + QStringLiteral("/tools");
        QVERIFY(QDir().mkpath(tools));
        QFile wrapper(tools + QStringLiteral("/kscreen-doctor"));
        QVERIFY(wrapper.open(QIODevice::WriteOnly));
        wrapper.write(R"PY(#!/usr/bin/python3
import json, os, pathlib, subprocess, sys
marker = pathlib.Path(os.environ['XDG_RUNTIME_DIR']) / 'physical-baseline'
aliases = json.loads(marker.read_text()) if marker.exists() else {}
args = []
for arg in sys.argv[1:]:
    for real, alias in aliases.items():
        prefix = 'output.' + alias + '.'
        if arg.startswith(prefix):
            arg = 'output.' + real + '.' + arg[len(prefix):]
            break
    args.append(arg)
result = subprocess.run(['/usr/bin/kscreen-doctor', *args], capture_output=True)
data = result.stdout
if '-j' in args and result.returncode == 0 and aliases:
    value = json.loads(data)
    for output in value.get('outputs', []):
        output['name'] = aliases.get(output.get('name'), output.get('name'))
    data = json.dumps(value).encode()
sys.stdout.buffer.write(data)
sys.stderr.buffer.write(result.stderr)
sys.exit(result.returncode)
)PY");
        wrapper.close(); QVERIFY(wrapper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QJsonObject aliases;
        for (int i = 0; i < initial.monitors.size(); ++i) aliases.insert(initial.monitors[i].name, QStringLiteral("DP-test-%1").arg(i));
        QFile file(marker); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(aliases).toJson()); file.close();
    }
    const auto query = [&]() -> std::optional<QByteArray> {
        QProcess command;
        auto environment = s->process->processEnvironment();
        environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-0"));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
        command.setProcessEnvironment(environment);
        const QString wrapper = s->home->path() + QStringLiteral("/tools/kscreen-doctor");
        command.start(QFileInfo::exists(wrapper) ? wrapper : QStringLiteral("/usr/bin/kscreen-doctor"), {QStringLiteral("-j")});
        if (!command.waitForStarted(1000) || !command.waitForFinished(5000)) {
            command.kill(); command.waitForFinished(1000); return {};
        }
        if (command.exitStatus() != QProcess::NormalExit || command.exitCode() != 0) return {};
        return command.readAllStandardOutput();
    };
    const auto baselineJson = query(); QVERIFY(baselineJson);
    const auto baseline = ConsoleVirtualOutputRestore::snapshot(*baselineJson, QStringLiteral("fixture")); QVERIFY(baseline);
    run.frames.clear(); endpoint.setControlState({1, true});
    ConsoleWorkerWire::EncoderConfig config; config.generation = 1;
    config.codec = codec;
    config.settings = CodecPolicy::EncoderSettings{.hardware = codec == VideoCodec::Hevc};
    if (codec == VideoCodec::Hevc) { QVERIFY(run.caps); QVERIFY(run.caps->encoders.hevc.hardware); }
    reports.clear();
    ClientDisplay::Info client{QSize(1600, 900), {}};
    if (count == 2) client = {QSize(2560, 720), {{QRect(0, 0, 1280, 720), true}, {QRect(1280, 0, 1280, 720), false}}};
    config.consoleVirtual = *ConsoleVirtualOutputPolicy::parse(true, replace ? QStringLiteral("replace") : QStringLiteral("extend"),
        QStringLiteral("client"), QSize(1600, 900), client);
    QVERIFY(endpoint.setEncoderConfig(config)); endpoint.requestKeyFrame();
    const auto expected = [&] {
        return outputs.monitors.size() == (virtualDesktop ? 2 : count)
            && std::all_of(outputs.monitors.cbegin(), outputs.monitors.cend(), [&](const auto &output) {
                return virtualDesktop ? std::any_of(initial.monitors.cbegin(), initial.monitors.cend(), [&](const auto &old) { return old.name == output.name; })
                    : output.name.startsWith(QStringLiteral("Virtual-krdp-m"));
            });
    };
    QTRY_VERIFY_WITH_TIMEOUT((expected() && !run.frames.isEmpty()) || !run.errors.isEmpty() || QFileInfo::exists(exitFile), 60000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n')))); QVERIFY(expected());
    const auto isProof = [&](const auto &frame) {
        return frame.isKeyFrame && frame.codec == codec && frame.size == QSize(!virtualDesktop && count == 1 ? 1600 : 1280, !virtualDesktop && count == 1 ? 900 : 720)
            && frame.monitors.size() == (virtualDesktop ? 2 : count);
    };
    endpoint.requestKeyFrame();
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(outputs.monitors.cbegin(), outputs.monitors.cend(), [&](const auto &output) {
        const auto index = std::distance(outputs.monitors.cbegin(), std::find_if(outputs.monitors.cbegin(), outputs.monitors.cend(),
            [&](const auto &candidate) { return candidate.name == output.name; }));
        return std::any_of(run.frames.cbegin(), run.frames.cend(), [&](const auto &frame) { return isProof(frame) && frame.monitorIndex == index; });
    }), 15000);
    const auto proof = std::find_if(run.frames.cbegin(), run.frames.cend(), isProof);
    QVERIFY(proof != run.frames.cend()); const auto proofFrame = *proof;
    ClientStyle::Decoder decoder;
    for (int index = 0; index < (virtualDesktop ? 2 : count); ++index) {
        const auto packet = std::find_if(run.frames.cbegin(), run.frames.cend(), [&](const auto &frame) {
            return frame.isKeyFrame && frame.codec == codec && frame.monitorIndex == index && frame.size == proofFrame.size
                && frame.monitors == proofFrame.monitors;
        });
        QVERIFY(packet != run.frames.cend());
        QVERIFY2(decoder.feed(index + 1, codec, packet->data), qPrintable(decoder.error()));
        QCOMPARE(decoder.surfaces().value(index + 1).lastPicture, packet->size);
        QVERIFY(verifiedHardware());
    }
    if (!virtualDesktop) {
        const auto activeJson = query(); QVERIFY(activeJson);
        bool parsed = false;
        const auto active = OutputRestoreJournal::parseCurrent(*activeJson, &parsed); QVERIFY(parsed);
        QCOMPARE(active.size(), baseline->outputs.size() + count);
        for (const auto &original : baseline->outputs) {
            const auto actual = std::find_if(active.cbegin(), active.cend(), [&](const auto &output) { return output.name == original.name; });
            QVERIFY(actual != active.cend()); QCOMPARE(actual->enabled, !(physicalAliases && replace));
        }
        QFile held(s->home->path() + QStringLiteral("/state/farside/output-restore.json"));
        QVERIFY(held.open(QIODevice::ReadOnly));
        const auto entries = OutputRestoreJournal::parse(held.readAll()); QVERIFY(entries);
        QVERIFY(std::any_of(entries->cbegin(), entries->cend(), [](const auto &entry) {
            return entry.owner == QString::fromLatin1(OutputRestoreJournal::ConsoleVirtualOwner);
        }));
        QVERIFY(endpoint.requestTopology());
        QTRY_VERIFY_WITH_TIMEOUT(topology.has_value() || !run.errors.isEmpty(), 15000);
        QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n')))); QVERIFY(topology);
        QVERIFY(!topology->complete); QCOMPARE(topology->outputs.size(), count);
        QVERIFY(std::all_of(topology->outputs.cbegin(), topology->outputs.cend(), [](const auto &output) { return !output.physical; }));
        ConsoleVirtualOutputPlan::Plan owned;
        for (const auto &output : outputs.monitors) {
            owned.outputs.append({output.name, {}, {}, {}, output.primary});
        }
        const auto projected = ConsoleVirtualOutputReadback::parse(*activeJson, QStringLiteral("fixture"), owned);
        QVERIFY(projected);
        QHash<quint64, ConsoleWorkerWire::ResizeResult> resizeResults;
        QHash<quint64, ConsoleWorkerWire::ManagedFitResult> fitResults;
        QObject mutationObserver;
        connect(&endpoint, &ConsoleWorkerEndpoint::resizeFinished, &mutationObserver, [&](const auto &value) { resizeResults.insert(value.requestId, value); });
        connect(&endpoint, &ConsoleWorkerEndpoint::managedFitFinished, &mutationObserver, [&](const auto &value) { fitResults.insert(value.requestId, value); });
        const QString selected = projected->outputs.last().backendKey;
        // A stale controller and a baseline (foreign) output confer no authority.
        QVERIFY(endpoint.resize({101, 9, selected, QSize(960, 540), 1.25}));
        QTRY_VERIFY_WITH_TIMEOUT(resizeResults.contains(101), 5000);
        QVERIFY(!resizeResults[101].error.isEmpty()); QCOMPARE(resizeResults[101].generation, quint64(9));
        QVERIFY(endpoint.resize({102, 1, baseline->outputs.first().name, QSize(960, 540), 1.25}));
        QTRY_VERIFY_WITH_TIMEOUT(resizeResults.contains(102), 5000);
        QVERIFY(!resizeResults[102].error.isEmpty());
        const auto untouched = query(); QVERIFY(untouched);
        const auto activeSnapshot = ConsoleVirtualOutputRestore::snapshot(*activeJson, QStringLiteral("fixture")); QVERIFY(activeSnapshot);
        QVERIFY(ConsoleVirtualOutputRestore::matches(*activeSnapshot, *untouched));

        const auto freshDecodedOutputs = [&](const RetainedKScreenReadback::Snapshot &expected) {
            for (int index = 0; index < expected.outputs.size(); ++index) {
                const auto found = std::find_if(run.frames.cbegin(), run.frames.cend(), [&](const auto &frame) {
                    return frame.isKeyFrame && frame.codec == codec && frame.monitorIndex == index && frame.size == expected.outputs[index].nativePixels
                        && frame.monitors.size() == expected.outputs.size();
                });
                if (found == run.frames.cend()) return false;
            }
            return true;
        };
        const auto decodeFresh = [&](const RetainedKScreenReadback::Snapshot &expected) {
            ClientStyle::Decoder fresh;
            for (int index = 0; index < expected.outputs.size(); ++index) {
                const auto packet = std::find_if(run.frames.cbegin(), run.frames.cend(), [&](const auto &frame) {
                    return frame.isKeyFrame && frame.codec == codec && frame.monitorIndex == index && frame.size == expected.outputs[index].nativePixels
                        && frame.monitors.size() == expected.outputs.size();
                });
                if (packet == run.frames.cend() || !fresh.feed(index + 1, codec, packet->data)
                    || fresh.surfaces().value(index + 1).lastPicture != packet->size) return false;
            }
            return true;
        };
        run.frames.clear();
        QVERIFY(endpoint.resize({103, 1, selected, QSize(960, 540), 1.25}));
        QTRY_VERIFY_WITH_TIMEOUT(resizeResults.contains(103) || !run.errors.isEmpty() || QFileInfo::exists(exitFile), 30000);
        QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n')))); QVERIFY(resizeResults.contains(103));
        QVERIFY2(resizeResults[103].error.isEmpty(), qPrintable(resizeResults[103].error));
        const auto resizedJson = query(); QVERIFY(resizedJson);
        QVERIFY(ConsoleVirtualOutputMutation::preservesForeign(*activeJson, *resizedJson, QStringLiteral("fixture"), owned));
        const auto resized = ConsoleVirtualOutputReadback::parse(*resizedJson, QStringLiteral("fixture"), owned); QVERIFY(resized);
        QCOMPARE(resized->outputs.last().nativePixels, QSize(960, 540)); QCOMPARE(resized->outputs.last().scale, 1.25);
        QTRY_VERIFY_WITH_TIMEOUT(freshDecodedOutputs(*resized), 15000); QVERIFY(decodeFresh(*resized)); QVERIFY(verifiedHardware());

        ConsoleWorkerWire::ManagedFit fit{104, 1, projected->outputs.first().backendKey, QSize(1024, 768), 1, {}};
        if (count == 2) fit.relations.append({fit.output, selected, 1, 0});
        run.frames.clear(); reports.clear(); QVERIFY(endpoint.managedFit(fit));
        QTRY_VERIFY_WITH_TIMEOUT(fitResults.contains(104) || !run.errors.isEmpty() || QFileInfo::exists(exitFile), 30000);
        QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n')))); QVERIFY(fitResults.contains(104));
        QVERIFY2(fitResults[104].error.isEmpty(), qPrintable(fitResults[104].error));
        const auto fitJson = query(); QVERIFY(fitJson);
        QVERIFY(ConsoleVirtualOutputMutation::preservesForeign(*activeJson, *fitJson, QStringLiteral("fixture"), owned));
        const auto fitted = ConsoleVirtualOutputReadback::parse(*fitJson, QStringLiteral("fixture"), owned); QVERIFY(fitted);
        QCOMPARE(fitted->outputs.first().nativePixels, QSize(1024, 768)); QCOMPARE(fitted->outputs.first().scale, 1.0);
        QCOMPARE(fitted->outputs.first().logicalGeometry.topLeft(), projected->outputs.first().logicalGeometry.topLeft());
        if (count == 2) {
            QCOMPARE(fitted->outputs.last().nativePixels, QSize(960, 540)); QCOMPARE(fitted->outputs.last().scale, 1.25);
            QCOMPARE(fitted->outputs.last().logicalGeometry.topLeft(), fitted->outputs.first().logicalGeometry.topLeft() + QPoint(1024, 0));
        }
        QTRY_VERIFY_WITH_TIMEOUT(freshDecodedOutputs(*fitted), 15000); QVERIFY(decodeFresh(*fitted)); QVERIFY(verifiedHardware());
        topology.reset(); QVERIFY(endpoint.requestTopology());
        QTRY_VERIFY_WITH_TIMEOUT(topology.has_value(), 10000); QVERIFY(!topology->complete);
        QCOMPARE(topology->outputs.size(), count);
        for (int i = 0; i < count; ++i) {
            QCOMPARE(topology->outputs[i].logical, fitted->outputs[i].logicalGeometry);
            QCOMPARE(topology->outputs[i].pixels, fitted->outputs[i].nativePixels);
            QCOMPARE(topology->outputs[i].scale, fitted->outputs[i].scale);
        }
    } else {
        QTest::qWait(1000); QVERIFY(expected());
    }
    if (withdraw) endpoint.setControlState({2, false});
    else endpoint.stopWorker();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(exitFile), 30000);
    const auto restoredJson = query(); QVERIFY(restoredJson);
    QVERIFY(ConsoleVirtualOutputRestore::matches(*baseline, *restoredJson));
    if (!virtualDesktop) {
        QVERIFY(released); QVERIFY(released->verified); QCOMPARE(released->controlGeneration, quint64(1));
        QFile journal(s->home->path() + QStringLiteral("/state/farside/output-restore.json"));
        if (journal.exists()) {
            QVERIFY(journal.open(QIODevice::ReadOnly));
            const auto entries = OutputRestoreJournal::parse(journal.readAll()); QVERIFY(entries); QVERIFY(entries->isEmpty());
        }
    }
    QFile code(exitFile); QVERIFY(code.open(QIODevice::ReadOnly)); QCOMPARE(code.readAll().trimmed(), QByteArray("0"));
    qInfo() << "Configured Console outputs:" << count << "physical fixture" << physicalAliases << "replace" << replace
            << "codec" << int(codec) << "HEVC hardware verified" << (codec == VideoCodec::Hevc && verifiedHardware()) << "Virtual" << virtualDesktop << "release" << (released ? released->verified : true) << "decoded" << proofFrame.size;
}

void WorkerEndToEndTest::workerReachesReadyAndDeliversFrames_data()
{
    QTest::addColumn<bool>("virtualDesktop");
    QTest::newRow("virtual desktop (:3395)") << true;
    QTest::newRow("console session (:3391)") << false;
}

void WorkerEndToEndTest::workerReachesReadyAndDeliversFrames()
{
    QFETCH(bool, virtualDesktop);
    if (!m_skip.isEmpty()) {
        QSKIP(qPrintable(m_skip));
    }
    auto *s = session(1);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));

    ConsoleWorkerEndpoint endpoint;
    WorkerRun run;
    connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, this, [&endpoint](const auto &) {
        endpoint.setControlState({1, true});
        endpoint.requestKeyFrame();
    });
    const auto dumpLogs = qScopeGuard([&] {
        if (!QTest::currentTestFailed()) return;
        qWarning().noquote() << "worker.log:\n" << s->log(QStringLiteral("worker.log"), 4000) << "\nkwin.log:\n" << s->log(QStringLiteral("kwin.log"), 2000)
                             << "\nsession.log:\n" << s->log(QStringLiteral("session.log"), 1000) << "\nsycoca.log:\n" << s->log(QStringLiteral("sycoca.log"), 1500);
    });
    QVERIFY(startWorker(*s, virtualDesktop, endpoint, run));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    // Whatever failed, the session's loop must be free for the next worker.
    const auto reap = qScopeGuard([&] {
        if (QFileInfo::exists(exitFile)) return;
        endpoint.stopWorker();
        if (!QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000)) qWarning("the worker did not stop");
    });

    QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready() || !run.errors.isEmpty() || QFileInfo::exists(exitFile), 45000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QVERIFY2(endpoint.ready(), "the real worker never confirmed capture");
    QTRY_VERIFY_WITH_TIMEOUT(run.keyframes >= 1 || !run.errors.isEmpty() || QFileInfo::exists(exitFile), 30000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QVERIFY(run.keyframes >= 1);
    QCOMPARE(run.keyframeSize, QSize(1280, 720));
    QVERIFY(run.payloadMatches);
    qInfo().noquote() << "Real worker order:" << run.order.join(QStringLiteral(", ")) << "-" << run.frames.size() << "frames," << run.keyframes
                      << "keyframes on" << m_renderNode;
    // Hello (authentication), EncoderCaps, Ready, then everything else - and no pre-Ready failure.
    QCOMPARE(run.order.value(0), QStringLiteral("caps"));
    QCOMPARE(run.order.value(1), QStringLiteral("ready"));
    QCOMPARE(run.order.count(QStringLiteral("caps")), 1);
    QCOMPARE(run.order.count(QStringLiteral("ready")), 1);
    QVERIFY(run.caps);
    if (EncoderSupport::probeUncached().encoders.avc.hardware) {
        // B3: the worker's own probe, inside the sandbox, sees the host's hardware encoder.
        QVERIFY2(run.caps->encoders.avc.hardware, "the sandboxed worker's probe found no hardware encoder");
        QCOMPARE(run.caps->renderNode, m_renderNode);
    }
    // AUD-FIX10: a client's Refresh Rect reaches the worker as this keyframe request (the brokers
    // forward VideoStream::keyFrameRequested). On an idle desktop - no frame for 1 s - it must
    // still produce a keyframe of the output (KPipeWire re-encodes the last picture).
    qsizetype quietFrom = run.frames.size();
    QElapsedTimer quiet;
    quiet.start();
    QElapsedTimer settle;
    settle.start();
    while (quiet.elapsed() < 1000 && settle.elapsed() < 15000) {
        QTest::qWait(50);
        if (run.frames.size() != quietFrom) {
            quietFrom = run.frames.size();
            quiet.restart();
        }
    }
    QVERIFY2(quiet.elapsed() >= 1000, "the virtual desktop never went idle");
    endpoint.requestKeyFrame();
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(run.frames.cbegin() + quietFrom, run.frames.cend(), [](const VideoFrame &frame) {
        return frame.isKeyFrame && frame.size == QSize(1280, 720) && h264KeyframeSize(frame.data) == std::optional(frame.size);
    }), 2000);
    stopWorker(*s, endpoint);
}

void WorkerEndToEndTest::codecSwitchAtAttach_data()
{
    // AUD-FIX9 R1: a client attaches to a worker that is Ready and idle, and its codec policy
    // moves the encoders from AVC to a hardware HEVC/AV1 at once ("immediate": in the same read as
    // the new grant, as on ace and cray) or after the grant's capture refresh has published its
    // outputs ("late"). Two outputs take the per-output (multi) capture path, one the single one.
    // AUD-FIX10 R5: at 1920x1080 AMD's AV1 codes 1082 rows (HEVC 1088 with a conformance window);
    // 299cd25 never proved such a layout (0 frames on cray). 1280x720 aligns exactly and hid it.
    QTest::addColumn<bool>("virtualDesktop");
    QTest::addColumn<int>("outputs");
    QTest::addColumn<int>("codecId");
    QTest::addColumn<bool>("late");
    QTest::addColumn<QSize>("size");
    for (const QSize size : {QSize(1280, 720), QSize(1920, 1080), QSize(2560, 1440), QSize(3840, 2160)}) {
        for (const bool virtualDesktop : {true, false}) {
            for (const int outputs : {2, 1}) {
                for (const auto codec : {VideoCodec::Hevc, VideoCodec::Av1, VideoCodec::Avc420}) {
                    for (const bool late : {false, true}) {
                        if (late && outputs == 1) continue;
                        // AUD-FIX11: H.264 at attach (no switch), for the decode check, at 1080p.
                        if (codec == VideoCodec::Avc420 && (late || size != QSize(1920, 1080))) continue;
                        // The 720p rows keep AUD-FIX9's names.
                        const QByteArray prefix = size == QSize(1280, 720) ? QByteArray()
                                                                          : QStringLiteral("%1x%2 ").arg(size.width()).arg(size.height()).toLatin1();
                        QTest::addRow("%s%s %d-output %s %s", prefix.constData(), virtualDesktop ? "virtual" : "console", outputs,
                                      codec == VideoCodec::Hevc ? "hevc" : codec == VideoCodec::Av1 ? "av1" : "h264", late ? "late" : "immediate")
                            << virtualDesktop << outputs << int(codec) << late << size;
                    }
                }
            }
        }
    }
}

void WorkerEndToEndTest::codecSwitchAtAttach()
{
    QFETCH(bool, virtualDesktop);
    QFETCH(int, outputs);
    QFETCH(int, codecId);
    QFETCH(bool, late);
    QFETCH(QSize, size);
    const auto codec = VideoCodec(codecId);
    if (!m_skip.isEmpty()) {
        QSKIP(qPrintable(m_skip));
    }
    auto *s = session(outputs, size, true);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));

    ConsoleWorkerEndpoint endpoint;
    WorkerRun run;
    const auto dumpLogs = qScopeGuard([&] {
        if (!QTest::currentTestFailed()) return;
        qWarning().noquote() << "worker.log:\n" << s->log(QStringLiteral("worker.log"), 6000) << "\nkwin.log:\n" << s->log(QStringLiteral("kwin.log"), 1500);
    });
    QVERIFY(startWorker(*s, virtualDesktop, endpoint, run));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    // Whatever failed, the session's loop must be free for the next worker.
    const auto reap = qScopeGuard([&] {
        if (QFileInfo::exists(exitFile)) return;
        endpoint.stopWorker();
        if (!QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000)) qWarning("the worker did not stop");
    });
    const auto alive = [&] {
        return run.errors.isEmpty() && !QFileInfo::exists(exitFile);
    };

    // Ready with no client: the retained desktop idles until someone attaches.
    QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready() || !alive(), 45000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QVERIFY2(endpoint.ready(), "the real worker never confirmed capture");
    QVERIFY(run.caps);
    const auto family = codec == VideoCodec::Hevc ? CodecPolicy::Family::Hevc : codec == VideoCodec::Av1 ? CodecPolicy::Family::Av1 : CodecPolicy::Family::Avc;
    const auto &backends = run.caps->encoders.of(family);
    const auto host = EncoderSupport::probeUncached().encoders.of(family);
    if (!backends.hardware) {
        QVERIFY2(!host.hardware, "the host has this hardware encoder but the sandboxed worker does not see it");
        stopWorker(*s, endpoint);
        QSKIP("no hardware encoder for this codec on this host");
    }
    QTest::qWait(500); // let the pre-attach capture go idle

    // The attach, in the broker's order: the grant, a keyframe request and the connection's first
    // (AVC) codec config (WorkerCodecBridge::bind), then the private codec it negotiated.
    const qsizetype outputsBefore = run.outputs;
    const qsizetype grantMark = run.frames.size();
    const qint64 grantLog = s->workerLogSize();
    endpoint.setControlState({1, true});
    endpoint.requestKeyFrame();
    ConsoleWorkerWire::EncoderConfig avc{.generation = 1, .codec = VideoCodec::Avc420, .settings = CodecPolicy::EncoderSettings{.hardware = true}};
    QVERIFY(endpoint.setEncoderConfig(avc));
    if (late) {
        // The capture refresh for the new client has published (multi) or AVC flows (single).
        QTRY_VERIFY_WITH_TIMEOUT((outputs < 2 || run.outputs > outputsBefore) || !alive(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(run.frames.cbegin() + grantMark, run.frames.cend(), [](const auto &f) { return f.isKeyFrame; }) || !alive(), 15000);
        QVERIFY2(alive(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    }
    ConsoleWorkerWire::EncoderConfig target{.generation = 1, .codec = codec, .settings = CodecPolicy::EncoderSettings{.hardware = true}};
    const qsizetype mark = run.frames.size();
    const qsizetype outputsAtSwitch = run.outputs;
    QElapsedTimer clock;
    clock.start();
    QVERIFY(endpoint.setEncoderConfig(target));

    // Within 2 s: a decodable keyframe of the new codec that shows the output size, for every output.
    QSet<int> decoded;
    QSize coded;
    qsizetype checked = mark;
    qint64 elapsed = -1;
    while (clock.elapsed() < 2000 && alive()) {
        for (; checked < run.frames.size(); ++checked) {
            const auto &frame = run.frames[checked];
            if (frame.isKeyFrame && frame.codec == codec && frame.size == size && encodedKeyframeShows(codec, frame.data, size)) {
                decoded.insert(outputs > 1 ? frame.monitorIndex : 0);
                if (const auto keyframe = encodedKeyframe(codec, frame.data)) coded = keyframe->coded;
            }
        }
        if (decoded.size() == outputs) {
            elapsed = clock.elapsed();
            break;
        }
        QTest::qWait(10);
    }
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QVERIFY2(!QFileInfo::exists(exitFile), "the worker exited");
    int newCodec = 0;
    int oldCodec = 0;
    for (qsizetype i = mark; i < run.frames.size(); ++i) (run.frames[i].codec == codec ? newCodec : oldCodec)++;
    qInfo().noquote() << VideoCodecSupport::codecName(codec) << "at attach:" << decoded.size() << "of" << outputs
                      << "outputs decoded in" << elapsed << "ms (coded" << coded << "for" << size << ");" << newCodec << "new-codec and" << oldCodec << "older frames after the switch;"
                      << (run.outputs - outputsBefore) << "layouts published since the grant; order" << run.order.join(QStringLiteral(", "));
    QVERIFY2(decoded.size() == outputs, "no decodable keyframe of the new codec from every output within 2 s");

    // AUD-FIX11 R6: the client decodes what the broker delivers, from the first packet on. On
    // cray, libdav1d rejected the first AV1 delta after an output's proof keyframe: the packets
    // that output encoded while the other proved itself had been dropped.
    QTest::qWait(1000); // the new codec's delta frames (motion: ffplay in the session)
    // KRDP_E2E_DUMP=DIR: every frame since the grant, one file each, and an index (OBU/NAL types).
    if (const QString dump = qEnvironmentVariable("KRDP_E2E_DUMP"); !dump.isEmpty()) {
        QDir().mkpath(dump);
        QFile index(dump + QStringLiteral("/index.txt"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        for (qsizetype i = grantMark; i < run.frames.size(); ++i) {
            const auto &f = run.frames[i];
            const auto c = f.codec.value_or(VideoCodec::Avc420);
            const QString name = QStringLiteral("%1-m%2-%3%4.bin").arg(i - grantMark, 4, 10, QLatin1Char('0')).arg(f.monitorIndex)
                                     .arg(QLatin1String(VideoCodecSupport::codecName(c)), f.isKeyFrame ? QStringLiteral("-key") : QString());
            QFile out(dump + QLatin1Char('/') + name);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(f.data);
            index.write(QStringLiteral("%1 %2 %3x%4 monitors=%5 %6 %7\n").arg(name, i < mark ? QStringLiteral("pre") : QStringLiteral("post"))
                            .arg(f.size.width()).arg(f.size.height()).arg(f.monitors.size()).arg(f.data.size())
                            .arg(ClientStyle::unitTypes(c, f.data)).toUtf8());
        }
    }
    const auto delivery = deliverAndDecode(run.frames, grantMark, [&](qsizetype i) {
        // Immediate: the client's codec was chosen at connect, as on cray. Late: AVC first.
        return late && i < mark ? VideoCodec::Avc420 : codec;
    });
    qInfo().noquote() << "Delivered from the grant on:" << delivery.delivered << "packets," << delivery.held << "held back by a surface's chain; deltas per output"
                      << delivery.deltas << "; pictures" << delivery.pictures << "\n  " << delivery.firstPackets.join(QStringLiteral("\n   "));
    QVERIFY2(delivery.error.isEmpty(), qPrintable(QStringLiteral("the client would reject %1\nfirst packets:\n%2").arg(delivery.error, delivery.firstPackets.join(QLatin1Char('\n')))));
    for (int monitor = 0; monitor < outputs; ++monitor) {
        QVERIFY2(delivery.pictures.value(monitor) >= 1, qPrintable(QStringLiteral("output %1 showed no picture").arg(monitor)));
    }
    if (outputs > 1) QVERIFY2(run.outputs > outputsBefore, "the new client's capture never published its outputs (KScreen readback)");
    const QString since = s->workerLogSince(grantLog);
    const QString restart = QStringLiteral("Codec changed to %1 on a running stream").arg(QLatin1String(VideoCodecSupport::codecName(codec)));
    if (outputs > 1 && !late && codec != VideoCodec::Avc420) {
        // The codec arrived with the grant: the refreshed captures open in it; the streams they
        // replace never restart their encoders first (the ace/cray sequence).
        QVERIFY2(since.contains(QStringLiteral("Refreshing per-output captures for a new client")), qPrintable(since));
        QVERIFY2(!since.contains(restart), qPrintable(since));
    }
    if (outputs > 1 && late) {
        // A codec change on a live, published layout swaps the encoders on the same capture
        // streams: no capture refresh, no new layout.
        QCOMPARE(since.count(restart), outputs);
        QCOMPARE(run.outputs, outputsAtSwitch);
    }
    // Once an output has sent the new codec, it never goes back to the old one.
    QHash<int, bool> switched;
    for (qsizetype i = mark; i < run.frames.size(); ++i) {
        const auto &frame = run.frames[i];
        const int index = outputs > 1 ? frame.monitorIndex : 0;
        if (frame.codec == codec) switched[index] = true;
        else QVERIFY2(!switched.value(index), "an older codec's frame followed the new codec's");
    }
    stopWorker(*s, endpoint);
}

void WorkerEndToEndTest::coalesceKeepsEveryOutputAlive_data()
{
    // AUD-FIX12 (ace, c27909d): a client falls behind a full window; the broker throttles the
    // frame rate to what it takes (60 -> 6 fps), coalesces the moving output's queued frames and
    // asks for a keyframe, then raises the rate step by step (9, 14, 21, 32, 48, 60 fps) as the
    // window drains. On ace (HEVC hardware, two 1920x1080 outputs) monitor 0 sent its keyframe and
    // one delta, then nothing for 61 s. Replays the broker's sequence (timings from ace's journal)
    // against the real worker and asserts that the moving output is never silent for more than 2 s.
    QTest::addColumn<int>("codecId");
    QTest::newRow("hevc") << int(VideoCodec::Hevc);
    QTest::newRow("h264") << int(VideoCodec::Avc420);
    QTest::newRow("av1") << int(VideoCodec::Av1);
}

void WorkerEndToEndTest::coalesceKeepsEveryOutputAlive()
{
    QFETCH(int, codecId);
    const auto codec = VideoCodec(codecId);
    const QSize size(1920, 1080);
    if (!m_skip.isEmpty()) {
        QSKIP(qPrintable(m_skip));
    }
    auto *s = session(2, size, true);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    if (qEnvironmentVariableIsEmpty("KRDP_E2E_MOTION") && QStandardPaths::findExecutable(QStringLiteral("ffplay")).isEmpty()) QSKIP("no ffplay for motion");

    ConsoleWorkerEndpoint endpoint;
    WorkerRun run;
    QVector<qint64> arrivals; // ms on \a clock, parallel to run.frames
    QElapsedTimer clock;
    clock.start();
    connect(&endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [&arrivals, &clock](const VideoFrame &) {
        arrivals.append(clock.elapsed());
    });
    const qint64 logStart = s->workerLogSize();
    const auto dumpLogs = qScopeGuard([&] {
        if (!QTest::currentTestFailed()) return;
        qWarning().noquote() << "worker.log:\n" << s->workerLogSince(logStart).right(8000);
    });
    QVERIFY(startWorker(*s, true, endpoint, run));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    const auto reap = qScopeGuard([&] {
        if (QFileInfo::exists(exitFile)) return;
        endpoint.stopWorker();
        if (!QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000)) qWarning("the worker did not stop");
    });
    const auto alive = [&] {
        return run.errors.isEmpty() && !QFileInfo::exists(exitFile);
    };
    QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready() || !alive(), 45000);
    QVERIFY2(endpoint.ready(), "the real worker never confirmed capture");
    QVERIFY(run.caps);
    const auto family = codec == VideoCodec::Hevc ? CodecPolicy::Family::Hevc : codec == VideoCodec::Av1 ? CodecPolicy::Family::Av1 : CodecPolicy::Family::Avc;
    if (!run.caps->encoders.of(family).hardware) {
        stopWorker(*s, endpoint);
        QSKIP("no hardware encoder for this codec on this host");
    }

    // Attach as the broker does, straight into the codec.
    endpoint.setControlState({1, true});
    endpoint.requestKeyFrame();
    ConsoleWorkerWire::EncoderConfig config{.generation = 1, .codec = codec, .settings = CodecPolicy::EncoderSettings{.hardware = true}};
    QVERIFY(endpoint.setEncoderConfig(config));
    // Both outputs published in the codec, and motion on one of them.
    const qsizetype attachMark = run.frames.size();
    QHash<int, int> deltas;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            deltas.clear();
            for (qsizetype i = attachMark; i < run.frames.size(); ++i) {
                const auto &f = run.frames[i];
                if (f.codec == codec && !f.isKeyFrame && f.monitors.size() == 2) ++deltas[f.monitorIndex];
            }
            return std::any_of(deltas.cbegin(), deltas.cend(), [](int n) { return n >= 30; }) || !alive();
        }(),
        20000);
    QVERIFY2(alive(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    int moving = 0;
    for (auto it = deltas.cbegin(); it != deltas.cend(); ++it) {
        if (it.value() > deltas.value(moving)) moving = it.key();
    }

    // Adaptive quality moved before the coalesce (each change reopens a VA-API encoder).
    const int qualitySteps = qEnvironmentVariableIsSet("KRDP_E2E_QUALITY_STEPS") ? qEnvironmentVariableIntValue("KRDP_E2E_QUALITY_STEPS") : 16;
    for (int i = 0; i < qualitySteps && alive(); ++i) {
        QVERIFY(endpoint.setVideoQuality({1, quint8(i % 2 ? 80 : 70)}));
        QTest::qWait(250);
    }
    // KRDP_E2E_RATE_FLAPS=N: N extra frame-rate changes 150 ms apart first (each renegotiates the
    // PipeWire stream), to make KPipeWire's failed VA-API import more likely.
    for (int i = 0; i < qEnvironmentVariableIntValue("KRDP_E2E_RATE_FLAPS") && alive(); ++i) {
        config.frameRate = i % 2 ? 60 : 6;
        QVERIFY(endpoint.setEncoderConfig(config));
        QTest::qWait(150);
    }
    config.frameRate = 60;
    QVERIFY(endpoint.setEncoderConfig(config));
    // The broker's sequence on ace (14:29:34.555 throttle to 6 fps, .566 coalesce + keyframe,
    // then raises at +6.0, +10.5, +15.0, +18.0, +21.0 and +25.5 s).
    const qsizetype mark = run.frames.size();
    const qint64 start = clock.elapsed();
    const auto setRate = [&](quint32 fps) {
        config.frameRate = fps;
        QVERIFY(endpoint.setEncoderConfig(config));
    };
    // First, short cycles of the same pair (throttle to 6 fps, keyframe request d ms later, back
    // to 60 fps): the renegotiation races the keyframe request's re-feed of the last captured
    // buffer, and on a lost race KPipeWire's import of that buffer fails for good (see
    // EncoderWatchdog.h). KRDP_E2E_CYCLES sets how many (default 8).
    const int cycles = qEnvironmentVariableIsSet("KRDP_E2E_CYCLES") ? qEnvironmentVariableIntValue("KRDP_E2E_CYCLES") : 8;
    for (int i = 0; i < cycles && alive(); ++i) {
        setRate(6);
        QTest::qWait((i * 17) % 60);
        endpoint.requestKeyFrame();
        QTest::qWait(1500);
        setRate(60);
        QTest::qWait(1000);
    }
    const qint64 sequence = clock.elapsed();
    setRate(6);
    endpoint.requestKeyFrame();
    const QVector<std::pair<qint64, quint32>> raises{{6000, 9}, {10500, 14}, {15000, 21}, {18000, 32}, {21000, 48}, {25500, 60}};
    const qint64 runFor = qEnvironmentVariableIntValue("KRDP_E2E_COALESCE_SECONDS") > 0 ? qEnvironmentVariableIntValue("KRDP_E2E_COALESCE_SECONDS") * 1000LL : 30000;
    qsizetype next = 0;
    while (clock.elapsed() - sequence < runFor && alive()) {
        if (next < raises.size() && clock.elapsed() - sequence >= raises[next].first) setRate(raises[next++].second);
        QTest::qWait(20);
    }
    QVERIFY2(alive(), qPrintable(run.errors.join(QLatin1Char('\n'))));

    // Every output's frames since the coalesce, and the moving one's longest silence.
    qint64 last = start;
    qint64 longest = 0;
    qint64 longestAt = 0;
    QHash<int, int> sent;
    QHash<int, int> keyframes;
    for (qsizetype i = mark; i < run.frames.size(); ++i) {
        const auto &f = run.frames[i];
        ++sent[f.monitorIndex];
        if (f.isKeyFrame) ++keyframes[f.monitorIndex];
        if (f.monitorIndex != moving) continue;
        if (arrivals.value(i) - last > longest) {
            longest = arrivals.value(i) - last;
            longestAt = last - start;
        }
        last = arrivals.value(i);
    }
    const qint64 end = clock.elapsed();
    if (end - last > longest) {
        longest = end - last;
        longestAt = last - start;
    }
    const QString since = s->workerLogSince(logStart);
    qInfo().noquote() << VideoCodecSupport::codecName(codec) << "coalesce on output" << moving << ": frames per output" << sent << ", keyframes" << keyframes
                      << "; the moving output's longest silence" << longest << "ms from +" << longestAt << "ms; watchdog restarts:"
                      << since.count(QStringLiteral("restarting its encoder"));
    // KRDP_E2E_WORKER_LOG=FILE: the worker's log of this run.
    if (const QString path = qEnvironmentVariable("KRDP_E2E_WORKER_LOG"); !path.isEmpty()) {
        QFile out(path);
        if (out.open(QIODevice::WriteOnly)) out.write(since.toUtf8());
    }
    QVERIFY2(longest <= 2000, qPrintable(QStringLiteral("output %1 was silent for %2 ms (from +%3 ms)").arg(moving).arg(longest).arg(longestAt)));
    stopWorker(*s, endpoint);
}

void WorkerEndToEndTest::cursorShapeReachesTheBroker_data()
{
    QTest::addColumn<bool>("virtualDesktop");
    QTest::newRow("virtual desktop (:3395)") << true;
    QTest::newRow("console session (:3391)") << false;
}

void WorkerEndToEndTest::cursorShapeReachesTheBroker()
{
    // FIX-CURSOR: KWin changes the cursor over a window (arrow -> I-beam -> hidden -> resize ->
    // arrow); the real worker must report each shape to the broker as a Cursor record, with the
    // bitmap, and never for a mere move.
    using Shape = ConsoleWorkerWire::CursorShape;
    QFETCH(bool, virtualDesktop);
    if (!m_skip.isEmpty()) {
        QSKIP(qPrintable(m_skip));
    }
    auto *s = session(1, QSize(1280, 720), false, true);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));

    ConsoleWorkerEndpoint endpoint;
    WorkerRun run;
    QVector<Shape> cursors;
    connect(&endpoint, &ConsoleWorkerEndpoint::cursorShapeReceived, this, [&cursors](const Shape &shape) {
        cursors.append(shape);
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, this, [&endpoint](const auto &) {
        endpoint.setControlState({1, true});
        endpoint.requestKeyFrame();
    });
    const auto dumpLogs = qScopeGuard([&] {
        if (!QTest::currentTestFailed()) return;
        QStringList seen;
        for (const auto &shape : cursors)
            seen << QStringLiteral("%1 %2x%3").arg(int(shape.type)).arg(shape.size.width()).arg(shape.size.height());
        qWarning().noquote() << "cursor records:" << seen.join(QStringLiteral(", ")) << "\nworker.log:\n" << s->log(QStringLiteral("worker.log"), 4000)
                             << "\nkwin.log:\n" << s->log(QStringLiteral("kwin.log"), 2000) << "\ncursor-client.log:\n"
                             << s->log(QStringLiteral("cursor-client.log"), 1000);
    });
    QVERIFY(startWorker(*s, virtualDesktop, endpoint, run));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    const auto reap = qScopeGuard([&] {
        if (QFileInfo::exists(exitFile)) return;
        endpoint.stopWorker();
        if (!QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000)) qWarning("the worker did not stop");
    });
    QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready() || !run.errors.isEmpty() || QFileInfo::exists(exitFile), 45000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QVERIFY(endpoint.ready());

    const auto move = [&endpoint](QPointF to) {
        ConsoleWorkerWire::Input input;
        input.type = ConsoleWorkerWire::Input::Type::Mouse;
        input.eventType = QEvent::MouseMove;
        input.position = to;
        endpoint.sendInput(input);
    };
    const auto setShape = [&](const char *word) {
        QFile control(s->runtime->path() + QStringLiteral("/cursor-shape"));
        QVERIFY(control.open(QIODevice::WriteOnly | QIODevice::Truncate));
        control.write(word);
    };
    const auto lastImage = [&cursors]() -> std::optional<Shape> {
        for (auto it = cursors.crbegin(); it != cursors.crend(); ++it) {
            if (it->type == Shape::Type::Image) return *it;
        }
        return std::nullopt;
    };
    // The pointer onto the window (it is full screen): KWin shows the client's arrow.
    move({600, 300});
    move({640, 360});
    QTRY_VERIFY_WITH_TIMEOUT(lastImage(), 15000);
    const Shape arrow = *lastImage();
    QVERIFY(arrow.size.width() >= 8 && arrow.size.width() <= Shape::MaxDimension);
    QCOMPARE(arrow.pixels.size(), qsizetype(arrow.size.width()) * arrow.size.height() * 4);

    // Moves alone send nothing.
    const qsizetype beforeMoves = cursors.size();
    for (int i = 0; i < 30; ++i) {
        move({400.0 + 10 * i, 300.0 + 5 * i});
        QTest::qWait(20);
    }
    QTest::qWait(300);
    QCOMPARE(cursors.size(), beforeMoves);

    // KWin sends a changed bitmap with its next record of the output (a move or a repaint): the
    // pointer keeps moving by a pixel, as a hand on a mouse does, until the shape arrives.
    int nudge = 0;
    const auto whileMoving = [&](const std::function<bool()> &arrived) {
        QElapsedTimer timer;
        timer.start();
        while (!arrived() && timer.elapsed() < 10000) {
            move({640.0 + (++nudge % 2), 360.0});
            QTest::qWait(50);
        }
        return arrived();
    };
    // The I-beam (a text field): a new bitmap.
    setShape("ibeam");
    QVERIFY(whileMoving([&] {
        return cursors.constLast().type == Shape::Type::Image && cursors.constLast().pixels != arrow.pixels;
    }));
    const Shape ibeam = cursors.constLast();
    // Hidden (a full-screen video): Hidden, not an image.
    setShape("blank");
    QVERIFY(whileMoving([&] {
        return cursors.constLast().type == Shape::Type::Hidden;
    }));
    // A window edge's resize cursor: shown again, a third bitmap.
    setShape("sizehor");
    QVERIFY(whileMoving([&] {
        return cursors.constLast().type == Shape::Type::Image && cursors.constLast().pixels != ibeam.pixels && cursors.constLast().pixels != arrow.pixels;
    }));
    // And back to the arrow.
    setShape("arrow");
    QVERIFY(whileMoving([&] {
        return cursors.constLast() == arrow;
    }));
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    qInfo().noquote() << "cursor records:" << cursors.size() << "arrow" << arrow.size << "hot" << arrow.hotspot << "ibeam" << ibeam.size << "hot" << ibeam.hotspot;
    stopWorker(*s, endpoint);
}

void WorkerEndToEndTest::av1TilesAndBitrate_data()
{
    // AV1-Q on this host's real AV1 encoder (Hal's 780M): the tile count the broker resolves from
    // the host setting and the client's decode path (CodecPolicy::resolveAv1Tiles()) reaches the
    // worker's encoder, the keyframe header shows those tile rows, the display-size proof (R5)
    // and the client-style decode of every packet (R6) still pass, and full-screen motion at
    // 30 fps and the default quality (80) stays in a sane bitrate range (it was near-lossless).
    QTest::addColumn<QSize>("size");
    QTest::addColumn<int>("setting"); // Av1Tiles: 0 = auto
    QTest::addColumn<int>("decode"); // the client's decode.av1 (CodecPolicy::DecodePath)
    QTest::addColumn<QSize>("tiles"); // columns x rows in the keyframe header
    QTest::addColumn<double>("maxMbps");
    const int sw = int(CodecPolicy::DecodePath::Software), hw = int(CodecPolicy::DecodePath::Hardware), unknown = int(CodecPolicy::DecodePath::Unknown);
    QTest::newRow("1080p auto, software decoder") << QSize(1920, 1080) << 0 << sw << QSize(1, 4) << 30.0;
    QTest::newRow("1080p auto, decoder unknown") << QSize(1920, 1080) << 0 << unknown << QSize(1, 4) << 30.0;
    QTest::newRow("1080p auto, hardware decoder") << QSize(1920, 1080) << 0 << hw << QSize(1, 1) << 30.0;
    QTest::newRow("1080p 8 tiles") << QSize(1920, 1080) << 8 << hw << QSize(1, 8) << 30.0;
    QTest::newRow("1440p auto, software decoder") << QSize(2560, 1440) << 0 << sw << QSize(1, 8) << 50.0;
}

void WorkerEndToEndTest::av1TilesAndBitrate()
{
    QFETCH(QSize, size);
    QFETCH(int, setting);
    QFETCH(int, decode);
    QFETCH(QSize, tiles);
    QFETCH(double, maxMbps);
    if (!m_skip.isEmpty()) {
        QSKIP(qPrintable(m_skip));
    }
    auto *s = session(1, size, true);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    if (QStandardPaths::findExecutable(QStringLiteral("ffplay")).isEmpty()) QSKIP("ffplay is not installed: no motion to measure");

    ConsoleWorkerEndpoint endpoint;
    WorkerRun run;
    const auto dumpLogs = qScopeGuard([&] {
        if (!QTest::currentTestFailed()) return;
        qWarning().noquote() << "worker.log:\n" << s->log(QStringLiteral("worker.log"), 6000);
    });
    QVERIFY(startWorker(*s, true, endpoint, run));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    const auto reap = qScopeGuard([&] {
        if (QFileInfo::exists(exitFile)) return;
        endpoint.stopWorker();
        if (!QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000)) qWarning("the worker did not stop");
    });
    const auto alive = [&] {
        return run.errors.isEmpty() && !QFileInfo::exists(exitFile);
    };
    QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready() || !alive(), 45000);
    QVERIFY2(endpoint.ready(), "the real worker never confirmed capture");
    QVERIFY(run.caps);
    if (!run.caps->encoders.av1.hardware) {
        QVERIFY2(!EncoderSupport::probeUncached().encoders.av1.hardware, "the host has a hardware AV1 encoder but the sandboxed worker does not see it");
        stopWorker(*s, endpoint);
        QSKIP("no hardware AV1 encoder on this host");
    }

    // What the broker sends for this client: AV1 in hardware at 30 fps with the resolved tiles.
    const int av1Tiles = CodecPolicy::resolveAv1Tiles(setting, CodecPolicy::DecodePath(decode));
    endpoint.setControlState({1, true});
    endpoint.requestKeyFrame();
    ConsoleWorkerWire::EncoderConfig config{.generation = 1, .codec = VideoCodec::Av1,
                                            .settings = CodecPolicy::EncoderSettings{.hardware = true, .av1Tiles = av1Tiles}, .frameRate = 30};
    const qsizetype mark = run.frames.size();
    QVERIFY(endpoint.setEncoderConfig(config));

    // The first AV1 keyframe: the display-size proof (R5) and the tile rows.
    std::optional<QSize> headerTiles;
    qsizetype first = -1;
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(run.frames.cbegin() + mark, run.frames.cend(), [](const auto &f) {
                                 return f.isKeyFrame && f.codec == VideoCodec::Av1;
                             }) || !alive(),
                             15000);
    QVERIFY2(alive(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    for (qsizetype i = mark; i < run.frames.size(); ++i) {
        const auto &frame = run.frames[i];
        if (frame.isKeyFrame && frame.codec == VideoCodec::Av1) {
            QVERIFY2(encodedKeyframeShows(VideoCodec::Av1, frame.data, size), "the AV1 keyframe does not prove the output size (R5)");
            headerTiles = av1KeyframeTiles(frame.data);
            first = i;
            break;
        }
    }
    QCOMPARE(headerTiles, std::optional<QSize>(tiles));

    // Five seconds of motion at the default quality (80).
    QElapsedTimer clock;
    clock.start();
    QTest::qWait(5000);
    QVERIFY2(alive(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    const qint64 elapsedMs = clock.elapsed();
    qint64 bytes = 0;
    int frames = 0;
    for (qsizetype i = first + 1; i < run.frames.size(); ++i) {
        if (run.frames[i].codec != VideoCodec::Av1) continue;
        bytes += run.frames[i].data.size();
        ++frames;
    }
    const double mbps = double(bytes) * 8.0 / 1000.0 / double(elapsedMs);
    const double fps = frames * 1000.0 / double(elapsedMs);
    qInfo().noquote() << QStringLiteral("AV1 %1x%2, setting %3, decode %4 -> tiles %5x%6 (columns x rows); %7 frames in %8 ms (%9 fps), %10 Mbit/s")
                             .arg(size.width()).arg(size.height())
                             .arg(CodecPolicy::av1TilesName(setting), QLatin1String(CodecPolicy::decodePathName(CodecPolicy::DecodePath(decode))))
                             .arg(headerTiles->width()).arg(headerTiles->height())
                             .arg(frames).arg(elapsedMs).arg(fps, 0, 'f', 1).arg(mbps, 0, 'f', 1);
    QVERIFY2(fps >= 10.0, "the motion stalled");
    QVERIFY2(mbps > 0.2, "no motion reached the encoder");
    QVERIFY2(mbps <= maxMbps, qPrintable(QStringLiteral("%1 Mbit/s at quality 80 (limit %2)").arg(mbps).arg(maxMbps)));

    // Every later keyframe keeps the tiles; every packet decodes the way the client decodes (R6).
    for (qsizetype i = first; i < run.frames.size(); ++i) {
        const auto &frame = run.frames[i];
        if (frame.isKeyFrame && frame.codec == VideoCodec::Av1) QCOMPARE(av1KeyframeTiles(frame.data), std::optional<QSize>(tiles));
    }
    const auto delivery = deliverAndDecode(run.frames, first, [](qsizetype) { return VideoCodec::Av1; });
    QVERIFY2(delivery.error.isEmpty(), qPrintable(QStringLiteral("the client would reject %1").arg(delivery.error)));
    QVERIFY(delivery.pictures.value(0) >= 1);
    stopWorker(*s, endpoint);
}

void WorkerEndToEndTest::clientCursorReachesTheBrokerAtRest_data()
{
    QTest::addColumn<bool>("virtualDesktop");
    QTest::newRow("virtual desktop (:3395)") << true;
    QTest::newRow("console session (:3391)") << false;
}

void WorkerEndToEndTest::clientCursorReachesTheBrokerAtRest()
{
    // AUD-FIX14: a cursor the application under the pointer sets itself, as Konsole's I-beam over
    // its text: a cursor surface of its own (wl_pointer.set_cursor, an shm buffer) or a
    // cursor-shape-v1 shape. The application sets it only once it heard of the move, after KWin
    // recorded that move - and KWin 6.6 records a changed cursor only with its next move or
    // repaint (ScreenCastStream::invalidateCursor schedules nothing). Each step here is ONE move
    // and then a pointer at rest: the worker must still report the application's exact shape.
    using Shape = ConsoleWorkerWire::CursorShape;
    QFETCH(bool, virtualDesktop);
    if (!m_skip.isEmpty()) {
        QSKIP(qPrintable(m_skip));
    }
    auto *s = session(1, QSize(1280, 720), false, true);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));

    ConsoleWorkerEndpoint endpoint;
    WorkerRun run;
    QVector<Shape> cursors;
    connect(&endpoint, &ConsoleWorkerEndpoint::cursorShapeReceived, this, [&cursors](const Shape &shape) {
        cursors.append(shape);
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, this, [&endpoint](const auto &) {
        endpoint.setControlState({1, true});
        endpoint.requestKeyFrame();
    });
    const auto dumpLogs = qScopeGuard([&] {
        if (!QTest::currentTestFailed()) return;
        QStringList seen;
        for (const auto &shape : cursors)
            seen << QStringLiteral("%1 %2x%3 hot %4,%5")
                        .arg(int(shape.type))
                        .arg(shape.size.width())
                        .arg(shape.size.height())
                        .arg(shape.hotspot.x())
                        .arg(shape.hotspot.y());
        qWarning().noquote() << "cursor records:" << seen.join(QStringLiteral(", ")) << "\nworker.log:\n" << s->log(QStringLiteral("worker.log"), 4000)
                             << "\nkwin.log:\n" << s->log(QStringLiteral("kwin.log"), 2000) << "\ncursor-client.log:\n"
                             << s->log(QStringLiteral("cursor-client.log"), 2000);
    });
    // The window's cursor follows the pointer (CursorShapeClient `regions`).
    {
        QFile control(s->runtime->path() + QStringLiteral("/cursor-shape"));
        QVERIFY(control.open(QIODevice::WriteOnly | QIODevice::Truncate));
        control.write("regions");
    }
    QTest::qWait(300); // the client polls its control file every 50 ms
    QVERIFY(startWorker(*s, virtualDesktop, endpoint, run));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    const auto reap = qScopeGuard([&] {
        if (QFileInfo::exists(exitFile)) return;
        endpoint.stopWorker();
        if (!QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000)) qWarning("the worker did not stop");
    });
    QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready() || !run.errors.isEmpty() || QFileInfo::exists(exitFile), 45000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QVERIFY(endpoint.ready());

    const auto move = [&endpoint](QPointF to) {
        ConsoleWorkerWire::Input input;
        input.type = ConsoleWorkerWire::Input::Type::Mouse;
        input.eventType = QEvent::MouseMove;
        input.position = to;
        endpoint.sendInput(input);
    };
    const auto solid = [](QSize size, QPoint hotspot, QRgb argb) {
        Shape shape;
        shape.type = Shape::Type::Image;
        shape.size = size;
        shape.hotspot = hotspot;
        const QImage image = QImage(size, QImage::Format_ARGB32);
        QImage filled = image;
        filled.fill(argb);
        for (int y = 0; y < size.height(); ++y)
            shape.pixels.append(reinterpret_cast<const char *>(filled.constScanLine(y)), qsizetype(size.width()) * 4);
        return shape;
    };
    const Shape bitmapA = solid(QSize(20, 14), QPoint(5, 9), qRgba(255, 0, 255, 255));
    const Shape bitmapB = solid(QSize(32, 24), QPoint(30, 2), qRgba(0, 255, 255, 255));
    const auto describe = [](const std::optional<Shape> &shape) {
        if (!shape) return QStringLiteral("nothing");
        return QStringLiteral("type %1 %2x%3 hot %4,%5")
            .arg(int(shape->type))
            .arg(shape->size.width())
            .arg(shape->size.height())
            .arg(shape->hotspot.x())
            .arg(shape->hotspot.y());
    };
    const auto last = [&cursors]() -> std::optional<Shape> {
        if (cursors.isEmpty()) return std::nullopt;
        return cursors.constLast();
    };
    // One move, then rest: the shape must arrive while the pointer stays put.
    const auto moveOnceAndExpect = [&](QPointF to, const std::function<bool(const Shape &)> &expected, const char *what) {
        move(to);
        const bool arrived = QTest::qWaitFor(
            [&] {
                return last() && expected(*last());
            },
            5000);
        if (!arrived) qWarning().noquote() << what << "did not arrive; last record:" << describe(last());
        return arrived;
    };

    // Left third: the client's own cursor surface A.
    QVERIFY(moveOnceAndExpect({200, 300}, [&](const Shape &shape) { return shape == bitmapA; }, "cursor surface A (20x14 hot 5,9)"));
    // Middle third: the I-beam (cursor-shape-v1 `text`), a themed image unlike A and B.
    QVERIFY(moveOnceAndExpect(
        {640, 300},
        [&](const Shape &shape) {
            return shape.type == Shape::Type::Image && shape != bitmapA && shape != bitmapB && shape.size.width() >= 8;
        },
        "I-beam"));
    const Shape ibeam = *last();
    // Right third: cursor surface B.
    QVERIFY(moveOnceAndExpect({1100, 300}, [&](const Shape &shape) { return shape == bitmapB; }, "cursor surface B (32x24 hot 30,2)"));
    // Back to A (a surface replacing a surface), then the I-beam again.
    QVERIFY(moveOnceAndExpect({210, 310}, [&](const Shape &shape) { return shape == bitmapA; }, "cursor surface A again"));
    QVERIFY(moveOnceAndExpect({650, 310}, [&](const Shape &shape) { return shape == ibeam; }, "I-beam again"));
    // Moves within one region change nothing.
    const qsizetype before = cursors.size();
    for (int i = 0; i < 10; ++i) {
        move({600.0 + 5 * i, 320.0});
        QTest::qWait(30);
    }
    QTest::qWait(600);
    QCOMPARE(cursors.size(), before);

    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    qInfo().noquote() << "cursor records:" << cursors.size() << "ibeam" << ibeam.size << "hot" << ibeam.hotspot;
    {
        QFile control(s->runtime->path() + QStringLiteral("/cursor-shape"));
        QVERIFY(control.open(QIODevice::WriteOnly | QIODevice::Truncate));
        control.write("arrow");
    }
    stopWorker(*s, endpoint);
}

namespace
{
int countOf(const QString &text, const QString &needle)
{
    return int(text.count(needle));
}
}

// OPT-055 T-K4b (i): one encoder failure while streaming -> the ladder restarts the encoder, and a
// new keyframe that decodes (right size, SPS) reaches the broker.
void WorkerEndToEndTest::encoderFailureRestartsAndRecovers()
{
    if (!m_skip.isEmpty()) QSKIP(qPrintable(m_skip));
    const QString shim = qEnvironmentVariable("KRDP_E2E_FAILFILTER", QStringLiteral(KRDP_E2E_FAILFILTER));
    auto *s = session(1);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    const qint64 logFrom = s->workerLogSize();
    m_nextWorkerEnv = {QStringLiteral("LD_PRELOAD=%1").arg(shim), QStringLiteral("FARSIDE_TEST_FAIL_FILTER_FILE=%1/fail-filter").arg(s->runtime->path()),
                       QStringLiteral("FARSIDE_TEST_FAIL_FILTER_COUNT=1")};
    ConsoleWorkerEndpoint endpoint;
    WorkerRun run;
    connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, this, [&endpoint](const auto &) {
        endpoint.setControlState({1, true});
        endpoint.requestKeyFrame();
    });
    const auto dumpLogs = qScopeGuard([&] {
        if (QTest::currentTestFailed()) qWarning().noquote() << "worker.log:\n" << s->workerLogSince(logFrom).right(6000) << "\nmotion.log:\n" << s->log(QStringLiteral("motion.log"), 1500);
    });
    QVERIFY(startWorker(*s, false, endpoint, run));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    const auto reap = qScopeGuard([&] {
        if (QFileInfo::exists(exitFile)) return;
        endpoint.stopWorker();
        QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000);
    });
    QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready() || !run.errors.isEmpty(), 45000);
    QTRY_VERIFY_WITH_TIMEOUT(run.keyframes >= 1, 30000);
    // Fail exactly one filter call (COUNT=1) on the next encode; a keyframe request makes the idle desktop encode.
    const QString failFile = s->runtime->path() + QStringLiteral("/fail-filter");
    {
        QFile f(failFile);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    const auto removeFail = qScopeGuard([&] { QFile::remove(failFile); });
    QElapsedTimer poke;
    poke.start();
    while (!s->workerLogSince(logFrom).contains(QStringLiteral("the encoder failed (")) && run.errors.isEmpty() && poke.elapsed() < 30000) {
        endpoint.requestKeyFrame();
        QTest::qWait(250);
    }
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    const int keyframesAtFailure = run.keyframes;
    const qsizetype framesAtFailure = run.frames.size();
    // An idle desktop sends nothing on its own; the client's Refresh Rect (a keyframe request) gets the new picture.
    poke.restart();
    while (run.keyframes <= keyframesAtFailure && run.errors.isEmpty() && poke.elapsed() < 30000) {
        endpoint.requestKeyFrame();
        QTest::qWait(500);
    }
    QVERIFY2(run.keyframes > keyframesAtFailure, "no keyframe after the encoder restart");
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QVERIFY(run.payloadMatches);
    QCOMPARE(run.keyframeSize, QSize(1280, 720));
    QVERIFY(run.frames.size() > framesAtFailure);
    const QString log = s->workerLogSince(logFrom);
    QCOMPARE(countOf(log, QStringLiteral("the encoder failed (")), 1);
    QVERIFY2(!log.contains(QStringLiteral("closing the session")), "one failure must not close the session");
    QVERIFY2(countOf(log, QStringLiteral("Failed receiving filtered frame")) <= 2, qPrintable(log.right(3000)));
    qInfo().noquote() << "T-K4b(i): restarted once, keyframes" << keyframesAtFailure << "->" << run.keyframes << "; 'Failed receiving filtered frame' lines:"
                      << countOf(log, QStringLiteral("Failed receiving filtered frame"));
    stopWorker(*s, endpoint);
}

// T-K4b (ii): the encoder keeps failing -> the ladder gives up (two restarts), the worker reports a
// capture failure and goes away; the next worker (no shim) streams a decodable keyframe again.
void WorkerEndToEndTest::persistentEncoderFailureClosesAndNextWorkerStreams()
{
    if (!m_skip.isEmpty()) QSKIP(qPrintable(m_skip));
    const QString shim = qEnvironmentVariable("KRDP_E2E_FAILFILTER", QStringLiteral(KRDP_E2E_FAILFILTER));
    auto *s = session(1);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    const qint64 logFrom = s->workerLogSize();
    m_nextWorkerEnv = {QStringLiteral("LD_PRELOAD=%1").arg(shim), QStringLiteral("FARSIDE_TEST_FAIL_FILTER_FILE=%1/fail-filter").arg(s->runtime->path())};
    {
        ConsoleWorkerEndpoint endpoint;
        WorkerRun run;
        connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, this, [&endpoint](const auto &) {
            endpoint.setControlState({1, true});
            endpoint.requestKeyFrame();
        });
        const auto dumpLogs = qScopeGuard([&] {
            if (QTest::currentTestFailed()) qWarning().noquote() << "worker.log:\n" << s->workerLogSince(logFrom).right(6000) << "\nmotion.log:\n" << s->log(QStringLiteral("motion.log"), 1500);
        });
        QVERIFY(startWorker(*s, false, endpoint, run));
        const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
        const auto reap = qScopeGuard([&] {
            if (QFileInfo::exists(exitFile)) return;
            endpoint.stopWorker();
            QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000);
        });
        QTRY_VERIFY_WITH_TIMEOUT(run.keyframes >= 1, 45000);
        const QString failFile = s->runtime->path() + QStringLiteral("/fail-filter");
        {
            QFile f(failFile);
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        const auto removeFail = qScopeGuard([&] { QFile::remove(failFile); });
        QElapsedTimer poke;
        poke.start();
        while (run.errors.isEmpty() && poke.elapsed() < 90000) {
            endpoint.requestKeyFrame();
            QTest::qWait(250);
        }
        QVERIFY2(run.errors.join(QLatin1Char(' ')).contains(QStringLiteral("worker capture failed")), qPrintable(run.errors.join(QLatin1Char('\n'))));
        // The worker disconnected after reporting; its process ends on its own.
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(exitFile), 15000);
        const QString log = s->workerLogSince(logFrom);
        QCOMPARE(countOf(log, QStringLiteral("the encoder failed (")), 2);
        QVERIFY2(log.contains(QStringLiteral("closing the session")), qPrintable(log.right(3000)));
        // Three encoder instances (the first and two restarts), at most two lines each.
        QVERIFY2(countOf(log, QStringLiteral("Failed receiving filtered frame")) <= 6, qPrintable(log.right(3000)));
        qInfo().noquote() << "T-K4b(ii): worker closed after" << countOf(log, QStringLiteral("the encoder failed (")) << "restarts; 'Failed receiving filtered frame' lines:"
                          << countOf(log, QStringLiteral("Failed receiving filtered frame")) << "; broker saw:" << run.errors.join(QStringLiteral(" | "));
    }
    // The next connection: a fresh worker with no shim must accept and decode a keyframe.
    ConsoleWorkerEndpoint next;
    WorkerRun again;
    connect(&next, &ConsoleWorkerEndpoint::workerReady, this, [&next](const auto &) {
        next.setControlState({1, true});
        next.requestKeyFrame();
    });
    QElapsedTimer accepted;
    accepted.start();
    QVERIFY(startWorker(*s, false, next, again));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    const auto reap = qScopeGuard([&] {
        if (QFileInfo::exists(exitFile)) return;
        next.stopWorker();
        QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000);
    });
    QTRY_VERIFY_WITH_TIMEOUT(again.keyframes >= 1 || !again.errors.isEmpty(), 30000);
    const qint64 ms = accepted.elapsed();
    QVERIFY2(again.errors.isEmpty(), qPrintable(again.errors.join(QLatin1Char('\n'))));
    QVERIFY(again.payloadMatches);
    QCOMPARE(again.keyframeSize, QSize(1280, 720));
    qInfo().noquote() << "T-K4b(ii): the next worker decoded a keyframe" << ms << "ms after its endpoint listened";
    QVERIFY2(ms <= 15000, qPrintable(QString::number(ms)));
    stopWorker(*s, next);
}

// T-K4b (iii): a failure that is already in place while the broker disconnects: the worker still exits promptly.
void WorkerEndToEndTest::encoderFailureDuringDisconnectStillExits()
{
    if (!m_skip.isEmpty()) QSKIP(qPrintable(m_skip));
    const QString shim = qEnvironmentVariable("KRDP_E2E_FAILFILTER", QStringLiteral(KRDP_E2E_FAILFILTER));
    auto *s = session(1);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    const qint64 logFrom = s->workerLogSize();
    const QString failFile = s->runtime->path() + QStringLiteral("/fail-filter");
    QFile::remove(failFile);
    m_nextWorkerEnv = {QStringLiteral("LD_PRELOAD=%1").arg(shim), QStringLiteral("FARSIDE_TEST_FAIL_FILTER_FILE=%1").arg(failFile)};
    ConsoleWorkerEndpoint endpoint;
    WorkerRun run;
    connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, this, [&endpoint](const auto &) {
        endpoint.setControlState({1, true});
        endpoint.requestKeyFrame();
    });
    const auto dumpLogs = qScopeGuard([&] {
        if (QTest::currentTestFailed()) qWarning().noquote() << "worker.log:\n" << s->workerLogSince(logFrom).right(6000) << "\nmotion.log:\n" << s->log(QStringLiteral("motion.log"), 1500);
    });
    QVERIFY(startWorker(*s, false, endpoint, run));
    const QString exitFile = s->runtime->path() + QStringLiteral("/worker-exit");
    const auto reap = qScopeGuard([&] {
        QFile::remove(failFile);
        if (QFileInfo::exists(exitFile)) return;
        endpoint.stopWorker();
        QTest::qWaitFor([&] { return QFileInfo::exists(exitFile); }, 15000);
    });
    QTRY_VERIFY_WITH_TIMEOUT(run.keyframes >= 1 || !run.errors.isEmpty(), 45000);
    QVERIFY2(run.errors.isEmpty(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    QFile fail(failFile);
    QVERIFY(fail.open(QIODevice::WriteOnly));
    fail.close();
    QElapsedTimer stopping;
    stopping.start();
    endpoint.stopWorker();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(exitFile), 15000);
    const qint64 ms = stopping.elapsed();
    qInfo().noquote() << "T-K4b(iii): the worker exited" << ms << "ms after stop with the encoder failing";
    QVERIFY2(ms <= 3000, qPrintable(QString::number(ms)));
}

bool WorkerEndToEndTest::startUserServer(PrivateSession &s, const QStringList &environment, int port)
{
    const QString runtime = s.runtime->path();
    for (const auto *name : {"server-exit", "server-stop"}) QFile::remove(runtime + QLatin1Char('/') + QLatin1String(name));
    QFile env(runtime + QStringLiteral("/server-env"));
    if (!env.open(QIODevice::WriteOnly)) return false;
    env.write(environment.join(QLatin1Char('\n')).toUtf8() + '\n');
    env.close();
    QFile args(runtime + QStringLiteral("/server-args"));
    if (!args.open(QIODevice::WriteOnly)) return false;
    // A disposable instance on loopback with a throwaway login (nothing here is a real credential).
    args.write(QStringLiteral("--plasma\n--address\n127.0.0.1\n--port\n%1\n--username\nk4test\n--password\nk4test-not-a-secret\n").arg(port).toUtf8());
    args.close();
    return true;
}

namespace
{
bool portAccepts(int port)
{
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), quint16(port));
    return socket.waitForConnected(300);
}
void stopUserServer(PrivateSession &s)
{
    QFile stop(s.runtime->path() + QStringLiteral("/server-stop"));
    if (stop.open(QIODevice::WriteOnly)) stop.close();
    QTest::qWaitFor([&] { return !QFileInfo::exists(s.runtime->path() + QStringLiteral("/server-args")); }, 15000);
}
/// The probe client; \a log collects stdout+stderr.
struct Probe {
    QProcess process;
    QByteArray log;
    explicit Probe(const QStringList &arguments)
    {
        process.setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(&process, &QProcess::readyRead, &process, [this] { log += process.readAll(); });
        process.start(QStringLiteral(KRDP_E2E_PROBE), QStringList{QStringLiteral("127.0.0.1"), QStringLiteral("3399"), QStringLiteral("k4test"),
                                                                      QStringLiteral("k4test-not-a-secret")} + arguments);
    }
    ~Probe()
    {
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(3000)) {
                process.kill();
                process.waitForFinished(3000);
            }
        }
    }
};
}

// OPT-055 T-K4b (ii), user server: a persistently failing encoder ends the connection with a KRDPCTL
// `session-end` (reason encoder-failed) and ERRINFO_GRAPHICS_SUBSYSTEM_FAILED, the listener keeps
// accepting, and the next client decodes a frame.
void WorkerEndToEndTest::userServerEndsWithSessionEndAndKeepsAccepting()
{
#ifndef KRDP_E2E_PROBE
    QSKIP("krdpctl-probe (FreeRDP client) is not built");
#else
    if (!m_skip.isEmpty()) QSKIP(qPrintable(m_skip));
    auto *s = session(1);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    const QString failFile = s->runtime->path() + QStringLiteral("/fail-filter");
    QFile::remove(failFile);
    const qint64 serverLogFrom = QFileInfo(s->home->path() + QStringLiteral("/server.log")).size();
    const auto serverLog = [&] {
        QFile f(s->home->path() + QStringLiteral("/server.log"));
        if (!f.open(QIODevice::ReadOnly) || !f.seek(serverLogFrom)) return QString();
        return QString::fromUtf8(f.readAll());
    };
    const auto cleanup = qScopeGuard([&] {
        QFile::remove(failFile);
        stopUserServer(*s);
        if (QTest::currentTestFailed()) qWarning().noquote() << "server.log:\n" << serverLog().right(6000);
    });
    QVERIFY(startUserServer(*s, {QStringLiteral("LD_PRELOAD=%1").arg(QStringLiteral(KRDP_E2E_FAILFILTER)),
                                 QStringLiteral("FARSIDE_TEST_FAIL_FILTER_FILE=%1").arg(failFile)}, 3399));
    QTRY_VERIFY_WITH_TIMEOUT(portAccepts(3399), 60000);

    {
        QFile f(failFile);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    {
        Probe probe({QStringLiteral("--silent"), QStringLiteral("--gfx"), QStringLiteral("--timeout"), QStringLiteral("60")});
        QTRY_VERIFY_WITH_TIMEOUT(probe.log.contains("session-end") || probe.process.state() == QProcess::NotRunning, 60000);
        QVERIFY2(probe.log.contains("session-end"), probe.log.right(3000).constData());
        QVERIFY2(probe.log.contains("encoder-failed"), probe.log.right(3000).constData());
        qInfo().noquote() << "T-K4b(ii) user server: the client saw:" << QString::fromUtf8(probe.log.mid(probe.log.indexOf("{\"errorInfo")).left(400));
    }
    const QString log = serverLog();
    QCOMPARE(countOf(log, QStringLiteral("the encoder failed (")), 2);
    QVERIFY2(log.contains(QStringLiteral("closing the session")), qPrintable(log.right(3000)));
    QVERIFY2(countOf(log, QStringLiteral("Failed receiving filtered frame")) <= 9, qPrintable(log.right(3000))); // 3 instances x (KPipeWire + krdp echo) + slack
    QFile::remove(failFile);
    QVERIFY2(portAccepts(3399), "the listener stopped accepting after the failed session");
    // The next connection: accepted and a frame decoded within 5 s.
    QElapsedTimer next;
    next.start();
    Probe again({QStringLiteral("--no-krdpctl"), QStringLiteral("--silent"), QStringLiteral("--gfx"), QStringLiteral("--timeout"), QStringLiteral("30")});
    QTRY_VERIFY_WITH_TIMEOUT(again.log.contains("first frame") || again.process.state() == QProcess::NotRunning, 30000);
    const qint64 ms = next.elapsed();
    QVERIFY2(again.log.contains("first frame"), again.log.right(3000).constData());
    qInfo().noquote() << "T-K4b(ii) user server: the next client decoded its first frame after" << ms << "ms";
    QVERIFY2(ms <= 5000, qPrintable(QString::number(ms)));
#endif
}

// OPT-055 T-K4c: after an encoder thread was abandoned the user server exits 70 once it is idle for 10 s;
// the Restart=on-failure loop starts it again and it serves a client.
void WorkerEndToEndTest::userServerRestartsItselfIdleAfterAbandonedEncoder()
{
#ifndef KRDP_E2E_PROBE
    QSKIP("krdpctl-probe (FreeRDP client) is not built");
#else
    if (!m_skip.isEmpty()) QSKIP(qPrintable(m_skip));
    auto *s = session(1);
    QVERIFY(s);
    if (s->skip.startsWith(QLatin1Char('!'))) QFAIL(qPrintable(s->skip.mid(1)));
    if (!s->skip.isEmpty()) QSKIP(qPrintable(s->skip));
    const QString blockFile = s->runtime->path() + QStringLiteral("/block-filter");
    const QString exitFile = s->runtime->path() + QStringLiteral("/server-exit");
    QFile::remove(blockFile);
    const qint64 serverLogFrom = QFileInfo(s->home->path() + QStringLiteral("/server.log")).size();
    const auto serverLog = [&] {
        QFile f(s->home->path() + QStringLiteral("/server.log"));
        if (!f.open(QIODevice::ReadOnly) || !f.seek(serverLogFrom)) return QString();
        return QString::fromUtf8(f.readAll());
    };
    const auto cleanup = qScopeGuard([&] {
        QFile::remove(blockFile);
        stopUserServer(*s);
        if (QTest::currentTestFailed()) qWarning().noquote() << "server.log:\n" << serverLog().right(6000);
    });
    QVERIFY(startUserServer(*s, {QStringLiteral("LD_PRELOAD=%1").arg(QStringLiteral(KRDP_E2E_FAILFILTER)),
                                 QStringLiteral("FARSIDE_TEST_BLOCK_FILTER_FILE=%1").arg(blockFile)}, 3399));
    QTRY_VERIFY_WITH_TIMEOUT(portAccepts(3399), 60000);
    qint64 disconnectedAt = 0;
    QElapsedTimer clock;
    clock.start();
    {
        // Frames flow; then the encoder's filter call blocks (a driver that never returns); the idle desktop's
        // Refresh Rect (3 s without a frame) makes the encoder run and wedge.
        Probe probe({QStringLiteral("--no-krdpctl"), QStringLiteral("--silent"), QStringLiteral("--gfx"), QStringLiteral("--refresh-rect-idle"),
                     QStringLiteral("3000"), QStringLiteral("--timeout"), QStringLiteral("90")});
        QTRY_VERIFY_WITH_TIMEOUT(probe.log.contains("first frame"), 30000);
        {
            QFile f(blockFile);
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        QTRY_VERIFY_WITH_TIMEOUT(probe.log.contains("refresh rect sent"), 15000);
        QTest::qWait(1500); // the wedged producer is now stuck inside the call
        probe.process.terminate();
        probe.process.waitForFinished(5000);
        disconnectedAt = clock.elapsed();
    }
    // The connection is gone; KPipeWire gives up the wedged producer (2 s), krdp waits 10 s idle, then exits 70.
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(exitFile), 40000);
    const qint64 exitMs = clock.elapsed() - disconnectedAt;
    QFile code(exitFile);
    QVERIFY(code.open(QIODevice::ReadOnly));
    const QByteArray exitCode = code.readAll().trimmed();
    QFile::remove(blockFile); // the restarted instance must not wedge
    qInfo().noquote() << "T-K4c: the server exited with" << exitCode << exitMs << "ms after the client left";
    QCOMPARE(exitCode, QByteArray("70"));
    QVERIFY2(exitMs >= 9000 && exitMs <= 30000, qPrintable(QString::number(exitMs)));
    const QString log = serverLog();
    QVERIFY2(log.contains(QStringLiteral("wedged encoder thread(s) leaked; restarting while idle")), qPrintable(log.right(3000)));
    // Restarted by the loop: a client is served again.
    QTRY_VERIFY_WITH_TIMEOUT(portAccepts(3399), 60000);
    Probe again({QStringLiteral("--no-krdpctl"), QStringLiteral("--silent"), QStringLiteral("--gfx"), QStringLiteral("--timeout"), QStringLiteral("30")});
    QTRY_VERIFY_WITH_TIMEOUT(again.log.contains("first frame") || again.process.state() == QProcess::NotRunning, 30000);
    QVERIFY2(again.log.contains("first frame"), again.log.right(3000).constData());
#endif
}

void WorkerEndToEndTest::cleanupTestCase()
{
    for (auto &[outputs, s] : m_sessions) {
        if (s.process && s.process->state() != QProcess::NotRunning) {
            // bwrap is the PID namespace's init: its exit ends every process of the session.
            s.process->terminate();
            if (!s.process->waitForFinished(5000)) {
                s.process->kill();
                s.process->waitForFinished(5000);
            }
        }
        if (s.runtime) s.runtime->remove();
        if (s.home) s.home->remove();
    }
}

QTEST_GUILESS_MAIN(WorkerEndToEndTest)

#include "WorkerEndToEndTest.moc"
