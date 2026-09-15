// SPDX-FileCopyrightText: 2026 KDE Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "DisplayWakeGuard.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>

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
}

DisplayWakeGuard::DisplayWakeGuard(QObject *parent)
    : QObject(parent)
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

    // If the Inhibit reply is still in flight, its handler releases the cookie.
    if (m_inhibitCookie.has_value()) {
        uninhibit();
    }
}

void DisplayWakeGuard::wakeDisplay()
{
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(powerManagementCall(u"wakeup"_s)), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        watcher->deleteLater();
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
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(screenSaverCall(u"SimulateUserActivity"_s)), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        watcher->deleteLater();
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
    message << u"krdpserver"_s << u"Remote desktop session active"_s;
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        watcher->deleteLater();
        m_inhibitPending = false;

        QDBusPendingReply<uint> reply = *watcher;
        if (reply.isError()) {
            qWarning() << "Failed to inhibit screen power management:" << reply.error().message();
            return;
        }
        m_inhibitCookie = reply.value();
        qInfo() << "Inhibited screen power management for the remote desktop session, cookie" << *m_inhibitCookie;

        // The last session may already have ended while the call was in flight.
        if (m_activeSessions == 0 || !m_enabled) {
            uninhibit();
        }
    });
}

void DisplayWakeGuard::uninhibit()
{
    const auto cookie = *m_inhibitCookie;
    m_inhibitCookie.reset();

    auto message = screenSaverCall(u"UnInhibit"_s);
    message << cookie;
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher]() {
        watcher->deleteLater();
        QDBusPendingReply<> reply = *watcher;
        if (reply.isError()) {
            qWarning() << "Failed to release screen power management inhibition:" << reply.error().message();
        }
    });
    qInfo() << "Released screen power management inhibition, cookie" << cookie;
}
