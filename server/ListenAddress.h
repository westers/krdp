// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QHostAddress>
#include <QString>

namespace KRdp
{
/**
 * The address krdpserver listens on, from `ListenAddress` in krdpserverrc or
 * `--address` (AUD-S4). Empty or `*` means every interface (IPv4 and IPv6),
 * which is the default. Anything else must be a literal IPv4 or IPv6 address;
 * std::nullopt means it is not one, and the server refuses to start rather
 * than fall back to listening everywhere.
 */
inline std::optional<QHostAddress> parseListenAddress(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty() || trimmed == QLatin1String("*")) {
        return QHostAddress(QHostAddress::Any);
    }
    QHostAddress address;
    if (!address.setAddress(trimmed)) {
        return std::nullopt;
    }
    return address;
}
}
