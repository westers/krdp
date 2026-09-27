// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <optional>

#include "ConsoleSeat.h"

namespace KRdp
{
/**
 * Event-driven, non-blocking view of logind's sessions (AUD-C-8).
 *
 * The console host used to poll every 500 ms with one blocking system-bus
 * call per session property on its main thread, which also carries RDP
 * frames. This watcher subscribes to Manager SessionNew/SessionRemoved, seat0
 * and per-session PropertiesChanged, and refreshes with asynchronous
 * ListSessions + GetAll calls. Session paths logind reports are used as-is
 * (Session::objectPath) and never rebuilt from the id; no QDBusInterface
 * (which introspects synchronously) is created.
 */
class ConsoleSeatWatcher : public QObject
{
    Q_OBJECT

public:
    /**
     * The logind calls the watcher makes. The default talks to the system
     * bus; tests inject a fake so the subscription bookkeeping is covered
     * without a real logind.
     */
    class Bus
    {
    public:
        using ListDone = std::function<void(const QList<ConsoleSeat::Session> &listed, const QString &error)>;
        using PropertiesDone = std::function<void(const std::optional<QVariantMap> &properties)>;
        virtual ~Bus() = default;
        /// Route Manager SessionNew/SessionRemoved and seat0 PropertiesChanged to watcher->scheduleRefresh().
        virtual void watchManager(ConsoleSeatWatcher *watcher) = 0;
        /// Asynchronous ListSessions. Each entry carries id, uid, user, seat and objectPath only.
        virtual void listSessions(QObject *context, ListDone done) = 0;
        /// Asynchronous Properties.GetAll on @p objectPath; nullopt if the session vanished.
        virtual void sessionProperties(const QString &objectPath, QObject *context, PropertiesDone done) = 0;
        /// Route the session object's PropertiesChanged to watcher->scheduleRefresh().
        virtual void watchSession(const QString &objectPath, ConsoleSeatWatcher *watcher) = 0;
        virtual void unwatchSession(const QString &objectPath, ConsoleSeatWatcher *watcher) = 0;
    };

    /// Watches logind on the system bus.
    explicit ConsoleSeatWatcher(QObject *parent = nullptr);
    explicit ConsoleSeatWatcher(std::unique_ptr<Bus> bus, QObject *parent = nullptr);
    ~ConsoleSeatWatcher() override;

    void start();
    QList<ConsoleSeat::Session> sessions() const;
    std::optional<ConsoleSeat::Session> session(const QString &id) const;
    /// Session object paths whose PropertiesChanged currently reaches this watcher.
    QSet<QString> watchedSessionPaths() const;

Q_SIGNALS:
    void sessionsChanged(const QList<KRdp::ConsoleSeat::Session> &sessions);

public Q_SLOTS:
    void scheduleRefresh();

private:
    void refresh();
    void watchSession(const QString &path);
    void publish(const QList<ConsoleSeat::Session> &sessions);

    std::unique_ptr<Bus> m_bus;
    QList<ConsoleSeat::Session> m_sessions;
    QSet<QString> m_watchedPaths;
    QTimer m_debounce;
    QTimer m_resync;
    quint64 m_generation = 0;
    bool m_published = false;
};
}
