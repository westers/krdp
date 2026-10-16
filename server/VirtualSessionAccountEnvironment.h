// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QProcessEnvironment>
#include <QString>

namespace KRdp
{
/**
 * OPT-073: identity variables of a Virtual session, taken from the OS account
 * database (never from a client or the broker's environment). A normal login
 * gets SHELL from the passwd entry (pam_env/login); the Virtual launcher runs
 * under `env -i`, so without SHELL Konsole warned "Could not find ''".
 */
struct VirtualSessionAccountEnvironment {
    static constexpr const char *fallbackShell = "/bin/bash";

    // pw_shell is empty on some accounts and may name a non-login or relative
    // path; only an absolute path without control characters is accepted.
    static QString usableShell(const QString &passwdShell)
    {
        if (!passwdShell.startsWith(QLatin1Char('/')) || passwdShell.contains(QLatin1Char('\n'))) return QString::fromLatin1(fallbackShell);
        for (const QChar c : passwdShell)
            if (c.unicode() < 0x20 || c.unicode() == 0x7f) return QString::fromLatin1(fallbackShell);
        return passwdShell;
    }

    static void apply(QProcessEnvironment &environment, const QString &login, const QString &home, const QString &passwdShell)
    {
        environment.insert(QStringLiteral("HOME"), home);
        environment.insert(QStringLiteral("USER"), login);
        environment.insert(QStringLiteral("LOGNAME"), login);
        environment.insert(QStringLiteral("SHELL"), usableShell(passwdShell));
    }
};
}
