// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>
#include <vector>

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
    explicit ConsoleWorkerLauncher(QString workerProgram, QObject *parent = nullptr);
    ~ConsoleWorkerLauncher() override;
    bool launch(const ConsoleHandoff::Target &target, const QString &socketName, const QByteArray &token, QString *error = nullptr);

Q_SIGNALS:
    void workerExited(const QString &socketName);

private:
    QString m_workerProgram;
    std::vector<std::unique_ptr<QProcess>> m_processes;
};
}
