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
#include <QUuid>
#include <QHash>
#include <QSet>

#include <map>

#include <unistd.h>

#include "ConsoleWorkerEndpoint.h"
#include "EncoderSupport.h"
#include "H264KeyframeSize.h"
#include "RenderNodes.h"
#include "VideoCodecSupport.h"

using namespace KRdp;

namespace
{
/// The worker under test; KRDP_E2E_WORKER runs another build's (e.g. a released one) for comparison.
QString workerProgram()
{
    return qEnvironmentVariable("KRDP_E2E_WORKER", QStringLiteral(KRDP_CONSOLE_WORKER));
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
kwin_wayland --virtual --width 1280 --height 720 --output-count "$KRDP_E2E_OUTPUTS" --socket wayland-0 \
    --no-lockscreen --no-global-shortcuts --no-kactivities >"$HOME/kwin.log" 2>&1 & children="$children $!"
wait_for "$R/wayland-0"
sleep 1
: >"$R/session-ready"
set +e
# One worker per broker socket, like the desktop loop; worker-args says which kind (console or virtual).
while true; do
    while [ ! -S "$R/worker.sock" ] || [ ! -f "$R/worker-args" ]; do sleep 0.05; done
    mapfile -t args <"$R/worker-args"
    rm -f "$R/worker-args"
    env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland "$KRDP_CONSOLE_WORKER" "${args[@]}" --uid "$(id -u)" \
        --socket "$R/worker.sock" --token-fd 0 --desktop-media <"$R/worker-token" >>"$HOME/worker.log" 2>&1
    echo $? >"$R/worker-exit"
done
)SH";
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
    void codecSwitchAtAttach_data();
    void codecSwitchAtAttach();
    void cleanupTestCase();

private:
    /// The session with \a outputs KWin virtual outputs, started on first use (nullptr: failed).
    PrivateSession *session(int outputs);
    /// Starts one worker in \a session behind \a endpoint and records what it sends into \a run.
    bool startWorker(PrivateSession &session, bool virtualDesktop, ConsoleWorkerEndpoint &endpoint, WorkerRun &run);
    void stopWorker(PrivateSession &session, ConsoleWorkerEndpoint &endpoint);

    std::map<int, PrivateSession> m_sessions;
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
    if (m_renderNode.isEmpty()) m_renderNode = RenderNodes::list().value(0);
    if (m_renderNode.isEmpty()) {
        m_skip = QStringLiteral("no render node: KWin needs one for OpenGL compositing and screencast");
        return;
    }
}

PrivateSession *WorkerEndToEndTest::session(int outputs)
{
    if (const auto it = m_sessions.find(outputs); it != m_sessions.end()) {
        return it->second.process ? &it->second : nullptr;
    }
    auto &s = m_sessions[outputs];
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
                             "<policy context=\"default\"><allow send_destination=\"*\"/><allow receive_sender=\"*\"/><allow own=\"*\"/></policy>\n"
                             "</busconfig>\n")
                  .arg(runtime)
                  .toUtf8());
    bus.close();
    // KWin grants the screencast and fake-input protocols to this exact worker executable.
    QFile desktop(home + QStringLiteral("/data/applications/org.kde.krdpconsoleworker.desktop"));
    if (!desktop.open(QIODevice::WriteOnly)) return nullptr;
    desktop.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=KRDP Virtual Capture\nNoDisplay=true\nExec=%1\n"
                                 "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,org_kde_kwin_fake_input\n")
                      .arg(workerProgram())
                      .toUtf8());
    desktop.close();

    // Built from nothing: no DISPLAY, WAYLAND_DISPLAY, session bus or PipeWire of the user's session.
    QProcessEnvironment env;
    const auto set = [&env](const char *name, const QString &value) {
        env.insert(QString::fromLatin1(name), value);
    };
    set("PATH", QStringLiteral("/usr/bin:/bin"));
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
    set("KRDP_SERVER_DIR", QStringLiteral(KRDP_SERVER_DIR));
    set("KRDP_CONSOLE_WORKER", workerProgram());
    set("KRDP_E2E_OUTPUTS", QString::number(outputs));
    if (qEnvironmentVariableIsSet("KRDP_E2E_LOGGING_RULES")) set("QT_LOGGING_RULES", qEnvironmentVariable("KRDP_E2E_LOGGING_RULES"));

    auto process = std::make_unique<QProcess>();
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(home);
    process->setStandardOutputFile(home + QStringLiteral("/session.log"));
    process->setStandardErrorFile(home + QStringLiteral("/session.log"), QIODevice::Append);
    process->start(QStandardPaths::findExecutable(QStringLiteral("bwrap")),
                   {QStringLiteral("--unshare-pid"), QStringLiteral("--die-with-parent"), QStringLiteral("--ro-bind"), QStringLiteral("/"), QStringLiteral("/"),
                    QStringLiteral("--proc"), QStringLiteral("/proc"), QStringLiteral("--dev"), QStringLiteral("/dev"), QStringLiteral("--dev-bind"),
                    m_renderNode, m_renderNode, QStringLiteral("--bind"), runtime, runtime, QStringLiteral("--bind"), home, home,
                    QStringLiteral("/bin/bash"), QStringLiteral("-c"), QString::fromLatin1(SessionScript)});
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
    if (!EncoderSupport::probeUncached().renderNode.isEmpty()) {
        // B3: the worker's own probe, inside the sandbox, sees the host's hardware encoder.
        QVERIFY2(run.caps->encoders.avc.hardware, "the sandboxed worker's probe found no hardware encoder");
        QCOMPARE(run.caps->renderNode, m_renderNode);
    }
    stopWorker(*s, endpoint);
}

