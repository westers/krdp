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

#include <unistd.h>

#include "ConsoleWorkerEndpoint.h"
#include "EncoderSupport.h"
#include "H264KeyframeSize.h"
#include "RenderNodes.h"

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
kwin_wayland --virtual --width 1280 --height 720 --output-count 1 --socket wayland-0 \
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

class WorkerEndToEndTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void workerReachesReadyAndDeliversFrames_data();
    void workerReachesReadyAndDeliversFrames();
    void cleanupTestCase();

private:
    QString log(const QString &name, int tail = 3000) const;

    std::unique_ptr<QTemporaryDir> m_runtime;
    std::unique_ptr<QTemporaryDir> m_home;
    std::unique_ptr<QProcess> m_session;
    QString m_skip;
    QString m_renderNode;
};

QString WorkerEndToEndTest::log(const QString &name, int tail) const
{
    QFile file(m_home->path() + QLatin1Char('/') + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll().right(tail)) : QString();
}

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
    const QString userRuntime = QStringLiteral("/run/user/%1").arg(getuid());
    // A short path: the Wayland and PipeWire socket names must fit sockaddr_un.
    const QString runtimeBase = QFileInfo(userRuntime).isDir() ? userRuntime : QDir::tempPath();
    m_runtime = std::make_unique<QTemporaryDir>(runtimeBase + QStringLiteral("/krdp-e2e-XXXXXX"));
    m_home = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/krdp-e2e-home-XXXXXX"));
    QVERIFY(m_runtime->isValid() && m_home->isValid());
    QVERIFY(QFile::setPermissions(m_runtime->path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    for (const auto *dir : {"config", "data/applications", "cache", "state"}) {
        QVERIFY(QDir(m_home->path()).mkpath(QString::fromLatin1(dir)));
    }
    const QString runtime = m_runtime->path();
    const QString home = m_home->path();

    QFile bus(runtime + QStringLiteral("/bus.conf"));
    QVERIFY(bus.open(QIODevice::WriteOnly));
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
    QVERIFY(desktop.open(QIODevice::WriteOnly));
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
    if (qEnvironmentVariableIsSet("KRDP_E2E_LOGGING_RULES")) set("QT_LOGGING_RULES", qEnvironmentVariable("KRDP_E2E_LOGGING_RULES"));

    m_session = std::make_unique<QProcess>();
    m_session->setProcessEnvironment(env);
    m_session->setWorkingDirectory(home);
    m_session->setStandardOutputFile(home + QStringLiteral("/session.log"));
    m_session->setStandardErrorFile(home + QStringLiteral("/session.log"), QIODevice::Append);
    m_session->start(QStandardPaths::findExecutable(QStringLiteral("bwrap")),
                     {QStringLiteral("--unshare-pid"), QStringLiteral("--die-with-parent"), QStringLiteral("--ro-bind"), QStringLiteral("/"), QStringLiteral("/"),
                      QStringLiteral("--proc"), QStringLiteral("/proc"), QStringLiteral("--dev"), QStringLiteral("/dev"), QStringLiteral("--dev-bind"),
                      m_renderNode, m_renderNode, QStringLiteral("--bind"), runtime, runtime, QStringLiteral("--bind"), home, home,
                      QStringLiteral("/bin/bash"), QStringLiteral("-c"), QString::fromLatin1(SessionScript)});
    QVERIFY(m_session->waitForStarted(5000));
    QElapsedTimer timer;
    timer.start();
    while (!QFileInfo::exists(runtime + QStringLiteral("/session-ready")) && m_session->state() == QProcess::Running && timer.elapsed() < 60000) {
        QTest::qWait(50);
    }
    if (!QFileInfo::exists(runtime + QStringLiteral("/session-ready"))) {
        qWarning().noquote() << "session.log:" << log(QStringLiteral("session.log")) << "\nkwin.log:" << log(QStringLiteral("kwin.log"));
        if (m_session->state() != QProcess::Running && log(QStringLiteral("session.log")).contains(QStringLiteral("bwrap"))) {
            m_skip = QStringLiteral("bwrap cannot create a sandbox here");
            return;
        }
        QFAIL("the private headless session did not come up");
    }
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
    const QString runtime = m_runtime->path();
    const QString session = virtualDesktop ? QUuid::createUuid().toString(QUuid::WithoutBraces) : QStringLiteral("c1");
    QFile::remove(runtime + QStringLiteral("/worker-exit"));
    QFile args(runtime + QStringLiteral("/worker-args"));
    QVERIFY(args.open(QIODevice::WriteOnly));
    args.write((virtualDesktop ? QStringLiteral("--virtual-session\n%1\n") : QStringLiteral("--logind-session\n%1\n")).arg(session).toUtf8());
    args.close();
    const QByteArray token = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
    QFile tokenFile(runtime + QStringLiteral("/worker-token"));
    QVERIFY(tokenFile.open(QIODevice::WriteOnly));
    QVERIFY(tokenFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    tokenFile.write(token);
    tokenFile.close();

    ConsoleWorkerEndpoint endpoint;
    QStringList order;
    QStringList errors;
    std::optional<ConsoleWorkerWire::EncoderCaps> caps;
    int keyframes = 0;
    int frames = 0;
    QSize keyframeSize;
    bool payloadMatches = false;
    connect(&endpoint, &ConsoleWorkerEndpoint::encoderCapsReceived, this, [&](const auto &value) {
        caps = value;
        order << QStringLiteral("caps");
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, this, [&](const auto &) {
        order << QStringLiteral("ready");
        endpoint.setControlState({1, true});
        endpoint.requestKeyFrame();
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::encoderReported, this, [&](const auto &report) {
        order << QStringLiteral("report:%1:%2").arg(int(report.codec)).arg(report.hardware ? QStringLiteral("hw") : QStringLiteral("sw"));
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [&](const auto &) {
        if (!order.contains(QStringLiteral("outputs"))) order << QStringLiteral("outputs");
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [&](const VideoFrame &frame) {
        ++frames;
        if (frame.isKeyFrame) {
            ++keyframes;
            keyframeSize = frame.size;
            payloadMatches = h264KeyframeSize(frame.data) == std::optional(frame.size);
            if (!order.contains(QStringLiteral("keyframe"))) order << QStringLiteral("keyframe");
        }
    });
    connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, this, [&](const QString &message) {
        errors << message;
    });
    const auto dumpLogs = qScopeGuard([&] {
        if (!QTest::currentTestFailed()) return;
        qWarning().noquote() << "worker.log:\n" << log(QStringLiteral("worker.log"), 4000) << "\nkwin.log:\n" << log(QStringLiteral("kwin.log"), 2000)
                             << "\nsession.log:\n" << log(QStringLiteral("session.log"), 1000) << "\nsycoca.log:\n" << log(QStringLiteral("sycoca.log"), 1500);
    });
    // The session's loop starts the worker as soon as this socket exists (virtual-session-desktop.sh).
    QString error;
    QVERIFY2(endpoint.listen(runtime + QStringLiteral("/worker.sock"), {virtualDesktop ? ConsoleSeat::Adapter::VirtualUser : ConsoleSeat::Adapter::PhysicalUser, session, quint32(getuid())}, token, &error),
             qPrintable(error));
    const QString exitFile = runtime + QStringLiteral("/worker-exit");

    QTRY_VERIFY_WITH_TIMEOUT(endpoint.ready() || !errors.isEmpty() || QFileInfo::exists(exitFile), 45000);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    QVERIFY2(endpoint.ready(), "the real worker never confirmed capture");
    QTRY_VERIFY_WITH_TIMEOUT(keyframes >= 1 || !errors.isEmpty() || QFileInfo::exists(exitFile), 30000);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    QVERIFY(keyframes >= 1);
    QCOMPARE(keyframeSize, QSize(1280, 720));
    QVERIFY(payloadMatches);
    qInfo().noquote() << "Real worker order:" << order.join(QStringLiteral(", ")) << "-" << frames << "frames," << keyframes << "keyframes on"
                      << m_renderNode;
    // Hello (authentication), EncoderCaps, Ready, then everything else - and no pre-Ready failure.
    QCOMPARE(order.value(0), QStringLiteral("caps"));
    QCOMPARE(order.value(1), QStringLiteral("ready"));
    QCOMPARE(order.count(QStringLiteral("caps")), 1);
    QCOMPARE(order.count(QStringLiteral("ready")), 1);
    QVERIFY(caps);
    if (!EncoderSupport::probeUncached().renderNode.isEmpty()) {
        // B3: the worker's own probe, inside the sandbox, sees the host's hardware encoder.
        QVERIFY2(caps->encoders.avc.hardware, "the sandboxed worker's probe found no hardware encoder");
        QCOMPARE(caps->renderNode, m_renderNode);
    }

    endpoint.stopWorker();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(exitFile), 15000);
    QFile exitCode(exitFile);
    QVERIFY(exitCode.open(QIODevice::ReadOnly));
    QCOMPARE(exitCode.readAll().trimmed(), QByteArray("0"));
}

void WorkerEndToEndTest::cleanupTestCase()
{
    if (m_session && m_session->state() != QProcess::NotRunning) {
        // bwrap is the PID namespace's init: its exit ends every process of the session.
        m_session->terminate();
        if (!m_session->waitForFinished(5000)) {
            m_session->kill();
            m_session->waitForFinished(5000);
        }
    }
    if (m_runtime) m_runtime->remove();
    if (m_home) m_home->remove();
}

QTEST_GUILESS_MAIN(WorkerEndToEndTest)

#include "WorkerEndToEndTest.moc"
