// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <QDBusConnection>
#include <QObject>

/**
 * OPT-060 S3: org.freedesktop.ScreenSaver.ActiveChanged on the session bus (any path), as a Qt signal.
 * Bus signals queue while the worker's event loop is blocked in a synchronous kscreen-doctor call, so a lock
 * edge during a release is still seen, in order, once the loop runs again (polling would miss it).
 */
class ConsoleLockWatch : public QObject
{
    Q_OBJECT
public:
    explicit ConsoleLockWatch(QObject *parent = nullptr)
        : QObject(parent)
    {
        QDBusConnection::sessionBus().connect(QString(), QString(), QStringLiteral("org.freedesktop.ScreenSaver"),
            QStringLiteral("ActiveChanged"), this, SLOT(onActiveChanged(bool)));
    }
Q_SIGNALS:
    void activeChanged(bool active);
private Q_SLOTS:
    void onActiveChanged(bool active) { Q_EMIT activeChanged(active); }
};
