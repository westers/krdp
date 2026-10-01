// SPDX-FileCopyrightText: 2026 KDE Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "DisplayWakeGuard.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QPointer>
#include <utility>

using namespace Qt::StringLiterals;

namespace
{
QDBusMessage powerManagementCall(const QString &method)
{
    return QDBusMessage::createMethodCall(u"org.kde.Solid.PowerManagement"_s, u"/org/kde/Solid/PowerManagement"_s, u"org.kde.Solid.PowerManagement"_s, method);
}

QDBusMessage screenSaverCall(const QString &method)
{
    return QDBusMessage::createMethodCall(u"org.freedesktop.ScreenSaver"_s, u"/ScreenSaver"_s, u"org.freedesktop.ScreenSaver"_s, method);
}

// A delayed Inhibit reply can arrive after the guard/worker was destroyed.
// Keep just the reply watcher alive and release on the same desktop bus and
// unique service owner. Never send an old cookie to a replacement service.
void releaseCookie(const QDBusConnection &connection, uint cookie, const QString &owner)
{
    auto message = QDBusMessage::createMethodCall(owner.isEmpty() ? u"org.freedesktop.ScreenSaver"_s : owner,
        u"/ScreenSaver"_s, u"org.freedesktop.ScreenSaver"_s, u"UnInhibit"_s);
    message << cookie;
    auto *watcher = new QDBusPendingCallWatcher(connection.asyncCall(message));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [watcher] {
        const QDBusPendingReply<> reply = *watcher;
        if (reply.isError()) qWarning() << "Failed to release desktop inhibition:" << reply.error().message();
        watcher->deleteLater();
    });
}
}

DisplayWakeGuard::DisplayWakeGuard(QObject *parent)
    : DisplayWakeGuard(QDBusConnection::sessionBus(), parent)
{
}

DisplayWakeGuard::DisplayWakeGuard(const QDBusConnection &connection, QObject *parent)
    : QObject(parent)
    , m_connection(connection)
{
}

DisplayWakeGuard::~DisplayWakeGuard()
{
    // Never leak the inhibition on server shutdown.
    if (m_inhibitCookie.has_value()) {
        uninhibit();
    }
}

void DisplayWakeGuard::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;

    if (!enabled) ++m_wakeSerial; // Invalidate an outstanding wake/fallback.

    if (!m_enabled && m_inhibitCookie.has_value()) {
        uninhibit();
    } else if (m_enabled && m_activeSessions > 0) {
        wakeDisplay();
        inhibit();
    }
}

void DisplayWakeGuard::acquire()
{
    ++m_activeSessions;
    if (m_activeSessions != 1 || !m_enabled) {
        return;
    }

    wakeDisplay();
    inhibit();
}

void DisplayWakeGuard::release()
{
    if (m_activeSessions == 0) {
        return;
    }
    --m_activeSessions;
    if (m_activeSessions != 0) {
        return;
    }

    ++m_wakeSerial;

    // If the Inhibit reply is still in flight, its handler releases the cookie.
    if (m_inhibitCookie.has_value()) {
        uninhibit();
    }
}

void DisplayWakeGuard::wakeNow()
{
    wakeDisplay();
}

void DisplayWakeGuard::wakeDisplay()
{
    const auto serial = ++m_wakeSerial;
    auto watcher = new QDBusPendingCallWatcher(m_connection.asyncCall(powerManagementCall(u"wakeup"_s)), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, serial]() {
        watcher->deleteLater();
        if (serial != m_wakeSerial) return;
        QDBusPendingReply<> reply = *watcher;
        if (reply.isError()) {
            qWarning() << "PowerDevil wakeup failed, falling back to ScreenSaver.SimulateUserActivity:" << reply.error().message();
            simulateUserActivity();
            return;
        }
        qInfo() << "Woke the display for the remote desktop session";
        Q_EMIT displayWakeRequested(true);
    });
}

void DisplayWakeGuard::simulateUserActivity()
{
    const auto serial = m_wakeSerial;
    auto watcher = new QDBusPendingCallWatcher(m_connection.asyncCall(screenSaverCall(u"SimulateUserActivity"_s)), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, serial]() {
        watcher->deleteLater();
        if (serial != m_wakeSerial) return;
        QDBusPendingReply<> reply = *watcher;
        if (reply.isError()) {
            qWarning() << "ScreenSaver.SimulateUserActivity failed:" << reply.error().message();
            Q_EMIT displayWakeRequested(false);
            return;
        }
        qInfo() << "Simulated user activity to wake the display for the remote desktop session";
        Q_EMIT displayWakeRequested(true);
    });
}

void DisplayWakeGuard::inhibit()
{
    if (m_inhibitPending || m_inhibitCookie.has_value()) {
        return;
    }
    m_inhibitPending = true;

    auto message = screenSaverCall(u"Inhibit"_s);
    message << u"Farside"_s << u"Remote desktop session active"_s;
    // Intentionally not parented to the guard: its reply must retire even if
    // teardown happens before ScreenSaver answers. The watcher owns its slot.
    const QPointer<DisplayWakeGuard> guard(this);
    const auto connection = m_connection;
    auto watcher = new QDBusPendingCallWatcher(connection.asyncCall(message));
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [guard, connection, watcher]() {
        watcher->deleteLater();
        if (guard) guard->m_inhibitPending = false;

        QDBusPendingReply<uint> reply = *watcher;
        if (reply.isError()) {
            qWarning() << "Failed to inhibit screen power management:" << reply.error().message();
            return;
        }
        const auto owner = reply.reply().service();
        if (!guard) {
            releaseCookie(connection, reply.value(), owner);
            return;
        }
        guard->m_inhibitCookie = reply.value();
        guard->m_inhibitOwner = owner;
        qInfo() << "Inhibited screen power management for the remote desktop session, cookie" << *guard->m_inhibitCookie;

        // The last session may already have ended while the call was in flight.
        if (guard->m_activeSessions == 0 || !guard->m_enabled) {
            guard->uninhibit();
        }
    });
}

void DisplayWakeGuard::uninhibit()
{
    const auto cookie = *m_inhibitCookie;
    m_inhibitCookie.reset();

    releaseCookie(m_connection, cookie, std::exchange(m_inhibitOwner, {}));
    qInfo() << "Released screen power management inhibition, cookie" << cookie;
}
