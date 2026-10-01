// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "kcmkrdpserver.h"
#include <KPluginFactory>
#include <QClipboard>
#include <QGuiApplication>
#include <QHostInfo>
#include <QQmlEngine>

K_PLUGIN_CLASS_WITH_JSON(KRDPServerConfig, "kcm_farside.json")

KRDPServerConfig::KRDPServerConfig(QObject *parent, const KPluginMetaData &data)
    : KQuickConfigModule(parent, data)
    , m_authentication(new BrokerAuthenticationSettings(this))
    , m_services(new BrokerServices(this))
    , m_preferences(new BrokerPreferences(this))
    , m_console(new BrokerHostSettings(BrokerHostSettings::Scope::Console, this))
    , m_virtual(new BrokerHostSettings(BrokerHostSettings::Scope::Virtual, this))
    , m_session(new BrokerHostSettings(BrokerHostSettings::Scope::VirtualSession, this))
{
    setButtons(Help);
    const QList<QObject *> models{m_authentication, m_services, m_preferences, m_console, m_virtual, m_session};
    for (QObject *model : models) QQmlEngine::setObjectOwnership(model, QQmlEngine::CppOwnership);
}
void KRDPServerConfig::load()
{
    KQuickConfigModule::load();
    m_services->refresh(false);
}
QString KRDPServerConfig::hostName() const { return QHostInfo::localHostName(); }
void KRDPServerConfig::copyAddressToClipboard(const QString &address)
{
    QGuiApplication::clipboard()->setText(address.trimmed());
}
#include "kcmkrdpserver.moc"
