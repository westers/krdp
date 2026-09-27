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

QList<Session> parseList(const QDBusMessage &reply, QString *error)
{
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        if (error) {
            *error = reply.errorMessage().isEmpty() ? QStringLiteral("logind ListSessions returned no session list") : reply.errorMessage();
        }
        return {};
    }
    QList<Session> listed;
    // The reply argument is read-only. Keep the QDBusArgument const so the
    // parser overloads are selected; a mutable `beginArray()` means "start
    // serialising" and aborts against this reply object.
    const QDBusArgument entries = qvariant_cast<QDBusArgument>(reply.arguments().constFirst());
    entries.beginArray();
    while (!entries.atEnd()) {
        Session entry;
        QDBusObjectPath path;
        entries.beginStructure();
        entries >> entry.id >> entry.uid >> entry.user >> entry.seat >> path;
        entries.endStructure();
        entry.objectPath = path.path();
        listed.append(std::move(entry));
    }
    entries.endArray();
    return listed;
}

Session sessionFrom(const Session &listed, const QVariantMap &properties)
{
    Session session = listed;
    session.type = properties.value(QStringLiteral("Type")).toString();
    session.sessionClass = properties.value(QStringLiteral("Class")).toString();
    session.state = properties.value(QStringLiteral("State")).toString();
    session.active = properties.value(QStringLiteral("Active")).toBool();
    session.locked = properties.value(QStringLiteral("LockedHint")).toBool();
    session.leader = properties.value(QStringLiteral("Leader")).toUInt();
    return session;
}

QDBusMessage getAll(const QString &path)
{
    auto message = QDBusMessage::createMethodCall(Login1, path, PropertiesInterface, QStringLiteral("GetAll"));
    message << SessionInterface;
    return message;
}

QDBusMessage listSessionsCall()
{
    return QDBusMessage::createMethodCall(Login1, ManagerPath, ManagerInterface, QStringLiteral("ListSessions"));
}

class SystemBus final : public KRdp::ConsoleSeatWatcher::Bus
{
public:
    void watchManager(KRdp::ConsoleSeatWatcher *watcher) override
    {
        auto bus = QDBusConnection::systemBus();
        bus.connect(Login1, ManagerPath, ManagerInterface, QStringLiteral("SessionNew"), watcher, SLOT(scheduleRefresh()));
        bus.connect(Login1, ManagerPath, ManagerInterface, QStringLiteral("SessionRemoved"), watcher, SLOT(scheduleRefresh()));
        // seat0's ActiveSession flips on every greeter/user/VT switch.
        bus.connect(Login1, QStringLiteral("/org/freedesktop/login1/seat/seat0"), PropertiesInterface, QStringLiteral("PropertiesChanged"), watcher,
                    SLOT(scheduleRefresh()));
    }

    void listSessions(QObject *context, ListDone done) override
    {
        auto *call = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(listSessionsCall()), context);
        QObject::connect(call, &QDBusPendingCallWatcher::finished, context, [done = std::move(done)](QDBusPendingCallWatcher *finished) {
            finished->deleteLater();
            QString error;
            const auto listed = parseList(finished->reply(), &error);
            done(listed, error);
        });
    }

    void sessionProperties(const QString &objectPath, QObject *context, PropertiesDone done) override
    {
        auto *call = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(getAll(objectPath)), context);
        QObject::connect(call, &QDBusPendingCallWatcher::finished, context, [done = std::move(done)](QDBusPendingCallWatcher *finished) {
            finished->deleteLater();
            const QDBusPendingReply<QVariantMap> values = *finished;
            done(values.isError() ? std::nullopt : std::optional<QVariantMap>(values.value()));
        });
    }

    void watchSession(const QString &objectPath, KRdp::ConsoleSeatWatcher *watcher) override
    {
        QDBusConnection::systemBus().connect(Login1, objectPath, PropertiesInterface, QStringLiteral("PropertiesChanged"), watcher, SLOT(scheduleRefresh()));
    }

    void unwatchSession(const QString &objectPath, KRdp::ConsoleSeatWatcher *watcher) override
    {
        QDBusConnection::systemBus().disconnect(Login1, objectPath, PropertiesInterface, QStringLiteral("PropertiesChanged"), watcher, SLOT(scheduleRefresh()));
    }
};
}

QList<Session> readLogindSessions(QString *error)
{
    if (error) {
        error->clear();
    }
    auto bus = QDBusConnection::systemBus();
    const auto listed = parseList(bus.call(listSessionsCall()), error);
    QList<Session> sessions;
    for (const auto &entry : listed) {
        const QDBusMessage reply = bus.call(getAll(entry.objectPath));
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
    : ConsoleSeatWatcher(std::make_unique<SystemBus>(), parent)
{
}

ConsoleSeatWatcher::ConsoleSeatWatcher(std::unique_ptr<Bus> bus, QObject *parent)
    : QObject(parent)
    , m_bus(std::move(bus))
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
    m_bus->watchManager(this);
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

QSet<QString> ConsoleSeatWatcher::watchedSessionPaths() const
{
    return m_watchedPaths;
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
    m_bus->listSessions(this, [this, generation](const QList<Session> &listed, const QString &error) {
        if (generation != m_generation) {
            return; // A newer refresh supersedes this one.
        }
        if (listed.isEmpty() && !error.isEmpty()) {
            qWarning().noquote() << "Unable to read console seat:" << error;
            return;
        }
        if (listed.isEmpty()) {
            publish({});
            return;
        }
        auto pending = std::make_shared<int>(int(listed.size()));
        auto collected = std::make_shared<QList<Session>>();
        for (const auto &entry : listed) {
            watchSession(entry.objectPath);
            m_bus->sessionProperties(entry.objectPath, this, [this, generation, entry, pending, collected](const std::optional<QVariantMap> &values) {
                // A session that vanished between the two calls is simply
                // absent; its SessionRemoved signal triggers another refresh.
                if (values) {
                    collected->append(sessionFrom(entry, *values));
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
    if (path.isEmpty() || m_watchedPaths.contains(path)) {
        return;
    }
    m_watchedPaths.insert(path);
    // Active, State and LockedHint changes arrive here.
    m_bus->watchSession(path, this);
}

void ConsoleSeatWatcher::publish(const QList<Session> &sessions)
{
    auto sorted = sessions;
    std::sort(sorted.begin(), sorted.end(), [](const Session &a, const Session &b) {
        return a.id < b.id;
    });
    // Drop per-session signal subscriptions for sessions logind forgot. Use
    // the object paths logind reported: ids are escaped in paths ("3" is
    // ".../session/_33"), so a path rebuilt from the id would drop every live
    // numeric session's LockedHint/Active subscription (AUD-C-1/C-8).
    QSet<QString> alive;
    for (const auto &session : sorted) {
        alive.insert(session.objectPath);
    }
    for (auto it = m_watchedPaths.begin(); it != m_watchedPaths.end();) {
        if (!alive.contains(*it)) {
            m_bus->unwatchSession(*it, this);
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
