// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QSet>
#include <QTimer>

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
 * ListSessions + GetAll calls. Session paths logind reports are used as-is;
 * no QDBusInterface (which introspects synchronously) is created.
 */
class ConsoleSeatWatcher : public QObject
{
    Q_OBJECT

public:
    explicit ConsoleSeatWatcher(QObject *parent = nullptr);
    ~ConsoleSeatWatcher() override;

    void start();
    QList<ConsoleSeat::Session> sessions() const;
    std::optional<ConsoleSeat::Session> session(const QString &id) const;

Q_SIGNALS:
    void sessionsChanged(const QList<KRdp::ConsoleSeat::Session> &sessions);

public Q_SLOTS:
    void scheduleRefresh();

private:
    void refresh();
    void watchSession(const QString &path);
    void publish(const QList<ConsoleSeat::Session> &sessions);

    QList<ConsoleSeat::Session> m_sessions;
    QSet<QString> m_watchedPaths;
    QTimer m_debounce;
    QTimer m_resync;
    quint64 m_generation = 0;
    bool m_published = false;
};
}
