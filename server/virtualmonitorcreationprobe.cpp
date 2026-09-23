// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// Disposable Sol-only proof that a retained KWin can gain and then lose one
// output while its original two outputs remain untouched. This is deliberately
// not a production topology command or a capability advertisement.
#include "RetainedKScreenReadback.h"

#include <PlasmaScreencastV1Session.h>

#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QTimer>
#include <QDebug>

#include <algorithm>
#include <cmath>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

using namespace KRdp;

namespace
{
std::optional<RetainedKScreenReadback::Snapshot> readKScreen()
{
    QProcess process;
    process.start(QStringLiteral("kscreen-doctor"), {QStringLiteral("-j")});
    if (!process.waitForStarted(1000) || !process.waitForFinished(3000)
        || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    return RetainedKScreenReadback::parse(process.readAllStandardOutput(), QStringLiteral("private-probe"));
}

const RemoteTopologyCatalog::Output *named(const RetainedKScreenReadback::Snapshot &snapshot, const QString &name)
{
    const auto it = std::find_if(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&name](const auto &output) {
        return output.backendKey == name;
    });
    return it == snapshot.outputs.cend() ? nullptr : &*it;
}
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    app.setDesktopFileName(QStringLiteral("org.kde.krdpvirtualmonitorprobe"));
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QFileInfo info(runtime);
    struct stat permissions{};
    char hostname[256] = {};
    if (argc != 1 || gethostname(hostname, sizeof(hostname) - 1)
        || QByteArray(hostname).split('.').first() != "sol" || !getuid() || getuid() != geteuid()
        || !runtime.startsWith(QStringLiteral("/run/user/%1/krdp-headless.").arg(getuid()))
        || info.canonicalFilePath() != runtime || info.ownerId() != getuid()
        || stat(QFile::encodeName(runtime).constData(), &permissions) || (permissions.st_mode & 0777) != 0700
        || qEnvironmentVariable("WAYLAND_DISPLAY") != QStringLiteral("wayland-0")) {
        qCritical("Virtual creation probe requires Sol's owned private compositor");
        return 1;
    }
    const auto before = readKScreen();
    if (!before || before->outputs.size() != 2 || qGuiApp->screens().size() != 2) return 1;
    const QString name = QStringLiteral("Virtual-krdp-probe-extra");
    if (named(*before, name)) return 1;
    const QRect desktop = before->outputs[0].logicalGeometry.united(before->outputs[1].logicalGeometry);
    const QPoint wanted(desktop.right() + 1, desktop.top());

    auto creator = std::make_unique<PlasmaScreencastV1Session>();
    creator->setVirtualMonitor(VirtualMonitor{QStringLiteral("krdp-probe-extra"), QSize(960, 540), 1.0});
    creator->setVideoCodec(VideoCodec::Avc420);
    creator->setStreamingEnabled(true);
    enum class Phase { Creating, Removing } phase = Phase::Creating;
    bool placed = false;
    int elapsed = 0;
    QTimer poll;
    poll.setInterval(200);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        elapsed += poll.interval();
        if (elapsed > 20000) {
            qCritical("Timed out waiting for private virtual-output creation/removal");
            app.exit(1);
            return;
        }
        const auto current = readKScreen();
        if (!current) return;
        if (phase == Phase::Creating) {
            const auto *extra = named(*current, name);
            if (!extra || current->outputs.size() != 3 || qGuiApp->screens().size() != 3) return;
            if (extra->nativePixels != QSize(960, 540) || std::abs(extra->scale - 1.0) > 0.001) {
                qCritical("Created output mode/scale differs from request");
                app.exit(1);
                return;
            }
            for (const auto &original : before->outputs) {
                const auto *still = named(*current, original.backendKey);
                if (!still || still->logicalGeometry != original.logicalGeometry || still->primary != original.primary) {
                    qCritical("Creating output changed an existing screen");
                    app.exit(1);
                    return;
                }
            }
            if (!placed) {
                const auto args = RetainedKScreenReadback::positionArguments(*current, {{name, wanted}});
                if (!args) { app.exit(1); return; }
                QProcess command;
                command.start(QStringLiteral("kscreen-doctor"), *args);
                if (!command.waitForStarted(1000) || !command.waitForFinished(3000)
                    || command.exitStatus() != QProcess::NormalExit || command.exitCode() != 0) {
                    command.kill();
                    command.waitForFinished(1000);
                    qCritical("Could not position new private output");
                    app.exit(1);
                    return;
                }
                placed = true;
                return;
            }
            if (extra->logicalGeometry.topLeft() != wanted) return;
            qInfo() << "Created private output" << name << "at" << wanted << "without changing existing screens";
            creator.reset();
            phase = Phase::Removing;
        } else if (current->outputs.size() == 2 && qGuiApp->screens().size() == 2 && !named(*current, name)) {
            for (const auto &original : before->outputs) {
                const auto *still = named(*current, original.backendKey);
                if (!still || still->logicalGeometry != original.logicalGeometry || still->primary != original.primary) {
                    qCritical("Removing owned output changed an existing screen");
                    app.exit(1);
                    return;
                }
            }
            qInfo("Owned private output disappeared; original two screens remained unchanged");
            app.exit(0);
        }
    });
    poll.start();
    return app.exec();
}
