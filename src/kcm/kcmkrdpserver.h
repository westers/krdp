// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "brokerauthenticationsettings.h"
#include "brokerservices.h"
#include "brokerpreferences.h"
#include "brokerhostsettings.h"
#include "brokersettingsapply.h"
#include <KQuickConfigModule>

// Navigation and status plus the standard Apply / Reset / Defaults bar. The
// scoped models keep their own drafts; BrokerSettingsApply saves them together; opening the module never migrates legacy
// settings, opens a wallet, probes encoders or controls a user-manager service.
class KRDPServerConfig : public KQuickConfigModule {
    Q_OBJECT
    Q_PROPERTY(BrokerAuthenticationSettings *brokerAuthentication READ brokerAuthentication CONSTANT)
    Q_PROPERTY(BrokerServices *brokerServices READ brokerServices CONSTANT)
    Q_PROPERTY(BrokerPreferences *brokerPreferences READ brokerPreferences CONSTANT)
    Q_PROPERTY(BrokerHostSettings *consoleHostSettings READ consoleHostSettings CONSTANT)
    Q_PROPERTY(BrokerHostSettings *virtualHostSettings READ virtualHostSettings CONSTANT)
    Q_PROPERTY(BrokerHostSettings *virtualSessionSettings READ virtualSessionSettings CONSTANT)
    Q_PROPERTY(BrokerSettingsApply *settingsApply READ settingsApply CONSTANT)
    Q_PROPERTY(QString hostName READ hostName CONSTANT)
public:
    explicit KRDPServerConfig(QObject *parent, const KPluginMetaData &data);
    BrokerAuthenticationSettings *brokerAuthentication() const { return m_authentication; }
    BrokerServices *brokerServices() const { return m_services; }
    BrokerPreferences *brokerPreferences() const { return m_preferences; }
    BrokerHostSettings *consoleHostSettings() const { return m_console; }
    BrokerHostSettings *virtualHostSettings() const { return m_virtual; }
    BrokerHostSettings *virtualSessionSettings() const { return m_session; }
    BrokerSettingsApply *settingsApply() const { return m_apply; }
    QString hostName() const;
    Q_INVOKABLE void copyAddressToClipboard(const QString &address);
public Q_SLOTS:
    void load() override;
    void save() override;
    void defaults() override;
private:
    BrokerAuthenticationSettings *m_authentication;
    BrokerServices *m_services;
    BrokerPreferences *m_preferences;
    BrokerHostSettings *m_console, *m_virtual, *m_session;
    BrokerSettingsApply *m_apply;
};
