// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "ConsoleWorkerEndpoint.h"
#include "H264KeyframeSize.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
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
    const bool dragProbe = arguments.size() == 2 && arguments[1] == QStringLiteral("--multi-drag");
    const bool mixed = arguments.size() == 2 && (arguments[1] == QStringLiteral("--multi-mixed") || inputProbe);
    const bool multi = arguments.size() == 2 && (arguments[1] == QStringLiteral("--multi") || mixed || dragProbe);
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
    bool inputVerified = !inputProbe && !dragProbe;
    bool postMoveCaptured = !dragProbe;
    bool stopping = false;
    int result = 1;
    ConsoleWorkerWire::Outputs outputs;
    const auto maybeStop = [&] {
        if (captured && resizeRefused && inputVerified && postMoveCaptured && !stopping) {
            stopping = true;
            endpoint.stopWorker();
        }
    };
    QTimer inputTimer;
    inputTimer.setInterval(100);
    QObject::connect(&inputTimer, &QTimer::timeout, &app, [&] {
        QFile log(runtime + QStringLiteral("/kwin.log"));
        if (!log.open(QIODevice::ReadOnly)) return;
        const auto contents = log.readAll();
        if (dragProbe) {
            if (!contents.contains("krdp-monitor-drag: outputChanged=Virtual-1")) return;
            endpoint.requestKeyFrame();
            qInfo("Private KWin observed worker-input drag move to Virtual-1");
        } else if (!contents.contains("krdp-monitor-probe: cursor=1224,300")) return;
        inputVerified = true;
        inputTimer.stop();
        if (!dragProbe) qInfo("Private KWin observed mixed-scale second-output pointer at logical (1224,300)");
        maybeStop();
    });
    QPointF dragFrom;
    QPointF dragTo;
    QTimer dragTimer;
    dragTimer.setInterval(50);
    int dragStep = 0;
    QObject::connect(&dragTimer, &QTimer::timeout, &app, [&] {
        ++dragStep;
        ConsoleWorkerWire::Input drag;
        drag.type = ConsoleWorkerWire::Input::Type::Mouse;
        drag.eventType = QEvent::MouseMove;
        drag.position = dragFrom + (dragTo - dragFrom) * (double(dragStep) / 14.0);
        drag.buttons = Qt::LeftButton;
        endpoint.sendInput(drag);
        if (dragStep == 14) {
            dragTimer.stop();
            drag.eventType = QEvent::MouseButtonRelease;
            drag.button = Qt::LeftButton;
            drag.buttons = Qt::NoButton;
            endpoint.sendInput(drag);
            inputTimer.start();
        }
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
        if (dragProbe && inputVerified && frame.monitorIndex == 1) {
            postMoveCaptured = true;
            qInfo("Captured second output again after KWin drag destination changed");
        }
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
        if (captured && dragProbe && !inputSent) {
            inputSent = true;
            QFile log(runtime + QStringLiteral("/kwin.log"));
            if (!log.open(QIODevice::ReadOnly)) { app.quit(); return; }
            const auto contents = log.readAll();
            const auto marker = QByteArrayLiteral("krdp-monitor-drag: before output=Virtual-0 geometry=");
            const int line = contents.indexOf(marker);
            const int startJson = line < 0 ? -1 : contents.indexOf('{', line);
            const int endJson = startJson < 0 ? -1 : contents.indexOf('}', startJson);
            if (endJson < 0) { app.quit(); return; }
            int lineEnd = contents.indexOf('\n', endJson);
            if (lineEnd < 0) lineEnd = contents.size();
            if (!contents.mid(endJson, lineEnd - endJson).contains("movable=true")) { app.quit(); return; }
            const auto geometry = QJsonDocument::fromJson(contents.mid(startJson, endJson - startJson + 1)).object();
            const double x = geometry.value(QStringLiteral("x")).toDouble(-1);
            const double y = geometry.value(QStringLiteral("y")).toDouble(-1);
            const double width = geometry.value(QStringLiteral("width")).toDouble(-1);
            if (x < 0 || y < 0 || width < 100) { app.quit(); return; }
            dragFrom = QPointF(x + std::min(width / 2.0, 200.0), y + 12.0);
            dragTo = QPointF(std::min(dragFrom.x() + 1400.0, 2400.0), dragFrom.y() + 40.0);
            qInfo() << "Dragging marked Konsole titlebar from" << dragFrom << "to" << dragTo;
            ConsoleWorkerWire::Input pointer;
            pointer.type = ConsoleWorkerWire::Input::Type::Mouse;
            pointer.eventType = QEvent::MouseMove;
            pointer.position = dragFrom;
            endpoint.sendInput(pointer);
            pointer.eventType = QEvent::MouseButtonPress;
            pointer.button = Qt::LeftButton;
            pointer.buttons = Qt::LeftButton;
            endpoint.sendInput(pointer);
            dragStep = 0;
            dragTimer.start();
        }
        maybeStop();
    });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, &app, [&](const QString &message) {
        qCritical().noquote() << message;
        app.quit();
    });
    if (managed) QObject::connect(&endpoint, &ConsoleWorkerEndpoint::workerStopped, &app, [&] {
        result = captured && resizeRefused && inputVerified && postMoveCaptured && stopping ? 0 : 1;
        app.quit();
    });
    QObject::connect(&worker, &QProcess::errorOccurred, &app, [&](auto) { app.quit(); });
    QObject::connect(&worker, &QProcess::finished, &app, [&](int code, QProcess::ExitStatus status) {
        result = captured && resizeRefused && inputVerified && postMoveCaptured && stopping && code == 0 && status == QProcess::NormalExit ? 0 : 1;
        app.quit();
    });
    QTimer::singleShot(inputProbe || dragProbe ? 30000 : 20000, &app, &QCoreApplication::quit);
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
