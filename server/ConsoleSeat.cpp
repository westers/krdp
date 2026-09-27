// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleSeat.h"
#include "ConsoleSeatWatcher.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDebug>
#include <QSet>

#include <algorithm>
#include <memory>

namespace KRdp::ConsoleSeat
{
namespace
{
const QString Login1 = QStringLiteral("org.freedesktop.login1");
const QString ManagerPath = QStringLiteral("/org/freedesktop/login1");
const QString ManagerInterface = QStringLiteral("org.freedesktop.login1.Manager");
const QString SessionInterface = QStringLiteral("org.freedesktop.login1.Session");
const QString PropertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

struct Listed {
    QString id;
    quint32 uid = 0;
    QString user;
    QString seat;
    QDBusObjectPath path;
};

QList<Listed> parseList(const QDBusMessage &reply, QString *error)
{
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        if (error) {
            *error = reply.errorMessage().isEmpty() ? QStringLiteral("logind ListSessions returned no session list") : reply.errorMessage();
        }
        return {};
    }
    QList<Listed> listed;
    // The reply argument is read-only. Keep the QDBusArgument const so the
    // parser overloads are selected; a mutable `beginArray()` means "start
    // serialising" and aborts against this reply object.
    const QDBusArgument entries = qvariant_cast<QDBusArgument>(reply.arguments().constFirst());
    entries.beginArray();
    while (!entries.atEnd()) {
        Listed entry;
        entries.beginStructure();
        entries >> entry.id >> entry.uid >> entry.user >> entry.seat >> entry.path;
        entries.endStructure();
        listed.append(std::move(entry));
    }
    entries.endArray();
    return listed;
}

Session sessionFrom(const Listed &listed, const QVariantMap &properties)
{
    Session session;
    session.id = listed.id;
    session.uid = listed.uid;
    session.user = listed.user;
    session.seat = listed.seat;
    session.type = properties.value(QStringLiteral("Type")).toString();
    session.sessionClass = properties.value(QStringLiteral("Class")).toString();
    session.state = properties.value(QStringLiteral("State")).toString();
    session.active = properties.value(QStringLiteral("Active")).toBool();
    session.locked = properties.value(QStringLiteral("LockedHint")).toBool();
    session.leader = properties.value(QStringLiteral("Leader")).toUInt();
    return session;
}

QDBusMessage getAll(const QDBusObjectPath &path)
{
    auto message = QDBusMessage::createMethodCall(Login1, path.path(), PropertiesInterface, QStringLiteral("GetAll"));
    message << SessionInterface;
    return message;
}
}

QList<Session> readLogindSessions(QString *error)
{
    if (error) {
        error->clear();
    }
    auto bus = QDBusConnection::systemBus();
    const auto listed = parseList(bus.call(QDBusMessage::createMethodCall(Login1, ManagerPath, ManagerInterface, QStringLiteral("ListSessions"))), error);
    QList<Session> sessions;
    for (const auto &entry : listed) {
        const QDBusMessage reply = bus.call(getAll(entry.path));
        const QVariantMap properties = reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()
            ? qdbus_cast<QVariantMap>(reply.arguments().constFirst())
            : QVariantMap{};
        sessions.append(sessionFrom(entry, properties));
    }
    return sessions;
}
}

namespace KRdp
{
using namespace ConsoleSeat;

ConsoleSeatWatcher::ConsoleSeatWatcher(QObject *parent)
    : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(50);
    connect(&m_debounce, &QTimer::timeout, this, &ConsoleSeatWatcher::refresh);
    // A missed signal (bus reconnect) must not leave the seat stale forever.
    // This is a slow asynchronous resync, not the old 500 ms blocking poll.
    m_resync.setInterval(60000);
    connect(&m_resync, &QTimer::timeout, this, &ConsoleSeatWatcher::refresh);
}

ConsoleSeatWatcher::~ConsoleSeatWatcher() = default;

