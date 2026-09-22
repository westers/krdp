// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionSupervisor.h"
#include "VirtualSessionControl.h"
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
        handle = created;
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
    quint64 controlGeneration = 0;
    VirtualSessionControl control(supervisor, [&](quint64, const auto &) {
        endpoint.setControlState({++controlGeneration, false});
        endpoint.setMedia({false, false});
    });
    quint64 requestId = 0;
    const auto command = [&](quint64 client, const QString &action, const QString &session = QString()) {
        QJsonObject record{{QStringLiteral("type"), QStringLiteral("virtual-session")}, {QStringLiteral("v"), 1},
                           {QStringLiteral("id"), QString::number(++requestId)}, {QStringLiteral("action"), action}};
        if (!session.isEmpty()) record.insert(QStringLiteral("session"), session);
        // Probe uses its own verified OS identity, not PAM or client JSON.
        return control.request(getuid(), client, record).value(QStringLiteral("ok")).toBool();
    };
    bool accepted = false;
    bool stopping = false;
    int result = 1;
    ConsoleWorkerWire::Outputs outputs;
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::workerReady, &app, [&](const auto &) { endpoint.requestKeyFrame(); });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::outputsReceived, &app, [&](const auto &value) { outputs = value; });
    QObject::connect(&endpoint, &ConsoleWorkerEndpoint::frameReceived, &app, [&](const VideoFrame &frame) {
        if (accepted || !handle || !frame.isKeyFrame || frame.data.isEmpty() || frame.size != QSize(1280, 720)
            || outputs.monitors.size() != 1 || outputs.monitors[0].geometry != QRect(0, 0, 1280, 720)) return;
        if (!supervisor.captureReady(*handle) || !command(1, QStringLiteral("attach"), handle->id)) {
            app.quit();
            return;
        }
        endpoint.setControlState({++controlGeneration, true});
        if (!command(1, QStringLiteral("detach"))) { app.quit(); return; }
        accepted = true;
        qInfo() << "Managed desktop ready; first transport detached at generation" << handle->generation;
        QTimer::singleShot(1000, &app, [&] {
            const auto state = supervisor.list(getuid());
            if (state.size() != 1 || state.first().phase != VirtualSessionState::Phase::Retained) {
                app.quit();
                return;
            }
            if (!command(2, QStringLiteral("attach"), handle->id)) { app.quit(); return; }
            const auto resumed = control.attachment(2);
            if (!resumed || resumed->generation != handle->generation) {
                app.quit();
                return;
            }
            qInfo() << "Managed desktop reattached without recreation";
            endpoint.setControlState({++controlGeneration, true});
            stopping = command(2, QStringLiteral("stop"), handle->id);
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
    if (!command(1, QStringLiteral("create")) || !handle) return 1;
    app.exec();
    qInfo() << "Real namespace supervisor create/ready/detach/reattach/stop result" << result;
    return result;
}
