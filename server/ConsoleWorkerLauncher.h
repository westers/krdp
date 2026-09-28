// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>

#include <QObject>
#include <QSet>
#include <QProcess>

#include "ConsoleHandoff.h"

namespace KRdp
{
/** Launches a capture worker inside an existing logind Wayland session. */
class ConsoleWorkerLauncher final : public QObject
{
    Q_OBJECT

public:
    using SessionLookup = std::function<std::optional<ConsoleSeat::Session>(const QString &sessionId)>;
    /** The Wayland environment of a process in the session (default: scan /proc). */
    using EnvironmentProbe = std::function<QProcessEnvironment(const ConsoleSeat::Session &, QString *error)>;
    /** Whether a Wayland socket accepts a connection (default: AF_UNIX connect). */
    using SocketProbe = std::function<bool(const QString &path)>;
    /** Starts the worker process; replaced only by tests (the real one drops privileges). */
    using Starter = std::function<bool(const ConsoleHandoff::Target &, const ConsoleSeat::Session &, const QString &socketName,
                                       const QByteArray &token, QProcessEnvironment environment, QString *error)>;

    explicit ConsoleWorkerLauncher(QString workerProgram, QObject *parent = nullptr);
    ~ConsoleWorkerLauncher() override;
    /** Cached logind data (ConsoleSeatWatcher); the launcher never blocks on the system bus. */
    void setSessionLookup(SessionLookup lookup);
    void setEnvironmentProbe(EnvironmentProbe probe);
    void setSocketProbe(SocketProbe probe);
    void setStarter(Starter starter);
    void setWaitBudgetMs(int budgetMs) { m_waitBudgetMs = budgetMs; }
    /** AUD-FIX8: the host's VaapiDriverMode (VaapiDriverMode::normalize()) for every worker. */
    void setVaapiDriverMode(const QString &mode) { m_vaapiDriverMode = mode; }
    QString vaapiDriverMode() const { return m_vaapiDriverMode; }
    /**
     * Start a worker for @p target. When the session is not ready yet (AUD-FIX
     * F6: not Active, no Wayland environment, or its socket not accepting
     * connections) the launch is accepted and started later, polling with
     * backoff within a bounded budget; if that runs out, or the session goes
     * away, workerExited() reports the launch as ended without a process.
     */
    bool launch(const ConsoleHandoff::Target &target, const QString &socketName, const QByteArray &token, QString *error = nullptr);
    /** Signal a still-running launch (a waiting one is cancelled); a no-op once it has been reaped. */
    void signalWorker(const QString &socketName, int signal);
    qsizetype runningCount() const { return qsizetype(m_processes.size() + m_pending.size()); }
    qsizetype waitingCount() const { return qsizetype(m_pending.size()); }

Q_SIGNALS:
    /** Emitted once the launch's process has been reaped and forgotten (or a waiting launch ended). */
    void workerExited(const QString &socketName);

private:
    struct Pending;
    struct Observation;
    Observation observe(const ConsoleHandoff::Target &target) const;
    void retryPending(const QString &socketName);
    void endPending(const QString &socketName);
    bool startProcess(const ConsoleHandoff::Target &target, const ConsoleSeat::Session &session, const QString &socketName,
                      const QByteArray &token, QProcessEnvironment environment, QString *error);

    QString m_workerProgram;
    SessionLookup m_sessionLookup;
    EnvironmentProbe m_environmentProbe;
    SocketProbe m_socketProbe;
    Starter m_starter;
    int m_waitBudgetMs;
    QString m_vaapiDriverMode = QStringLiteral("auto");
    QSet<uid_t> m_renderWarned; // one render-group warning per user
    std::map<QString, std::unique_ptr<QProcess>> m_processes;
    std::map<QString, std::unique_ptr<Pending>> m_pending;
};
}
