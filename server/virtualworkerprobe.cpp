// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "ConsoleWorkerEndpoint.h"
#include "H264KeyframeSize.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryFile>
#include <QTimer>
#include <QSet>
#include <QUuid>
#include <QDebug>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace KRdp;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    const bool managed = arguments.size() == 3 && arguments[1] == QStringLiteral("--managed-session");
    const bool inputProbe = arguments.size() == 2 && arguments[1] == QStringLiteral("--multi-input");
    const bool mixed = arguments.size() == 2 && (arguments[1] == QStringLiteral("--multi-mixed") || inputProbe);
    const bool multi = arguments.size() == 2 && (arguments[1] == QStringLiteral("--multi") || mixed);
    if (arguments.size() != 1 && !managed && !multi) return 1;
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QFileInfo runtimeInfo(runtime);
    const QString prefix = managed ? QStringLiteral("/run/user/%1/krdp-virtual/").arg(getuid())
        : QStringLiteral("/run/user/%1/krdp-headless.").arg(getuid());
    struct stat runtimePermissions{};
    if (!getuid() || getuid() != geteuid() || !runtime.startsWith(prefix)
        || runtimeInfo.canonicalFilePath() != runtime || runtimeInfo.ownerId() != getuid()
        || stat(QFile::encodeName(runtime).constData(), &runtimePermissions) || (runtimePermissions.st_mode & 0777) != 0700
        || QFileInfo::exists(runtime + QStringLiteral("/worker.sock"))
        || QFileInfo(runtime + QStringLiteral("/worker.sock")).isSymLink()
        || !QFileInfo::exists(runtime + QStringLiteral("/wayland-0"))) {
        qCritical("Probe requires an existing owned private compositor runtime");
        return 1;
    }
    const QString id = managed ? arguments[2] : QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id) return 1;
    QByteArray token;
    QTemporaryFile tokenFile(runtime + QStringLiteral("/worker-token.XXXXXX"));
    if (managed) {
        const int fd = open(QFile::encodeName(runtime + QStringLiteral("/worker-token")).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return 1;
        struct stat info{};
        QFile credential;
        if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid()
            || (info.st_mode & 0777) != 0600 || info.st_size != 32 || info.st_nlink != 1) { close(fd); return 1; }
        if (!credential.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { close(fd); return 1; }
        token = credential.readAll();
        if (token.size() != 32) return 1;
    } else {
        token = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
        if (!tokenFile.open() || tokenFile.write(token) != token.size() || !tokenFile.flush()) return 1;
        tokenFile.close();
    }
    ConsoleWorkerEndpoint endpoint;
    QString error;
    if (!endpoint.listen(runtime + QStringLiteral("/worker.sock"), {ConsoleSeat::Adapter::VirtualUser, id, quint32(getuid())}, token, &error)) {
        qCritical().noquote() << error;
        return 1;
    }
    QProcess worker;
    worker.setProcessChannelMode(QProcess::ForwardedChannels);
    worker.setStandardInputFile(tokenFile.fileName());
    auto environment = QProcessEnvironment::systemEnvironment(); // already namespace-private
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-0"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
    worker.setProcessEnvironment(environment);
    worker.setProgram(QCoreApplication::applicationDirPath() + QStringLiteral("/krdp-console-worker"));
    worker.setArguments({QStringLiteral("--socket"), endpoint.socketName(), QStringLiteral("--virtual-session"), id,
                         QStringLiteral("--uid"), QString::number(getuid()), QStringLiteral("--token-fd"), QStringLiteral("0"),
                         QStringLiteral("--desktop-media")});
    bool captured = false;
    QSet<int> capturedOutputs;
    bool resizeRefused = false;
    bool inputSent = false;
    bool inputVerified = !inputProbe;
    bool stopping = false;
    int result = 1;
    ConsoleWorkerWire::Outputs outputs;
    const auto maybeStop = [&] {
        if (captured && resizeRefused && inputVerified && !stopping) {
            stopping = true;
            endpoint.stopWorker();
        }
    };
    QTimer inputTimer;
    inputTimer.setInterval(100);
    QObject::connect(&inputTimer, &QTimer::timeout, &app, [&] {
        QFile log(runtime + QStringLiteral("/kwin.log"));
        if (!log.open(QIODevice::ReadOnly)) return;
        if (!log.readAll().contains("krdp-monitor-probe: cursor=1224,300")) return;
        inputVerified = true;
        inputTimer.stop();
        qInfo("Private KWin observed mixed-scale second-output pointer at logical (1224,300)");
        maybeStop();
    });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, &app, [&](const auto &) {
        endpoint.setControlState({1, true});
        endpoint.resize({1, 1, QStringLiteral("Virtual-0"), QSize(1024, 768), 1});
        endpoint.requestKeyFrame();
    });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, &app, [&](const auto &value) { outputs = value; });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::resizeFinished, &app, [&](const auto &value) {
        resizeRefused = value.requestId == 1 && value.generation == 1
            && value.error == (multi ? QStringLiteral("single-output Fit is unavailable with multiple remote monitors")
                : QStringLiteral("Physical-output resize is unavailable in a virtual session"));
        maybeStop();
    });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::frameReceived, &app, [&](const VideoFrame &frame) {
        const int expected = multi ? 2 : 1;
        if (!frame.isKeyFrame || frame.data.isEmpty() || frame.size != QSize(1280, 720)
            || h264KeyframeSize(frame.data) != frame.size || frame.monitorIndex < 0 || frame.monitorIndex >= expected
            || outputs.monitors.size() != expected || frame.monitors.size() != expected) return;
        for (int i = 0; i < expected; ++i) {
            const QRect logical = mixed && i == 0 ? QRect(0, 0, 1024, 576)
                : mixed && i == 1 ? QRect(1024, 100, 1280, 720) : QRect(i * 1280, 0, 1280, 720);
            const QRect wire = mixed && i == 1 ? QRect(1280, 100, 1280, 720) : QRect(i * 1280, 0, 1280, 720);
            if (outputs.monitors[i].geometry != logical || frame.monitors[i].geometry != wire
                || outputs.monitors[i].scale != (mixed && i == 0 ? 1.25 : 1.0)) return;
        }
        QFile output(runtime + (multi ? QStringLiteral("/worker-keyframe-%1.h264").arg(frame.monitorIndex)
                                      : QStringLiteral("/worker-keyframe.h264")));
        if (!output.open(QIODevice::WriteOnly) || output.write(frame.data) != frame.data.size()) return;
        capturedOutputs.insert(frame.monitorIndex);
        captured = capturedOutputs.size() == expected;
        qInfo() << "Authenticated virtual worker keyframe" << frame.monitorIndex << frame.size << frame.data.size();
        if (captured && inputProbe && !inputSent) {
            inputSent = true;
            // Deliberately send no separate motion. The worker must position
            // fake-input at this packet's logical point before clicking.
            ConsoleWorkerWire::Input click;
            click.type = ConsoleWorkerWire::Input::Type::Mouse;
            click.eventType = QEvent::MouseButtonPress;
            click.position = QPointF(1480, 300); // output 1 pixel atlas -> KWin logical (1224,300)
            click.button = Qt::LeftButton;
            click.buttons = Qt::LeftButton;
            endpoint.sendInput(click);
            click.eventType = QEvent::MouseButtonRelease;
            click.buttons = Qt::NoButton;
            endpoint.sendInput(click);
            inputTimer.start();
        }
        maybeStop();
    });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, &app, [&](const QString &message) {
        qCritical().noquote() << message;
        app.quit();
    });
    if (managed) QObject::connect(&endpoint, &ConsoleWorkerEndpoint::workerStopped, &app, [&] {
        result = captured && resizeRefused && inputVerified && stopping ? 0 : 1;
        app.quit();
    });
    QObject::connect(&worker, &QProcess::errorOccurred, &app, [&](auto) { app.quit(); });
    QObject::connect(&worker, &QProcess::finished, &app, [&](int code, QProcess::ExitStatus status) {
        result = captured && resizeRefused && inputVerified && stopping && code == 0 && status == QProcess::NormalExit ? 0 : 1;
        app.quit();
    });
    QTimer::singleShot(inputProbe ? 25000 : 20000, &app, &QCoreApplication::quit);
    // Managed mode owns only the broker endpoint. The existing desktop loop
    // supplies its worker; this process must never enter or stop the guardian.
    if (!managed) worker.start();
    app.exec();
    if (worker.state() != QProcess::NotRunning) {
        worker.kill();
        worker.waitForFinished(3000);
        result = 1;
    }
    qInfo() << "Worker capture/virtual-resize refusal/scoped stop result" << result;
    return result;
}