void WorkerEndToEndTest::codecSwitchAtAttach_data()
{
    // AUD-FIX9 R1: a client attaches to a worker that is Ready and idle, and its codec policy
    // moves the encoders from AVC to a hardware HEVC/AV1 at once ("immediate": in the same read as
    // the new grant, as on ace and cray) or after the grant's capture refresh has published its
    // outputs ("late"). Two outputs take the per-output (multi) capture path, one the single one.
    QTest::addColumn<bool>("virtualDesktop");
    QTest::addColumn<int>("outputs");
    QTest::addColumn<int>("codecId");
    QTest::addColumn<bool>("late");
    for (const bool virtualDesktop : {true, false}) {
        for (const int outputs : {2, 1}) {
            for (const auto codec : {VideoCodec::Hevc, VideoCodec::Av1}) {
                for (const bool late : {false, true}) {
                    if (late && outputs == 1) continue;
                    QTest::addRow("%s %d-output %s %s", virtualDesktop ? "virtual" : "console", outputs,
                                  codec == VideoCodec::Hevc ? "hevc" : "av1", late ? "late" : "immediate")
                        << virtualDesktop << outputs << int(codec) << late;
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
    const auto codec = VideoCodec(codecId);
    if (!m_skip.isEmpty()) {
        QSKIP(qPrintable(m_skip));
    }
    auto *s = session(outputs);
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
    const auto &backends = run.caps->encoders.of(codec == VideoCodec::Hevc ? CodecPolicy::Family::Hevc : CodecPolicy::Family::Av1);
    const auto host = EncoderSupport::probeUncached().encoders.of(codec == VideoCodec::Hevc ? CodecPolicy::Family::Hevc : CodecPolicy::Family::Av1);
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
        QTRY_VERIFY_WITH_TIMEOUT((outputs < 2 || run.outputs > outputsBefore) || !alive(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(run.frames.cbegin() + grantMark, run.frames.cend(), [](const auto &f) { return f.isKeyFrame; }) || !alive(), 5000);
        QVERIFY2(alive(), qPrintable(run.errors.join(QLatin1Char('\n'))));
    }
    ConsoleWorkerWire::EncoderConfig target{.generation = 1, .codec = codec, .settings = CodecPolicy::EncoderSettings{.hardware = true}};
    const qsizetype mark = run.frames.size();
    const qsizetype outputsAtSwitch = run.outputs;
    QElapsedTimer clock;
    clock.start();
    QVERIFY(endpoint.setEncoderConfig(target));

    // Within 2 s: a decodable keyframe of the new codec, at the output size, for every output.
    QSet<int> decoded;
    qsizetype checked = mark;
    qint64 elapsed = -1;
    while (clock.elapsed() < 2000 && alive()) {
        for (; checked < run.frames.size(); ++checked) {
            const auto &frame = run.frames[checked];
            if (frame.isKeyFrame && frame.codec == codec && frame.size == QSize(1280, 720)
                && encodedKeyframeSize(codec, frame.data) == std::optional(frame.size)) {
                decoded.insert(outputs > 1 ? frame.monitorIndex : 0);
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
                      << "outputs decoded in" << elapsed << "ms;" << newCodec << "new-codec and" << oldCodec << "older frames after the switch;"
                      << (run.outputs - outputsBefore) << "layouts published since the grant; order" << run.order.join(QStringLiteral(", "));
    QVERIFY2(decoded.size() == outputs, "no decodable keyframe of the new codec from every output within 2 s");
    if (outputs > 1) QVERIFY2(run.outputs > outputsBefore, "the new client's capture never published its outputs (KScreen readback)");
    const QString since = s->workerLogSince(grantLog);
    const QString restart = QStringLiteral("Codec changed to %1 on a running stream").arg(QLatin1String(VideoCodecSupport::codecName(codec)));
    if (outputs > 1 && !late) {
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
