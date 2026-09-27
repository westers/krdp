// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>

#include <QObject>
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

    explicit ConsoleWorkerLauncher(QString workerProgram, QObject *parent = nullptr);
    ~ConsoleWorkerLauncher() override;
    /** Cached logind data (ConsoleSeatWatcher); the launcher never blocks on the system bus. */
    void setSessionLookup(SessionLookup lookup);
    bool launch(const ConsoleHandoff::Target &target, const QString &socketName, const QByteArray &token, QString *error = nullptr);
    /** Signal a still-running launch; a no-op once it has been reaped. */
    void signalWorker(const QString &socketName, int signal);
    qsizetype runningCount() const { return qsizetype(m_processes.size()); }

Q_SIGNALS:
    /** Emitted once the launch's process has been reaped and forgotten. */
    void workerExited(const QString &socketName);

private:
    QString m_workerProgram;
    SessionLookup m_sessionLookup;
    std::map<QString, std::unique_ptr<QProcess>> m_processes;
};
}
