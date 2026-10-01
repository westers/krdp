// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "brokerauthenticationsettings.h"
#include "brokerservices.h"
#include "brokerpreferences.h"
#include "brokerhostsettings.h"
#include <KQuickConfigModule>

// Navigation and read-only status only. Each scoped model owns its explicit
// save/default/restart transaction; opening the module never migrates legacy
// settings, opens a wallet, probes encoders or controls a user-manager service.
class KRDPServerConfig : public KQuickConfigModule {
    Q_OBJECT
    Q_PROPERTY(BrokerAuthenticationSettings *brokerAuthentication READ brokerAuthentication CONSTANT)
    Q_PROPERTY(BrokerServices *brokerServices READ brokerServices CONSTANT)
    Q_PROPERTY(BrokerPreferences *brokerPreferences READ brokerPreferences CONSTANT)
    Q_PROPERTY(BrokerHostSettings *consoleHostSettings READ consoleHostSettings CONSTANT)
    Q_PROPERTY(BrokerHostSettings *virtualHostSettings READ virtualHostSettings CONSTANT)
    Q_PROPERTY(BrokerHostSettings *virtualSessionSettings READ virtualSessionSettings CONSTANT)
    Q_PROPERTY(QString hostName READ hostName CONSTANT)
public:
    explicit KRDPServerConfig(QObject *parent, const KPluginMetaData &data);
    BrokerAuthenticationSettings *brokerAuthentication() const { return m_authentication; }
    BrokerServices *brokerServices() const { return m_services; }
    BrokerPreferences *brokerPreferences() const { return m_preferences; }
    BrokerHostSettings *consoleHostSettings() const { return m_console; }
    BrokerHostSettings *virtualHostSettings() const { return m_virtual; }
    BrokerHostSettings *virtualSessionSettings() const { return m_session; }
    QString hostName() const;
    Q_INVOKABLE void copyAddressToClipboard(const QString &address);
public Q_SLOTS:
    void load() override;
private:
    BrokerAuthenticationSettings *m_authentication;
    BrokerServices *m_services;
    BrokerPreferences *m_preferences;
    BrokerHostSettings *m_console, *m_virtual, *m_session;
};
