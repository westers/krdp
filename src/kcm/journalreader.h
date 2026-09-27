// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QString>
#include <QStringList>

// AUD-K7: the last `maxLines` krdpserver messages of one invocation of the
// user unit, oldest first. Blocking (it reads the journal), so the KCM calls
// it from a worker thread. Empty without systemd.
QStringList readServiceJournal(const QString &unit, const QString &invocationId, int maxLines = 20);

// Keeps the newest `maxLines` of lines collected newest-first and returns
// them oldest first.
QStringList newestLinesInOrder(const QStringList &newestFirst, int maxLines);
