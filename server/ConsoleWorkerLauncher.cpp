// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleWorkerLauncher.h"

#include "RenderAccess.h"
#include "VaapiDriverMode.h"

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
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

#include <sys/socket.h>
#include <sys/un.h>

#include "ConsoleSeat.h"
#include "ConsoleSessionReadiness.h"
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

bool socketAcceptsConnections(const QString &path)
{
    const QByteArray encoded = QFile::encodeName(path);
    sockaddr_un address{};
    if (encoded.isEmpty() || size_t(encoded.size()) >= sizeof(address.sun_path)) {
        return false;
    }
    address.sun_family = AF_UNIX;
    std::copy(encoded.cbegin(), encoded.cend(), address.sun_path);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    // A listening compositor accepts at once; a stale file refuses.
    const bool connected = ::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
    ::close(fd);
    return connected;
}
}

struct ConsoleWorkerLauncher::Pending {
    ConsoleHandoff::Target target;
    QByteArray token;
    QElapsedTimer waited;
    int attempt = 0;
    QString lastReason;
    QTimer timer;
};

struct ConsoleWorkerLauncher::Observation {
    ConsoleSessionReadiness::Observation readiness;
    ConsoleSessionReadiness::Result result;
};

ConsoleWorkerLauncher::ConsoleWorkerLauncher(QString workerProgram, QObject *parent)
    : QObject(parent)
    , m_workerProgram(std::move(workerProgram))
    , m_environmentProbe(environmentFor)
    , m_socketProbe(socketAcceptsConnections)
    , m_waitBudgetMs(ConsoleSessionReadiness::WaitBudgetMs)
{
    m_starter = [this](const ConsoleHandoff::Target &target, const ConsoleSeat::Session &session, const QString &socketName,
                       const QByteArray &token, QProcessEnvironment environment, QString *error) {
        return startProcess(target, session, socketName, token, std::move(environment), error);
    };
}

