// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionSupervisor.h"
#include "ConsoleWorkerEndpoint.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <unistd.h>

using namespace KRdp;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) != 0 || QByteArray(host).split('.').first() != "sol"
        || !getuid() || getuid() != geteuid()) {
        qCritical("Acceptance probe is restricted to ordinary-user Sol");
        return 1;
    }
    if (app.arguments().size() != 2) return 1;
    const QString script = app.arguments().at(1);
    if (!QDir::isAbsolutePath(script)) return 1;
    QTemporaryDir directory(QStringLiteral("/run/user/%1/krdp-headless.XXXXXX").arg(getuid()));
    if (!directory.isValid()) return 1;
    directory.setAutoRemove(false); // keep bounded diagnostic evidence
    qInfo().noquote() << "Supervised desktop evidence:" << directory.path();
    ConsoleWorkerEndpoint endpoint;
    std::optional<VirtualSessionRegistry::Handle> handle;
    VirtualSessionSupervisor supervisor([&](quint32 uid, const auto &created) -> std::optional<VirtualSessionSupervisor::Launch> {
        if (uid != getuid()) return {};
        const auto token = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
        QFile tokenFile(directory.filePath(QStringLiteral("worker-token")));
        if (!tokenFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            || !tokenFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            || tokenFile.write(token) != token.size()) return {};
        tokenFile.close();
        QString error;
        if (!endpoint.listen(directory.filePath(QStringLiteral("worker.sock")),
                             {ConsoleSeat::Adapter::VirtualUser, created.id, uid}, token, &error)) {
            qCritical().noquote() << error;
            return {};
        }
        QProcessEnvironment env;
        env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
        env.insert(QStringLiteral("HOME"), QDir::homePath());
        return VirtualSessionSupervisor::Launch{QStringLiteral("/usr/bin/bash"),
            {script, QStringLiteral("--supervised-worker-nvidia"), directory.path(), created.id}, env, {}};
    }, 60000, 3000);
    bool accepted = false;
    bool stopping = false;
    int result = 1;
    ConsoleWorkerWire::Outputs outputs;
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, &app, [&](const auto &) { endpoint.requestKeyFrame(); });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, &app, [&](const auto &value) { outputs = value; });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::frameReceived, &app, [&](const VideoFrame &frame) {
        if (accepted || !handle || !frame.isKeyFrame || frame.data.isEmpty() || frame.size != QSize(1280, 720)
            || outputs.monitors.size() != 1 || outputs.monitors[0].geometry != QRect(0, 0, 1280, 720)) return;
        if (!supervisor.captureReady(*handle) || !supervisor.attach(getuid(), handle->id, 1)
            || !supervisor.disconnect(*handle, 1)) {
            app.quit();
            return;
        }
        accepted = true;
        qInfo() << "Managed desktop ready; first transport detached at generation" << handle->generation;
        QTimer::singleShot(1000, &app, [&] {
            const auto state = supervisor.list(getuid());
            if (state.size() != 1 || state.first().phase != VirtualSessionState::Phase::Retained) {
                app.quit();
                return;
            }
            const auto resumed = supervisor.attach(getuid(), handle->id, 2);
            if (!resumed || resumed->generation != handle->generation) {
                app.quit();
                return;
            }
            qInfo() << "Managed desktop reattached without recreation";
            stopping = supervisor.stop(getuid(), handle->id);
            if (!stopping) app.quit();
        });
    });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::protocolError, &app, [&](const QString &message) {
        qCritical().noquote() << message;
        if (!stopping) app.quit();
    });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        const auto entries = supervisor.list(getuid());
        if (entries.size() != 1) return;
        if (entries.first().phase == VirtualSessionState::Phase::Failed) app.quit();
        if (stopping && entries.first().phase == VirtualSessionState::Phase::Absent) {
            result = accepted ? 0 : 1;
            app.quit();
        }
    });
    poll.start(100);
    QTimer::singleShot(75000, &app, &QCoreApplication::quit);
    handle = supervisor.create(getuid());
    if (!handle) return 1;
    app.exec();
    qInfo() << "Real namespace supervisor create/ready/detach/reattach/stop result" << result;
    return result;
}
