// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "kcmkrdpserver.h"
#include <KPluginFactory>
#include <QClipboard>
#include <QGuiApplication>
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
    setButtons(Help | Apply | Default);
    m_apply = new BrokerSettingsApply(m_authentication, m_console, m_virtual, m_session, m_preferences, this);
    QQmlEngine::setObjectOwnership(m_apply, QQmlEngine::CppOwnership);
    // The module's own state follows the aggregated drafts.
    const auto sync = [this] { setNeedsSave(m_apply->needsSave()); setRepresentsDefaults(m_apply->representsDefaults()); };
    connect(m_apply, &BrokerSettingsApply::stateChanged, this, sync);
    sync();
    // A saved host setting stays "restart to use" until its service has actually become active again.
    m_console->followService(m_services, 0);
    m_virtual->followService(m_services, 1);
    const QList<QObject *> models{m_authentication, m_services, m_preferences, m_console, m_virtual, m_session};
    for (QObject *model : models) QQmlEngine::setObjectOwnership(model, QQmlEngine::CppOwnership);
}
void KRDPServerConfig::load()
{
    KQuickConfigModule::load();
    m_services->refresh(false);
    // Opening the panel never prompts: host settings come from the published
    // public snapshot and the user's own preferences from their own file.
    // This is also Reset: every pending draft is dropped and re-read. Account
    // names and aliases (administrator protected) load once, when "Who can
    // sign in" is expanded, and Reset only discards their edits.
    m_apply->reset();
    // System Settings enables Defaults until it is told otherwise and only reacts to a change signal, so a clean
    // module (which already represents defaults, setting the same value emits nothing) must announce it once.
    const bool atDefaults = m_apply->representsDefaults();
    setRepresentsDefaults(!atDefaults);
    setRepresentsDefaults(atDefaults);
}
void KRDPServerConfig::save()
{
    KQuickConfigModule::save();
    m_apply->apply();
}
void KRDPServerConfig::defaults()
{
    KQuickConfigModule::defaults();
    m_apply->useDefaults();
}
void KRDPServerConfig::copyAddressToClipboard(const QString &address)
{
    QGuiApplication::clipboard()->setText(address.trimmed());
}
#include "kcmkrdpserver.moc"
