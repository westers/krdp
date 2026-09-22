// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionLogin.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QRegularExpression>
#include <QVariantMap>

namespace KRdp {
bool VirtualSessionLogin::matches(uid_t owner, pid_t expectedLeader, const QString &pamId, const QString &pamRuntime) const
{
    return owner && owner != uid_t(-1) && expectedLeader > 1
        && uid == owner && leader == quint32(expectedLeader)
        && QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z")).match(id).hasMatch()
        && id == pamId && runtime == pamRuntime && runtime == QStringLiteral("/run/user/%1").arg(owner)
        && service == QStringLiteral("krdp-virtual-session")
        && type == QStringLiteral("wayland") && sessionClass == QStringLiteral("background")
        && (state == QStringLiteral("active") || state == QStringLiteral("online"))
        && seat.isEmpty() && tty.isEmpty() && display.isEmpty() && virtualTerminal == 0
        && scope == QStringLiteral("session-%1.scope").arg(id);
}

std::optional<VirtualSessionLogin> VirtualSessionLogin::read(pid_t leader)
{
    if (leader <= 1) return {};
    const auto bus = QDBusConnection::systemBus();
    const auto call = [&](const QString &path, const QString &interface, const QString &method, const QList<QVariant> &args) {
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.login1"), path, interface, method);
        message.setArguments(args);
        return bus.call(message, QDBus::Block, 3000);
    };
    const QDBusReply<QDBusObjectPath> sessionPath = call(QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("GetSessionByPID"), {quint32(leader)});
    if (!sessionPath.isValid() || !sessionPath.value().path().startsWith(QStringLiteral("/org/freedesktop/login1/session/"))) return {};
    const auto properties = [&](const QDBusObjectPath &path, const QString &interface) {
        return QDBusReply<QVariantMap>(call(path.path(), QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("GetAll"), {interface}));
    };
    const auto session = properties(sessionPath.value(), QStringLiteral("org.freedesktop.login1.Session"));
    if (!session.isValid()) return {};
    const auto fields = session.value();
    // Missing/incorrectly typed fields must not turn into empty seat/zero VT.
    for (const auto *field : {"Id", "Service", "Type", "Class", "State", "TTY", "Display", "Scope"})
        if (fields.value(QString::fromLatin1(field)).metaType().id() != QMetaType::QString) return {};
    for (const auto *field : {"Leader", "VTNr"})
        if (fields.value(QString::fromLatin1(field)).metaType().id() != QMetaType::UInt) return {};
    for (const auto *field : {"User", "Seat"})
        if (fields.value(QString::fromLatin1(field)).metaType() != QMetaType::fromType<QDBusArgument>()) return {};
    const auto user = qvariant_cast<QDBusArgument>(fields.value(QStringLiteral("User")));
    const auto seat = qvariant_cast<QDBusArgument>(fields.value(QStringLiteral("Seat")));
    if (user.currentSignature() != QStringLiteral("(uo)") || seat.currentSignature() != QStringLiteral("(so)")) return {};
    VirtualSessionLogin result;
    QDBusObjectPath userPath, seatPath;
    user.beginStructure(); user >> result.uid >> userPath; user.endStructure();
    seat.beginStructure(); seat >> result.seat >> seatPath; seat.endStructure();
    if (result.seat.isEmpty() && seatPath.path() != QStringLiteral("/")) return {};
    if (!userPath.path().startsWith(QStringLiteral("/org/freedesktop/login1/user/"))) return {};
    const auto account = properties(userPath, QStringLiteral("org.freedesktop.login1.User"));
    if (!account.isValid()) return {};
    const auto userFields = account.value();
    if (userFields.value(QStringLiteral("UID")).metaType().id() != QMetaType::UInt
        || userFields.value(QStringLiteral("UID")).toUInt() != result.uid
        || userFields.value(QStringLiteral("RuntimePath")).metaType().id() != QMetaType::QString) return {};
    result.id = fields.value(QStringLiteral("Id")).toString();
    result.service = fields.value(QStringLiteral("Service")).toString();
    result.type = fields.value(QStringLiteral("Type")).toString();
    result.sessionClass = fields.value(QStringLiteral("Class")).toString();
    result.state = fields.value(QStringLiteral("State")).toString();
    result.tty = fields.value(QStringLiteral("TTY")).toString();
    result.display = fields.value(QStringLiteral("Display")).toString();
    result.scope = fields.value(QStringLiteral("Scope")).toString();
    result.leader = fields.value(QStringLiteral("Leader")).toUInt();
    result.virtualTerminal = fields.value(QStringLiteral("VTNr")).toUInt();
    result.runtime = userFields.value(QStringLiteral("RuntimePath")).toString();
    return result;
}
}
