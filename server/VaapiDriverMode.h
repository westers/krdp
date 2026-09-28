// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QProcessEnvironment>
#include <QString>

namespace KRdp::VaapiDriverMode
{
/**
 * AUD-FIX8 B3: krdpserver's VaapiDriverMode for the root brokers' capture workers
 * (KRDP_CONSOLE_VAAPI_DRIVER in /etc/krdp/console-host.conf, KRDP_VIRTUAL_VAAPI_DRIVER in
 * /etc/krdp/virtual-session.conf): auto (default), off, radeonsi, iHD or i965. nullopt: invalid.
 */
inline std::optional<QString> normalize(const QString &value)
{
    const QString mode = value.trimmed();
    if (mode.isEmpty() || mode.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) return QStringLiteral("auto");
    if (mode.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0 || mode.compare(QLatin1String("disabled"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("off");
    if (mode.compare(QLatin1String("radeonsi"), Qt::CaseInsensitive) == 0) return QStringLiteral("radeonsi");
    if (mode.compare(QLatin1String("ihd"), Qt::CaseInsensitive) == 0) return QStringLiteral("iHD");
    if (mode.compare(QLatin1String("i965"), Qt::CaseInsensitive) == 0) return QStringLiteral("i965");
    return std::nullopt;
}

/**
 * What the worker's KRdp::selectVaapiDriver() reads, set in its environment: a driver name
 * forces LIBVA_DRIVER_NAME (over one inherited from the desktop session), "off" stops the automatic
 * choice, "auto" leaves both unset (an inherited LIBVA_DRIVER_NAME is then respected).
 */
inline void apply(QProcessEnvironment &environment, const QString &normalized)
{
    environment.remove(QStringLiteral("KRDP_FORCE_VAAPI_DRIVER"));
    environment.remove(QStringLiteral("KRDP_AUTO_VAAPI_DRIVER"));
    if (normalized == QLatin1String("off")) {
        environment.insert(QStringLiteral("KRDP_AUTO_VAAPI_DRIVER"), QStringLiteral("0"));
    } else if (normalized != QLatin1String("auto") && !normalized.isEmpty()) {
        environment.insert(QStringLiteral("KRDP_FORCE_VAAPI_DRIVER"), normalized);
        // The administrator's choice wins over one inherited from the desktop session.
        environment.insert(QStringLiteral("LIBVA_DRIVER_NAME"), normalized);
    }
}
}
