// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace KRdp::ConsoleSeat
{
/** The subset of logind session data a console host needs to choose an adapter.
 *
 * Keep this value type independent of QDBus: the system-bus reader is an
 * integration detail, while the policy needs direct unit coverage for the
 * login/greeter/user handoff.
 */
struct Session {
    QString id;
    QString user;
    QString seat;
    QString type;
    QString sessionClass;
    QString state;
    bool active = false;
    quint32 uid = 0;
    quint32 leader = 0;
};

enum class Adapter {
    None,
    Greeter,
    PhysicalUser,
};

/** A session usable as the physical console for @p seat (normally `seat0`). */
inline bool isPhysicalUser(const Session &session, const QString &seat = QStringLiteral("seat0"))
{
    return session.active && session.seat == seat && session.sessionClass == QLatin1String("user")
        && session.type == QLatin1String("wayland") && session.state == QLatin1String("active") && !session.id.isEmpty();
}

/** The active display-manager greeter on @p seat. */
inline bool isGreeter(const Session &session, const QString &seat = QStringLiteral("seat0"))
{
    return session.active && session.seat == seat && session.sessionClass == QLatin1String("greeter")
        && session.state == QLatin1String("active") && !session.id.isEmpty();
}

/**
 * Prefer the logged-in physical user. During SDDM login that session appears
 * only after the greeter starts to disappear; this priority prevents a stale
 * greeter record from taking input after the desktop is available.
 */
inline Adapter adapterFor(const QList<Session> &sessions, const QString &seat = QStringLiteral("seat0"))
{
    for (const auto &session : sessions) {
        if (isPhysicalUser(session, seat)) {
            return Adapter::PhysicalUser;
        }
    }
    for (const auto &session : sessions) {
        if (isGreeter(session, seat)) {
            return Adapter::Greeter;
        }
    }
    return Adapter::None;
}

inline QString activeSessionId(const QList<Session> &sessions, Adapter adapter, const QString &seat = QStringLiteral("seat0"))
{
    for (const auto &session : sessions) {
        if ((adapter == Adapter::PhysicalUser && isPhysicalUser(session, seat)) || (adapter == Adapter::Greeter && isGreeter(session, seat))) {
            return session.id;
        }
    }
    return {};
}

/// Numeric uid of the selected live session; used to launch its capture worker.
inline std::optional<quint32> activeSessionUid(const QList<Session> &sessions, Adapter adapter, const QString &seat = QStringLiteral("seat0"))
{
    for (const auto &session : sessions) {
        if (((adapter == Adapter::PhysicalUser && isPhysicalUser(session, seat)) || (adapter == Adapter::Greeter && isGreeter(session, seat))) && session.uid != 0) {
            return session.uid;
        }
    }
    return std::nullopt;
}

/** Read logind sessions from the system bus. Returns an empty list with an
 * explanatory message when the system service is unavailable. */
QList<Session> readLogindSessions(QString *error = nullptr);
}
