// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "serviceinfo.h"

#include <QHash>

#include <algorithm>

namespace SystemdService
{
Status statusFromActiveState(const QString &activeState)
{
    if (activeState == QLatin1String("active") || activeState == QLatin1String("reloading") || activeState == QLatin1String("refreshing")) {
        return Running;
    }
    if (activeState == QLatin1String("activating") || activeState == QLatin1String("deactivating")) {
        return Busy;
    }
    if (activeState == QLatin1String("failed")) {
        return Failed;
    }
    if (activeState == QLatin1String("inactive") || activeState == QLatin1String("maintenance")) {
        return Stopped;
    }
    return Unknown;
}

QList<CommandLineOverride> commandLineOverrides(const QStringList &argv)
{
    // krdpserver's options (server/main.cpp) that replace a krdpserverrc key.
    static const QHash<QString, QString> settingByOption{
        {QStringLiteral("--monitor"), QStringLiteral("MonitorMode")},
        {QStringLiteral("--virtual-monitor"), QStringLiteral("MonitorMode")},
        {QStringLiteral("--port"), QStringLiteral("ListenPort")},
        {QStringLiteral("--address"), QStringLiteral("ListenAddress")},
        {QStringLiteral("--quality"), QStringLiteral("Quality")},
        {QStringLiteral("--certificate"), QStringLiteral("Certificate")},
        {QStringLiteral("--certificate-key"), QStringLiteral("Certificate")},
        {QStringLiteral("-u"), QStringLiteral("Users")},
        {QStringLiteral("--username"), QStringLiteral("Users")},
        {QStringLiteral("-p"), QStringLiteral("Users")},
        {QStringLiteral("--password"), QStringLiteral("Users")},
    };

    QList<CommandLineOverride> overrides;
    for (qsizetype i = 1; i < argv.size(); ++i) {
        const QString &arg = argv.at(i);
        if (arg == QLatin1String("--")) {
            break;
        }
        QString option = arg.section(QLatin1Char('='), 0, 0);
        // Qt also accepts "-port" for long options.
        if (option.startsWith(QLatin1Char('-')) && !option.startsWith(QLatin1String("--")) && option.size() > 2) {
            option.prepend(QLatin1Char('-'));
        }
        const auto it = settingByOption.constFind(option);
        if (it == settingByOption.constEnd()) {
            continue;
        }
        const bool known = std::any_of(overrides.cbegin(), overrides.cend(), [&](const CommandLineOverride &o) {
            return o.option == option;
        });
        if (!known) {
            overrides.append({option, it.value()});
        }
    }
    return overrides;
}
}
