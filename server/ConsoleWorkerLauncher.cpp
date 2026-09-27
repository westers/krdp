// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleWorkerLauncher.h"

#include <algorithm>
#include <csignal>
#include <grp.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QProcess>

#include "ConsoleSeat.h"
#include "ConsoleWorkerWire.h"

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

ConsoleWorkerLauncher::~ConsoleWorkerLauncher()
{
    // The broker is going away: do not leave a worker holding the seat.
    for (auto &[socket, process] : m_processes) {
        process->disconnect(this);
        process->terminate();
        if (!process->waitForFinished(2000)) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

void ConsoleWorkerLauncher::setSessionLookup(SessionLookup lookup)
{
    m_sessionLookup = std::move(lookup);
}

void ConsoleWorkerLauncher::signalWorker(const QString &socketName, int signal)
{
    const auto found = m_processes.find(socketName);
    if (found == m_processes.end() || found->second->state() == QProcess::NotRunning) {
        return;
    }
    const qint64 pid = found->second->processId();
    if (pid > 0) {
        ::kill(pid_t(pid), signal);
    }
}

bool ConsoleWorkerLauncher::launch(const ConsoleHandoff::Target &target, const QString &socketName, const QByteArray &token, QString *error)
{
    if (error) {
        error->clear();
    }
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (target.adapter != ConsoleSeat::Adapter::Greeter && target.adapter != ConsoleSeat::Adapter::PhysicalUser) {
        return fail(QStringLiteral("physical launcher cannot launch a virtual registry target"));
    }
    if (m_processes.count(socketName)) {
        return fail(QStringLiteral("a worker for this launch is already running"));
    }
    const auto found = m_sessionLookup ? m_sessionLookup(target.sessionId) : std::nullopt;
    if (!found || found->uid != target.uid) {
        return fail(QStringLiteral("selected logind session disappeared"));
    }
    QString environmentError;
    auto environment = environmentFor(*found, &environmentError);
    if (environment.isEmpty()) {
        return fail(environmentError);
    }
    const QString libraryPath = qEnvironmentVariable("LD_LIBRARY_PATH");
    // These descriptor numbers belong to the process whose environment we
    // inspected. The worker must open a fresh connection by display name.
    environment.remove(QStringLiteral("WAYLAND_SOCKET"));
    // A worker belongs to one compositor for its entire lifetime. Plasma's
    // reconnect mode can block Qt during logout, preventing Stop/socket-close
    // from reaching our event loop and leaving the broker draining forever.
    // Qt tests presence, so setting this to "0" would still enable it.
    environment.remove(QStringLiteral("QT_WAYLAND_RECONNECT"));
    if (!libraryPath.isEmpty()) {
        environment.insert(QStringLiteral("LD_LIBRARY_PATH"), libraryPath);
    }
    // Keep the per-launch socket path out of argv (world-readable /proc).
    environment.insert(QString::fromLatin1(ConsoleWorkerWire::SocketEnvironment), socketName);

    // All name-service lookups happen here, before fork: getpwuid() and
    // getgrouplist() may allocate, lock or talk to nscd/sssd, none of which is
    // safe between fork and exec (AUD-C-10). The child only makes syscalls.
    const passwd *account = getpwuid(target.uid);
    if (!account) {
        return fail(QStringLiteral("selected uid has no passwd entry"));
    }
    const gid_t gid = account->pw_gid;
    std::vector<gid_t> groups(32);
    int groupCount = int(groups.size());
    if (getgrouplist(account->pw_name, gid, groups.data(), &groupCount) < 0) {
        if (groupCount <= 0 || groupCount > 65536) {
            return fail(QStringLiteral("cannot resolve the selected user's groups"));
        }
        groups.resize(size_t(groupCount));
        if (getgrouplist(account->pw_name, gid, groups.data(), &groupCount) < 0) {
            return fail(QStringLiteral("cannot resolve the selected user's groups"));
        }
    }
    groups.resize(size_t(groupCount));

    int tokenPipe[2] = {-1, -1};
    if (pipe2(tokenPipe, O_CLOEXEC) != 0) {
        return fail(QStringLiteral("cannot create worker token pipe"));
    }
    auto closePipe = [&tokenPipe]() {
        for (int &fd : tokenPipe) {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
        }
    };
    auto process = std::make_unique<QProcess>();
    QProcess *raw = process.get();
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    process->setProcessEnvironment(environment);
    process->setProgram(m_workerProgram);
    QStringList arguments{QStringLiteral("--logind-session"), target.sessionId, QStringLiteral("--uid"), QString::number(target.uid),
                          QStringLiteral("--token-fd"), QStringLiteral("3")};
    if (target.adapter == ConsoleSeat::Adapter::PhysicalUser && ConsoleSeat::isPhysicalUser(*found)) {
        arguments.append(QStringLiteral("--desktop-media"));
    }
    process->setArguments(arguments);
    // Close every inherited descriptor except stdio and the token (fd 3),
    // which the modifier below installs before Qt closes the rest.
    QProcess::UnixProcessParameters parameters;
    parameters.flags = QProcess::UnixProcessFlag::CloseFileDescriptors | QProcess::UnixProcessFlag::ResetSignalHandlers
        | QProcess::UnixProcessFlag::IgnoreSigPipe;
    parameters.lowestFileDescriptorToClose = 4;
    process->setUnixProcessParameters(parameters);
    process->setChildProcessModifier([raw, uid = target.uid, gid, groups, tokenRead = tokenPipe[0]]() {
        if ((tokenRead == 3 ? fcntl(3, F_SETFD, 0) : dup2(tokenRead, 3)) < 0 || setgroups(groups.size(), groups.data()) != 0 || setgid(gid) != 0 || setuid(uid) != 0 || setuid(0) == 0) {
            raw->failChildProcessModifier("cannot drop privileges into logind session");
        }
    });
    connect(raw, &QProcess::errorOccurred, this, [raw](QProcess::ProcessError) {
        qWarning().noquote() << "Console worker launch error:" << raw->errorString();
    });
    connect(raw, &QProcess::finished, this, [this, socketName](int exitCode, QProcess::ExitStatus status) {
        qWarning().noquote() << "Console worker exited:" << exitCode << status;
        // Reap: forget the process before anyone may start a successor.
        const auto found = m_processes.find(socketName);
        if (found != m_processes.end()) {
            found->second.release()->deleteLater();
            m_processes.erase(found);
        }
        Q_EMIT workerExited(socketName);
    });
    const ssize_t written = write(tokenPipe[1], token.constData(), size_t(token.size()));
    close(tokenPipe[1]);
    tokenPipe[1] = -1;
    if (written != token.size()) {
        closePipe();
        return fail(QStringLiteral("cannot deliver worker token"));
    }
    process->start();
    if (!process->waitForStarted(3000)) {
        const QString message = process->errorString();
        closePipe();
        return fail(message);
    }
    closePipe();
    m_processes.emplace(socketName, std::move(process));
    return true;
}
}