void ConsoleSeatWatcher::start()
{
    auto bus = QDBusConnection::systemBus();
    bus.connect(Login1, ManagerPath, ManagerInterface, QStringLiteral("SessionNew"), this, SLOT(scheduleRefresh()));
    bus.connect(Login1, ManagerPath, ManagerInterface, QStringLiteral("SessionRemoved"), this, SLOT(scheduleRefresh()));
    // seat0's ActiveSession flips on every greeter/user/VT switch.
    bus.connect(Login1, QStringLiteral("/org/freedesktop/login1/seat/seat0"), PropertiesInterface, QStringLiteral("PropertiesChanged"), this,
                SLOT(scheduleRefresh()));
    m_resync.start();
    refresh();
}

QList<Session> ConsoleSeatWatcher::sessions() const
{
    return m_sessions;
}

std::optional<Session> ConsoleSeatWatcher::session(const QString &id) const
{
    return ConsoleSeat::find(m_sessions, id);
}

void ConsoleSeatWatcher::scheduleRefresh()
{
    if (!m_debounce.isActive()) {
        m_debounce.start();
    }
}

void ConsoleSeatWatcher::refresh()
{
    const quint64 generation = ++m_generation;
    auto *call = new QDBusPendingCallWatcher(
        QDBusConnection::systemBus().asyncCall(QDBusMessage::createMethodCall(Login1, ManagerPath, ManagerInterface, QStringLiteral("ListSessions"))), this);
    connect(call, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        if (generation != m_generation) {
            return; // A newer refresh supersedes this one.
        }
        QString error;
        const auto listed = parseList(finished->reply(), &error);
        if (listed.isEmpty() && !error.isEmpty()) {
            qWarning().noquote() << "Unable to read console seat:" << error;
            return;
        }
        auto pending = std::make_shared<int>(int(listed.size()));
        auto collected = std::make_shared<QList<Session>>();
        if (listed.isEmpty()) {
            publish({});
            return;
        }
        for (const auto &entry : listed) {
            watchSession(entry.path.path());
            auto *properties = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(getAll(entry.path)), this);
            connect(properties, &QDBusPendingCallWatcher::finished, this, [this, generation, entry, pending, collected](QDBusPendingCallWatcher *reply) {
                reply->deleteLater();
                const QDBusPendingReply<QVariantMap> values = *reply;
                // A session that vanished between the two calls is simply
                // absent; its SessionRemoved signal triggers another refresh.
                if (!values.isError()) {
                    collected->append(sessionFrom(entry, values.value()));
                }
                if (--*pending == 0 && generation == m_generation) {
                    publish(*collected);
                }
            });
        }
    });
}

void ConsoleSeatWatcher::watchSession(const QString &path)
{
    if (m_watchedPaths.contains(path)) {
        return;
    }
    m_watchedPaths.insert(path);
    // Active, State and LockedHint changes arrive here.
    QDBusConnection::systemBus().connect(Login1, path, PropertiesInterface, QStringLiteral("PropertiesChanged"), this, SLOT(scheduleRefresh()));
}

void ConsoleSeatWatcher::publish(const QList<Session> &sessions)
{
    auto sorted = sessions;
    std::sort(sorted.begin(), sorted.end(), [](const Session &a, const Session &b) {
        return a.id < b.id;
    });
    // Drop per-session signal subscriptions for sessions logind forgot.
    QSet<QString> alive;
    for (const auto &session : sorted) {
        alive.insert(QStringLiteral("/org/freedesktop/login1/session/") + session.id);
    }
    for (auto it = m_watchedPaths.begin(); it != m_watchedPaths.end();) {
        if (!alive.contains(*it) && it->startsWith(QStringLiteral("/org/freedesktop/login1/session/"))) {
            QDBusConnection::systemBus().disconnect(Login1, *it, PropertiesInterface, QStringLiteral("PropertiesChanged"), this, SLOT(scheduleRefresh()));
            it = m_watchedPaths.erase(it);
        } else {
            ++it;
        }
    }
    if (m_published && sorted == m_sessions) {
        return;
    }
    m_published = true;
    m_sessions = sorted;
    Q_EMIT sessionsChanged(m_sessions);
}
}

#include "moc_ConsoleSeatWatcher.cpp"
