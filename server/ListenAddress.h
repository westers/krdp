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
/** Whether a ListenAddress value means every interface: empty, blank or `*`. */
inline bool listensOnAllInterfaces(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() || trimmed == QLatin1String("*");
}

inline std::optional<QHostAddress> parseListenAddress(const QString &value)
{
    if (listensOnAllInterfaces(value)) {
        return QHostAddress(QHostAddress::Any);
    }
    const QString trimmed = value.trimmed();
    QHostAddress address;
    if (!address.setAddress(trimmed)) {
        return std::nullopt;
    }
    return address;
}
}
