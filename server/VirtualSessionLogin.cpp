// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionLogin.h"
#include "VirtualSessionLoginTag.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDeadlineTimer>
#include <QRegularExpression>
#include <QVariantMap>

namespace KRdp {
bool VirtualSessionLogin::matchesLaunch(uid_t owner, pid_t expectedLeader, const QString &pamId, const QString &pamRuntime, const QString &launch) const
{
    const auto tag = virtualLoginTag(launch);
    return !tag.isEmpty() && desktop == tag && matches(owner, expectedLeader, pamId, pamRuntime);
}
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
    return read(QDBusConnection::systemBus(), leader);
}

std::optional<VirtualSessionLogin> VirtualSessionLogin::read(const QDBusConnection &bus, pid_t leader, int timeoutMs)
{
    return readWithCalls(leader, timeoutMs,
        [&](const QDBusMessage &message, int remaining) { return bus.call(message, QDBus::Block, remaining); },
        [&] { return bus.isConnected(); });
}

std::optional<VirtualSessionLogin> VirtualSessionLogin::readWithCalls(pid_t leader, int timeoutMs, const Call &invoke,
                                                                   const std::function<bool()> &connected)
{
    // This is a bounded prerequisite, never an arbitrarily long bus operation.
    if (leader <= 1 || timeoutMs <= 0 || timeoutMs > 3000 || !invoke || !connected || !connected()) return {};
    const QDeadlineTimer deadline(timeoutMs, Qt::PreciseTimer);
    const QString daemon = QStringLiteral("org.freedesktop.DBus");
    const QString loginName = QStringLiteral("org.freedesktop.login1");
    const auto call = [&](const QString &destination, const QString &path, const QString &interface,
                          const QString &method, const QList<QVariant> &args) {
        if (!connected() || deadline.hasExpired()) return QDBusMessage();
        auto message = QDBusMessage::createMethodCall(destination, path, interface, method);
        message.setAutoStartService(false);
        message.setArguments(args);
        const auto remaining = deadline.remainingTime();
        if (remaining <= 0) return QDBusMessage();
        const auto reply = invoke(message, int(remaining));
        // Qt 6.10 service() deliberately returns empty for replies. Do not
        // mistake it for a sender check: rely on the bus call's correlation
        // and pinned destination, with generation checks around object reads.
        if (!connected() || deadline.hasExpired() || reply.type() != QDBusMessage::ReplyMessage) return QDBusMessage();
        return reply;
    };
    const auto busString = [&](const QString &method, const QList<QVariant> &args = {}) {
        const auto reply = call(daemon, QStringLiteral("/org/freedesktop/DBus"), daemon, method, args);
        if (reply.signature() != QStringLiteral("s") || reply.arguments().size() != 1
            || reply.arguments().first().metaType().id() != QMetaType::QString) return QString();
        return reply.arguments().first().toString();
    };
    const QString busId = busString(QStringLiteral("GetId"));
    const QString owner = busString(QStringLiteral("GetNameOwner"), {loginName});
    if (!QRegularExpression(QStringLiteral("\\A[0-9a-f]{32}\\z")).match(busId).hasMatch()
        || !QRegularExpression(QStringLiteral("\\A:[A-Za-z0-9_-]+(?:\\.[A-Za-z0-9_-]+)+\\z")).match(owner).hasMatch()) return {};
    // Endpoint checks deliberately do not claim to detect release/reacquire ABA.
    const auto sameEndpoint = [&] {
        return busString(QStringLiteral("GetId")) == busId
            && busString(QStringLiteral("GetNameOwner"), {loginName}) == owner
            && connected() && !deadline.hasExpired();
    };
    if (!sameEndpoint()) return {};
    const auto loginCall = [&](const QString &path, const QString &interface, const QString &method, const QList<QVariant> &args) {
        const auto reply = call(owner, path, interface, method, args);
        return sameEndpoint() ? reply : QDBusMessage();
    };
    const auto pathReply = loginCall(QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("GetSessionByPID"), {quint32(leader)});
    if (pathReply.signature() != QStringLiteral("o") || pathReply.arguments().size() != 1) return {};
    const QDBusReply<QDBusObjectPath> sessionPath = pathReply;
    if (!sessionPath.isValid() || !sessionPath.value().path().startsWith(QStringLiteral("/org/freedesktop/login1/session/"))) return {};
    const auto properties = [&](const QDBusObjectPath &path, const QString &interface) {
        const auto reply = loginCall(path.path(), QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"), {interface});
        return QDBusReply<QVariantMap>(reply.signature() == QStringLiteral("a{sv}") && reply.arguments().size() == 1 ? reply : QDBusMessage());
    };
    const auto session = properties(sessionPath.value(), QStringLiteral("org.freedesktop.login1.Session"));
    if (!session.isValid()) return {};
    const auto fields = session.value();
    // Missing/incorrectly typed fields must not turn into empty seat/zero VT.
    for (const auto *field : {"Id", "Service", "Type", "Class", "State", "TTY", "Display", "Scope", "Desktop"})
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
    if (result.uid == quint32(-1) || userPath.path() != QStringLiteral("/org/freedesktop/login1/user/_%1").arg(result.uid)) return {};
    // systemd bus-label encoding: escape underscore/non-alphanumeric bytes and
    // leading digits. A prefix-only path check does not bind a returned object.
    const auto label = [](const QString &value) {
        QString encoded;
        const auto bytes = value.toUtf8();
        for (int i = 0; i < bytes.size(); ++i) {
            const auto c = static_cast<unsigned char>(bytes[i]);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i > 0 && c >= '0' && c <= '9'))
                encoded += QLatin1Char(c);
            else encoded += QStringLiteral("_%1").arg(c, 2, 16, QLatin1Char('0'));
        }
        return encoded;
    };
    const auto id = fields.value(QStringLiteral("Id")).toString();
    if (!QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z")).match(id).hasMatch()
        || sessionPath.value().path() != QStringLiteral("/org/freedesktop/login1/session/") + label(id)
        || fields.value(QStringLiteral("Leader")).toUInt() != quint32(leader)
        || fields.value(QStringLiteral("Scope")).toString() != QStringLiteral("session-%1.scope").arg(id)) return {};
    if (!result.seat.isEmpty() && (!QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z")).match(result.seat).hasMatch()
        || seatPath.path() != QStringLiteral("/org/freedesktop/login1/seat/") + label(result.seat))) return {};
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
    result.desktop = fields.value(QStringLiteral("Desktop")).toString();
    result.leader = fields.value(QStringLiteral("Leader")).toUInt();
    result.virtualTerminal = fields.value(QStringLiteral("VTNr")).toUInt();
    result.runtime = userFields.value(QStringLiteral("RuntimePath")).toString();
    if (result.runtime != QStringLiteral("/run/user/%1").arg(result.uid)) return {};
    result.busId = busId;
    result.uniqueOwner = owner;
    if (!connected() || deadline.hasExpired()) return {};
    return result;
}
}
