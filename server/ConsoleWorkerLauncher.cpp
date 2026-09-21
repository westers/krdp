// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleWorkerLauncher.h"

#include <algorithm>
#include <grp.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QProcess>

#include "ConsoleSeat.h"

namespace KRdp
{
namespace
{
QProcessEnvironment readProcessEnvironment(pid_t pid)
{
    QFile file(QStringLiteral("/proc/%1/environ").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QProcessEnvironment environment;
    for (const auto &entry : file.readAll().split('\0')) {
        const int equals = entry.indexOf('=');
        if (equals > 0) {
            environment.insert(QString::fromLocal8Bit(entry.first(equals)), QString::fromLocal8Bit(entry.mid(equals + 1)));
        }
    }
    return environment;
}

bool isUsableWaylandEnvironment(const QProcessEnvironment &environment, const ConsoleSeat::Session &session)
{
    const QString runtime = environment.value(QStringLiteral("XDG_RUNTIME_DIR"));
    const QString expectedRuntime = QStringLiteral("/run/user/%1").arg(session.uid);
    return runtime == expectedRuntime && !environment.value(QStringLiteral("WAYLAND_DISPLAY")).isEmpty()
        && !environment.value(QStringLiteral("DBUS_SESSION_BUS_ADDRESS")).isEmpty();
}

bool belongsToSession(pid_t pid, const ConsoleSeat::Session &session)
{
    const QString scope = QStringLiteral("session-%1.scope").arg(session.id);
    QFile cgroup(QStringLiteral("/proc/%1/cgroup").arg(pid));
    return cgroup.open(QIODevice::ReadOnly) && cgroup.readAll().contains(scope.toUtf8());
}

QProcessEnvironment environmentFor(const ConsoleSeat::Session &session, QString *error)
{
    if (session.leader != 0) {
        const auto environment = readProcessEnvironment(session.leader);
        if (isUsableWaylandEnvironment(environment, session)) {
            return environment;
        }
    }

    const auto processes = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &process : processes) {
        bool ok = false;
        const auto pid = process.toLongLong(&ok);
        if (!ok || pid <= 0 || QFileInfo(QStringLiteral("/proc/%1").arg(pid)).ownerId() != session.uid) {
            continue;
        }
        const auto environment = readProcessEnvironment(pid);
        // Plasma starts applications as user-manager services outside the
        // logind scope, but retains the originating session ID in their env.
        if (!belongsToSession(pid, session)
            && environment.value(QStringLiteral("XDG_SESSION_ID")) != session.id) {
            continue;
        }
        if (isUsableWaylandEnvironment(environment, session)) {
            return environment;
        }
    }
    *error = QStringLiteral("no process in the logind session has a usable Wayland environment");
    return {};
}
}

ConsoleWorkerLauncher::ConsoleWorkerLauncher(QString workerProgram, QObject *parent)
    : QObject(parent)
    , m_workerProgram(std::move(workerProgram))
{
}

ConsoleWorkerLauncher::~ConsoleWorkerLauncher() = default;

bool ConsoleWorkerLauncher::launch(const ConsoleHandoff::Target &target, const QString &socketName, const QByteArray &token, QString *error)
{
    if (error) {
        error->clear();
    }
    const auto sessions = ConsoleSeat::readLogindSessions(error);
    const auto found = std::find_if(sessions.cbegin(), sessions.cend(), [&target](const auto &session) {
        return session.id == target.sessionId && session.uid == target.uid;
    });
    if (found == sessions.cend()) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("selected logind session disappeared");
        }
        return false;
    }
    QString environmentError;
    auto environment = environmentFor(*found, &environmentError);
    if (environment.isEmpty()) {
        if (error) {
            *error = environmentError;
        }
        return false;
    }
    const QString libraryPath = qEnvironmentVariable("LD_LIBRARY_PATH");
    // These descriptor numbers belong to the process whose environment we
    // inspected. The worker must open a fresh connection by display name.
    environment.remove(QStringLiteral("WAYLAND_SOCKET"));
    if (!libraryPath.isEmpty()) {
        environment.insert(QStringLiteral("LD_LIBRARY_PATH"), libraryPath);
    }
    const passwd *account = getpwuid(target.uid);
    if (!account) {
        if (error) {
            *error = QStringLiteral("selected uid has no passwd entry");
        }
        return false;
    }
    int tokenPipe[2] = {-1, -1};
    if (pipe(tokenPipe) != 0) {
        if (error) {
            *error = QStringLiteral("cannot create worker token pipe");
        }
        return false;
    }
    auto closePipe = [&tokenPipe]() {
        if (tokenPipe[0] >= 0) {
            close(tokenPipe[0]);
            tokenPipe[0] = -1;
        }
        if (tokenPipe[1] >= 0) {
            close(tokenPipe[1]);
            tokenPipe[1] = -1;
        }
    };
    auto process = std::make_unique<QProcess>();
    QProcess *raw = process.get();
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    process->setProcessEnvironment(environment);
    process->setProgram(m_workerProgram);
    process->setArguments({QStringLiteral("--socket"), socketName, QStringLiteral("--logind-session"), target.sessionId, QStringLiteral("--uid"), QString::number(target.uid), QStringLiteral("--token-fd"), QStringLiteral("3")});
    const gid_t gid = account->pw_gid;
    const QByteArray user = QByteArray(account->pw_name);
    process->setChildProcessModifier([raw, target, gid, user, tokenRead = tokenPipe[0]]() {
        if (dup2(tokenRead, 3) < 0 || (tokenRead != 3 && close(tokenRead) != 0) || initgroups(user.constData(), gid) != 0 || setgid(gid) != 0 || setuid(target.uid) != 0) {
            raw->failChildProcessModifier("cannot drop privileges into logind session");
        }
    });
    connect(raw, &QProcess::errorOccurred, this, [raw](QProcess::ProcessError) {
        qWarning().noquote() << "Console worker launch error:" << raw->errorString();
    });
    connect(raw, &QProcess::finished, this, [this, raw, socketName](int exitCode, QProcess::ExitStatus status) {
        qWarning().noquote() << "Console worker exited:" << exitCode << status << raw->errorString();
        Q_EMIT workerExited(socketName);
    });
    const ssize_t written = write(tokenPipe[1], token.constData(), size_t(token.size()));
    close(tokenPipe[1]);
    tokenPipe[1] = -1;
    if (written != token.size()) {
        if (error) {
            *error = QStringLiteral("cannot deliver worker token");
        }
        closePipe();
        return false;
    }
    process->start();
    if (!process->waitForStarted(3000)) {
        if (error) {
            *error = process->errorString();
        }
        closePipe();
        return false;
    }
    close(tokenPipe[0]);
    tokenPipe[0] = -1;
    m_processes.push_back(std::move(process));
    return true;
}
}