ConsoleWorkerLauncher::~ConsoleWorkerLauncher()
{
    m_pending.clear();
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

void ConsoleWorkerLauncher::setEnvironmentProbe(EnvironmentProbe probe)
{
    m_environmentProbe = std::move(probe);
}

void ConsoleWorkerLauncher::setSocketProbe(SocketProbe probe)
{
    m_socketProbe = std::move(probe);
}

void ConsoleWorkerLauncher::setStarter(Starter starter)
{
    m_starter = std::move(starter);
}

void ConsoleWorkerLauncher::signalWorker(const QString &socketName, int signal)
{
    if (m_pending.count(socketName)) {
        // Nothing runs yet: a stop (or kill) simply cancels the wait.
        qInfo().noquote() << "Console worker launch" << socketName << "cancelled while waiting for its session";
        endPending(socketName);
        return;
    }
    const auto found = m_processes.find(socketName);
    if (found == m_processes.end() || found->second->state() == QProcess::NotRunning) {
        return;
    }
    const qint64 pid = found->second->processId();
    if (pid > 0) {
        ::kill(pid_t(pid), signal);
    }
}

ConsoleWorkerLauncher::Observation ConsoleWorkerLauncher::observe(const ConsoleHandoff::Target &target) const
{
    Observation observation;
    auto &readiness = observation.readiness;
    readiness.expectedUid = target.uid;
    readiness.session = m_sessionLookup ? m_sessionLookup(target.sessionId) : std::nullopt;
    if (readiness.session && readiness.session->uid == target.uid && readiness.session->state == QLatin1String("active") && readiness.session->active) {
        readiness.environment = m_environmentProbe ? m_environmentProbe(*readiness.session, &readiness.environmentError) : QProcessEnvironment();
        const QString socket = ConsoleSessionReadiness::waylandSocketPath(readiness.environment);
        readiness.socketConnectable = !socket.isEmpty() && m_socketProbe && m_socketProbe(socket);
    }
    observation.result = ConsoleSessionReadiness::evaluate(readiness);
    return observation;
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
    if (m_processes.count(socketName) || m_pending.count(socketName)) {
        return fail(QStringLiteral("a worker for this launch is already running"));
    }
    auto observation = observe(target);
    using ConsoleSessionReadiness::Verdict;
    switch (observation.result.verdict) {
    case Verdict::Gone:
        return fail(observation.result.reason);
    case Verdict::Ready:
        return m_starter && m_starter(target, *observation.readiness.session, socketName, token, observation.readiness.environment, error);
    case Verdict::Wait:
        break;
    }
    // AUD-FIX F6: accepted, started once the session is ready.
    auto pending = std::make_unique<Pending>();
    pending->target = target;
    pending->token = token;
    pending->waited.start();
    pending->lastReason = observation.result.reason;
    pending->timer.setSingleShot(true);
    connect(&pending->timer, &QTimer::timeout, this, [this, socketName] { retryPending(socketName); });
    pending->timer.start(ConsoleSessionReadiness::retryDelayMs(0));
    qInfo().noquote() << "Waiting for session" << target.sessionId << "before starting its console worker:" << observation.result.reason;
    m_pending.emplace(socketName, std::move(pending));
    return true;
}

void ConsoleWorkerLauncher::retryPending(const QString &socketName)
{
    const auto found = m_pending.find(socketName);
    if (found == m_pending.end()) {
        return;
    }
    auto &pending = *found->second;
    const auto observation = observe(pending.target);
    using ConsoleSessionReadiness::Verdict;
    if (observation.result.verdict == Verdict::Ready) {
        const auto target = pending.target;
        const auto token = pending.token;
        const qint64 waited = pending.waited.elapsed();
        m_pending.erase(found);
        QString error;
        if (m_starter && m_starter(target, *observation.readiness.session, socketName, token, observation.readiness.environment, &error)) {
            qInfo().noquote() << "Session" << target.sessionId << "ready after" << waited << "ms; console worker started";
            return;
        }
        qWarning().noquote() << "Cannot launch console worker for session" << target.sessionId << ':' << error;
        // Reported like a process that exited: the broker's failure backoff takes over.
        QTimer::singleShot(0, this, [this, socketName] { Q_EMIT workerExited(socketName); });
        return;
    }
    if (observation.result.verdict == Verdict::Gone || pending.waited.elapsed() >= m_waitBudgetMs) {
        qWarning().noquote() << "Not starting the console worker for session" << pending.target.sessionId << "after waiting"
                             << pending.waited.elapsed() << "ms:" << observation.result.reason;
        endPending(socketName);
        return;
    }
    if (observation.result.reason != pending.lastReason) {
        pending.lastReason = observation.result.reason;
        qInfo().noquote() << "Still waiting for session" << pending.target.sessionId << ':' << observation.result.reason;
    }
    pending.timer.start(ConsoleSessionReadiness::retryDelayMs(++pending.attempt));
}

void ConsoleWorkerLauncher::endPending(const QString &socketName)
{
    if (!m_pending.erase(socketName)) {
        return;
    }
    // Never re-enter the broker from inside its own launch/stop call.
    QTimer::singleShot(0, this, [this, socketName] { Q_EMIT workerExited(socketName); });
}

bool ConsoleWorkerLauncher::startProcess(const ConsoleHandoff::Target &target, const ConsoleSeat::Session &session, const QString &socketName,
                                         const QByteArray &token, QProcessEnvironment environment, QString *error)
{
    if (error) {
        error->clear();
    }
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const auto *found = &session;
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
    // AUD-FIX8 B3: the host's VaapiDriverMode, resolved by the worker like krdpserver does.
    VaapiDriverMode::apply(environment, m_vaapiDriverMode);

    // All name-service lookups happen here, before fork: getpwuid() and
    // getgrouplist() may allocate, lock or talk to nscd/sssd, none of which is
    // safe between fork and exec (AUD-C-10). The child only makes syscalls.
    const passwd *account = getpwuid(target.uid);
    if (!account) {
        return fail(QStringLiteral("selected uid has no passwd entry"));
    }
    const gid_t gid = account->pw_gid;
    // The user's full group list (render, video, ...), as initgroups() would install it.
    const auto resolvedGroups = RenderAccess::userGroups(account->pw_name, gid);
    if (!resolvedGroups) {
        return fail(QStringLiteral("cannot resolve the selected user's groups"));
    }
    const std::vector<gid_t> groups = *resolvedGroups;
    // The greeter's user (sddm) lives on the seat's ACL by design; only desktop users are told.
    if (target.adapter != ConsoleSeat::Adapter::Greeter && !m_renderWarned.contains(target.uid)) {
        m_renderWarned.insert(target.uid);
        const QString warning = RenderAccess::missingGroupWarning(QString::fromLocal8Bit(account->pw_name), target.uid, groups,
            RenderAccess::nodes(), QStringLiteral("its console worker can use the GPU only through the seat's ACL, so hardware "
                                                  "encoding can be missing (e.g. from the greeter or after a seat change)"));
        if (!warning.isEmpty()) qWarning().noquote() << warning;
    }

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
