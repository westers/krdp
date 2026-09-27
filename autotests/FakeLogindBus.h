// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

// In-process stand-in for logind behind ConsoleSeatWatcher::Bus. Replies are
// queued, like real asynchronous D-Bus calls, and PropertiesChanged reaches
// the watcher only on paths it is subscribed to, so a dropped subscription
// shows up as a missed LockedHint/Active change.

#include "ConsoleSeatWatcher.h"

#include <QMetaObject>
#include <QPointer>

#include <memory>

namespace KRdp
{
class FakeLogindBus final : public ConsoleSeatWatcher::Bus
{
public:
    struct State {
        QList<ConsoleSeat::Session> sessions; ///< Full records; objectPath is what ListSessions reports.
        QSet<QString> subscribed;
        QPointer<ConsoleSeatWatcher> watcher;

        ConsoleSeat::Session *find(const QString &id)
        {
            for (auto &session : sessions) {
                if (session.id == id) return &session;
            }
            return nullptr;
        }
        /// logind emits PropertiesChanged on @p objectPath; true if the watcher is subscribed.
        bool propertiesChanged(const QString &objectPath)
        {
            if (!watcher || !subscribed.contains(objectPath)) return false;
            watcher->scheduleRefresh();
            return true;
        }
        /// Manager SessionNew/SessionRemoved.
        void sessionsChanged()
        {
            if (watcher) watcher->scheduleRefresh();
        }
    };

    explicit FakeLogindBus(std::shared_ptr<State> state)
        : m_state(std::move(state))
    {
    }

    void watchManager(ConsoleSeatWatcher *watcher) override
    {
        m_state->watcher = watcher;
    }

    void listSessions(QObject *context, ListDone done) override
    {
        QList<ConsoleSeat::Session> listed;
        for (const auto &session : std::as_const(m_state->sessions)) {
            ConsoleSeat::Session entry;
            entry.id = session.id;
            entry.uid = session.uid;
            entry.user = session.user;
            entry.seat = session.seat;
            entry.objectPath = session.objectPath;
            listed.append(entry);
        }
        QMetaObject::invokeMethod(context, [done = std::move(done), listed] { done(listed, {}); }, Qt::QueuedConnection);
    }

    void sessionProperties(const QString &objectPath, QObject *context, PropertiesDone done) override
    {
        std::optional<QVariantMap> properties;
        for (const auto &session : std::as_const(m_state->sessions)) {
            if (session.objectPath == objectPath) {
                properties = QVariantMap{{QStringLiteral("Type"), session.type},       {QStringLiteral("Class"), session.sessionClass},
                                         {QStringLiteral("State"), session.state},     {QStringLiteral("Active"), session.active},
                                         {QStringLiteral("LockedHint"), session.locked}, {QStringLiteral("Leader"), session.leader}};
            }
        }
        QMetaObject::invokeMethod(context, [done = std::move(done), properties] { done(properties); }, Qt::QueuedConnection);
    }

    void watchSession(const QString &objectPath, ConsoleSeatWatcher *) override
    {
        m_state->subscribed.insert(objectPath);
    }

    void unwatchSession(const QString &objectPath, ConsoleSeatWatcher *) override
    {
        m_state->subscribed.remove(objectPath);
    }

private:
    std::shared_ptr<State> m_state;
};

/// A logind session as the fake reports it; @p objectPath is logind's escaped path.
inline ConsoleSeat::Session logindSession(const QString &id, const QString &objectPath, quint32 uid, const QString &sessionClass, bool locked = false)
{
    ConsoleSeat::Session session;
    session.id = id;
    session.objectPath = objectPath;
    session.uid = uid;
    session.user = QStringLiteral("user%1").arg(uid);
    session.seat = QStringLiteral("seat0");
    session.type = QStringLiteral("wayland");
    session.sessionClass = sessionClass;
    session.state = QStringLiteral("active");
    session.active = true;
    session.locked = locked;
    return session;
}
}
