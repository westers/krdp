// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleSeat.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>

namespace KRdp::ConsoleSeat
{
namespace
{
QVariant property(const QDBusObjectPath &path, const QString &name)
{
    QDBusInterface properties(QStringLiteral("org.freedesktop.login1"), path.path(), QStringLiteral("org.freedesktop.DBus.Properties"), QDBusConnection::systemBus());
    const QDBusReply<QDBusVariant> reply = properties.call(QStringLiteral("Get"), QStringLiteral("org.freedesktop.login1.Session"), name);
    return reply.isValid() ? reply.value().variant() : QVariant{};
}
}

QList<Session> readLogindSessions(QString *error)
{
    if (error) {
        error->clear();
    }
    QDBusInterface manager(QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"), QStringLiteral("org.freedesktop.login1.Manager"), QDBusConnection::systemBus());
    const QDBusMessage reply = manager.call(QStringLiteral("ListSessions"));
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        if (error) {
            *error = reply.errorMessage().isEmpty() ? QStringLiteral("logind ListSessions returned no session list") : reply.errorMessage();
        }
        return {};
    }

    QList<Session> sessions;
    // The reply argument is read-only. Keep the QDBusArgument const so the
    // parser overloads are selected; a mutable `beginArray()` means "start
    // serialising" and aborts against this reply object.
    const QDBusArgument entries = qvariant_cast<QDBusArgument>(reply.arguments().constFirst());
    entries.beginArray();
    while (!entries.atEnd()) {
        QString id;
        quint32 uid = 0;
        QString user;
        QString seat;
        QDBusObjectPath path;
        entries.beginStructure();
        entries >> id >> uid >> user >> seat >> path;
        entries.endStructure();
        Session session;
        session.id = id;
        session.uid = uid;
        session.user = user;
        session.seat = seat;
        session.type = property(path, QStringLiteral("Type")).toString();
        session.sessionClass = property(path, QStringLiteral("Class")).toString();
        session.state = property(path, QStringLiteral("State")).toString();
        session.active = property(path, QStringLiteral("Active")).toBool();
        sessions.append(std::move(session));
    }
    entries.endArray();
    return sessions;
}
}
