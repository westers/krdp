// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <optional>

namespace KRdp::UserConfiguration
{
constexpr qint64 MaximumBytes = 256 * 1024;
/** Bounded, nonblocking read of a regular file owned by uid; never follows the final symlink. */
std::optional<QByteArray> readFile(const QString &path, quint32 uid);
/** Canonical account-home path; no process HOME/XDG or caller path. */
std::optional<QString> userPath(quint32 uid);
/** Reads ~/.config/farsideserverrc with the authenticated account's filesystem identity.
 * Root, unresolved accounts, failed identity changes and unreadable files produce no data.
 * No broker environment variables or user-supplied path participate in resolution.
 */
std::optional<QByteArray> readUser(quint32 uid);
}
