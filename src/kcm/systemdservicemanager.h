// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "coexistence.h"

#include <QDBusConnection>
#include <QObject>

// Coexistence::ServiceManager over the user systemd manager's D-Bus API.
class SystemdServiceManager : public QObject, public Coexistence::ServiceManager
{
    Q_OBJECT
public:
    explicit SystemdServiceManager(const QDBusConnection &bus, QObject *parent = nullptr);

    void queryUnit(const QString &unit, StateDone done) override;
    void stopUnit(const QString &unit, Done done) override;
    void disableUnitFile(const QString &unit, Done done) override;
    bool systemUnitInstalled(const QString &unit) const override;

private:
    QDBusConnection m_bus;
};
