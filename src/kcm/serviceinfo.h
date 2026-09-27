// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace SystemdService
{
Q_NAMESPACE
enum Status {
    Unknown,
    Running,
    Stopped,
    Failed,
    // AUD-K9: activating/deactivating; the switch waits for the outcome.
    Busy,
};
Q_ENUM_NS(Status)

// Maps a unit's ActiveState.
Status statusFromActiveState(const QString &activeState);

// AUD-K5: a krdpserver option in the unit's ExecStart (usually from a
// drop-in) that makes the server ignore a setting on this page.
struct CommandLineOverride {
    QString option; // as written, without any value
    QString setting; // kcfg key it overrides
};

// argv includes argv[0]. Values are never returned (a drop-in might carry
// --password).
QList<CommandLineOverride> commandLineOverrides(const QStringList &argv);
}
