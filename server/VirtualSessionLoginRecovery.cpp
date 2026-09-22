// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionLoginRecovery.h"
#include "VirtualSessionLoginTag.h"
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDeadlineTimer>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QVariantMap>
#include <climits>

namespace KRdp {
namespace {
const QString ManagerPath = QStringLiteral("/org/freedesktop/login1");
const QString ManagerInterface = QStringLiteral("org.freedesktop.login1.Manager");
const QString SessionInterface = QStringLiteral("org.freedesktop.login1.Session");
const QString SessionPrefix = QStringLiteral("/org/freedesktop/login1/session/");
struct Session {
    QString id, path, seat;
    quint32 uid = 0;
};
bool counterId(const QString &id)
{
    if (!QRegularExpression(QStringLiteral("\\Ac[1-9][0-9]{0,19}\\z")).match(id).hasMatch()) return false;
    bool ok = false; const auto number = id.mid(1).toULongLong(&ok);
    return ok && number && QStringLiteral("c") + QString::number(number) == id;
}
bool identity(const QVariantMap &fields, const Session &listed, const VirtualSessionJournal::Record &record,
    const VirtualSessionJournal::Keeper &keeper)
{
    for (const auto *field : {"Id", "Service", "Type", "Class", "State", "TTY", "Display", "Scope", "Desktop"})
        if (fields.value(QString::fromLatin1(field)).metaType().id() != QMetaType::QString) return false;
    for (const auto *field : {"Leader", "VTNr"})
        if (fields.value(QString::fromLatin1(field)).metaType().id() != QMetaType::UInt) return false;
    for (const auto *field : {"User", "Seat"})
        if (fields.value(QString::fromLatin1(field)).metaType() != QMetaType::fromType<QDBusArgument>()) return false;
    const auto user = qvariant_cast<QDBusArgument>(fields.value(QStringLiteral("User")));
    const auto seat = qvariant_cast<QDBusArgument>(fields.value(QStringLiteral("Seat")));
    if (user.currentSignature() != QStringLiteral("(uo)") || seat.currentSignature() != QStringLiteral("(so)")) return false;
    quint32 uid = 0; QString seatName; QDBusObjectPath userPath, seatPath;
    user.beginStructure(); user >> uid >> userPath; user.endStructure();
    seat.beginStructure(); seat >> seatName >> seatPath; seat.endStructure();
    const auto get = [&](const char *key) { return fields.value(QString::fromLatin1(key)).toString(); };
    const QString state = get("State");
    // No runtime/user lookup: those resources may already be disappearing.
    return counterId(listed.id) && listed.path == SessionPrefix + listed.id
        && get("Id") == listed.id && uid == record.uid && listed.uid == record.uid
        && userPath.path() == QStringLiteral("/org/freedesktop/login1/user/_%1").arg(record.uid)
        && listed.seat.isEmpty() && seatName.isEmpty() && seatPath.path() == QStringLiteral("/")
        && get("Desktop") == virtualLoginTag(record.launch) && get("Service") == QStringLiteral("krdp-virtual-session")
        && get("Type") == QStringLiteral("wayland") && get("Class") == QStringLiteral("background")
        && (state == QStringLiteral("opening") || state == QStringLiteral("online")
            || state == QStringLiteral("active") || state == QStringLiteral("closing"))
        && get("TTY").isEmpty() && get("Display").isEmpty() && fields.value(QStringLiteral("VTNr")).toUInt() == 0
        && fields.value(QStringLiteral("Leader")).toUInt() == quint32(keeper.pid)
        && get("Scope") == QStringLiteral("session-%1.scope").arg(listed.id);
}
}
VirtualSessionLoginRecovery::Result VirtualSessionLoginRecovery::reconcile(const QDBusConnection &bus,
    const VirtualSessionJournal::Record &record, const VirtualSessionJournal::Keeper &keeper, int timeoutMs, bool successfulCloseRecorded)
{
    if (!record.valid() || keeper.pid <= 1 || !keeper.startTicks || keeper.pidInode < 2
        || timeoutMs <= 0 || timeoutMs > 30000 || !bus.isConnected()) return Result::Refused;
    QDeadlineTimer deadline(timeoutMs);
    const auto call = [&](const QString &destination, const QString &path, const QString &interface,
                          const QString &method, const QList<QVariant> &args = {}) {
        if (deadline.hasExpired() || !bus.isConnected()) return QDBusMessage();
        auto message = QDBusMessage::createMethodCall(destination, path, interface, method);
        message.setAutoStartService(false); message.setArguments(args);
        return bus.call(message, QDBus::Block, int(qMin<qint64>(3000, qMax<qint64>(1, deadline.remainingTime()))));
    };
    const auto owner = [&]() -> QString {
        const QDBusReply<QString> reply = call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {QStringLiteral("org.freedesktop.login1")});
        return reply.isValid() && reply.value().startsWith(QLatin1Char(':')) ? reply.value() : QString();
    };
    const auto pinned = owner();
    if (pinned.isEmpty()) return Result::Refused;
    const auto sameOwner = [&] { return owner() == pinned; };
    const auto properties = [&](const Session &session) -> std::optional<QVariantMap> {
        const QDBusReply<QVariantMap> reply = call(pinned, session.path, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("GetAll"), {SessionInterface});
        if (!reply.isValid() || reply.value().size() > 128) return {};
        return reply.value();
    };
    QString terminated;
    while (!deadline.hasExpired()) {
        if (!sameOwner()) return Result::Refused;
        const auto reply = call(pinned, ManagerPath, ManagerInterface, QStringLiteral("ListSessions"));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1
            || reply.arguments()[0].metaType() != QMetaType::fromType<QDBusArgument>()) return Result::Refused;
        const auto array = qvariant_cast<QDBusArgument>(reply.arguments()[0]);
        if (array.currentSignature() != QStringLiteral("a(susso)")) return Result::Refused;
        QList<Session> sessions; QSet<QString> ids, paths;
        array.beginArray();
        while (!array.atEnd()) {
            Session session; QString name; QDBusObjectPath path;
            array.beginStructure(); array >> session.id >> session.uid >> name >> session.seat >> path; array.endStructure();
            session.path = path.path();
            if (sessions.size() >= 512 || session.id.isEmpty() || session.id.size() > 64 || session.path.size() > 256
                || !session.path.startsWith(SessionPrefix) || ids.contains(session.id) || paths.contains(session.path)) return Result::Refused;
            ids.insert(session.id); paths.insert(session.path); sessions.append(session);
        }
        array.endArray();
        std::optional<Session> candidate;
        for (const auto &session : sessions) {
            const auto fields = properties(session);
            // Any missing snapshot leaves enumeration uncertain, including a
            // concurrent unrelated logout. Refuse rather than overlook a tag.
            if (!fields || fields->value(QStringLiteral("Desktop")).metaType().id() != QMetaType::QString) return Result::Refused;
            if (fields->value(QStringLiteral("Desktop")).toString() != virtualLoginTag(record.launch)) continue;
            if (candidate || !identity(*fields, session, record, keeper)) return Result::Refused;
            candidate = session;
        }
        if (!sameOwner()) return Result::Refused;
        if (!candidate) {
            if (!terminated.isEmpty()) return ids.contains(terminated) ? Result::Refused : Result::Removed;
            if (successfulCloseRecorded) return Result::AlreadyClosed;
        } else if (terminated.isEmpty()) {
            // Narrow TOCTOU by revalidating immediately. cIDs are never reused
            // in the supported logind owner's counter lifetime (no wraparound).
            const auto fields = properties(*candidate);
            if (!fields || !identity(*fields, *candidate, record, keeper) || !sameOwner()) return Result::Refused;
            const auto stopped = call(pinned, candidate->path, SessionInterface, QStringLiteral("Terminate"));
            if (stopped.type() != QDBusMessage::ReplyMessage || !stopped.arguments().isEmpty()) return Result::Refused;
            terminated = candidate->id;
        } else if (candidate->id != terminated) {
            return Result::Refused;
        }
        if (!deadline.hasExpired()) QThread::msleep(ulong(qMin<qint64>(50, deadline.remainingTime())));
    }
    return Result::Unresolved;
}
}
