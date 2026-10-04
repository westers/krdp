// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokersettingsapply.h"
#include "brokerauthenticationsettings.h"
#include "brokerhostsettings.h"
#include "brokerpreferences.h"
#include <KLocalizedString>

using namespace Qt::StringLiterals;

BrokerSettingsApply::BrokerSettingsApply(BrokerAuthenticationSettings *authentication, BrokerHostSettings *console, BrokerHostSettings *virtualHost,
    BrokerHostSettings *session, BrokerPreferences *preferences, QObject *parent)
    : QObject(parent), m_authentication(authentication), m_console(console), m_virtual(virtualHost), m_session(session), m_preferences(preferences)
{
    for (QObject *model : QList<QObject *>{authentication, console, virtualHost, session, preferences})
        connect(model, SIGNAL(changed()), this, SLOT(advance()));
}
bool BrokerSettingsApply::needsSave() const
{
    return m_authentication->modified() || m_console->modified() || m_virtual->modified() || m_session->modified() || m_preferences->modified();
}
bool BrokerSettingsApply::representsDefaults() const
{
    return m_console->representsDefaults() && m_virtual->representsDefaults() && m_session->representsDefaults() && m_preferences->representsDefaults();
}
bool BrokerSettingsApply::anyBusy() const
{
    return m_authentication->busy() || m_console->busy() || m_virtual->busy() || m_session->busy();
}
void BrokerSettingsApply::apply()
{
    if (applying() || anyBusy()) return;
    m_failures.clear(); m_attempted.clear(); m_helpers = 0;
    m_starting = true;
    m_phase = Phase::Hosts;
    // No authorization: the user's own preferences.
    if (m_preferences->modified()) { m_attempted.append(m_preferences); m_preferences->save(); }
    const QList<BrokerHostSettings *> hosts{m_console, m_virtual, m_session};
    QList<BrokerHostSettings *> ready;
    for (auto *host : hosts) {
        if (!host->modified()) continue;
        m_attempted.append(host);
        if (host->canSave()) ready.append(host);
    }
    // One helper process, one authorization, for every dirty host scope.
    if (BrokerHostSettings::saveTogether(ready) > 0) ++m_helpers;
    m_starting = false;
    Q_EMIT stateChanged();
    advance();
}
void BrokerSettingsApply::advance()
{
    if (m_starting) return;
    Q_EMIT stateChanged();
    if (m_phase == Phase::Hosts && !m_console->busy() && !m_virtual->busy() && !m_session->busy()) {
        m_phase = Phase::Access;
        if (m_authentication->modified()) {
            m_attempted.append(m_authentication);
            m_starting = true;
            if (m_authentication->save()) ++m_helpers;
            m_starting = false;
        }
    }
    if (m_phase == Phase::Access && !m_authentication->busy()) finish();
}
void BrokerSettingsApply::finish()
{
    const auto label = [this](QObject *model) {
        if (model == m_console) return i18nc("@title:group", "Console");
        if (model == m_virtual) return i18nc("@title:group", "Virtual");
        if (model == m_session) return i18nc("@title:group", "New desktop hardware");
        if (model == m_authentication) return i18nc("@title:group", "Who can connect");
        return i18nc("@title:group", "My preferences");
    };
    const auto problem = [this](QObject *model) {
        if (model == m_authentication) return m_authentication->error();
        if (model == m_preferences) return m_preferences->error();
        return static_cast<BrokerHostSettings *>(model)->error();
    };
    const auto stillPending = [this](QObject *model) {
        if (model == m_authentication) return m_authentication->modified();
        if (model == m_preferences) return m_preferences->modified();
        return static_cast<BrokerHostSettings *>(model)->modified();
    };
    m_phase = Phase::Idle;
    // A scope is reported only if it did not save: its draft is still pending.
    for (QObject *model : std::as_const(m_attempted))
        if (stillPending(model)) {
            const auto text = problem(model);
            m_failures.append(i18nc("@info %1 settings area, %2 reason", "%1: %2", label(model),
                text.isEmpty() ? i18nc("@info", "not saved") : text));
        }
    m_attempted.clear();
    Q_EMIT stateChanged();
}
void BrokerSettingsApply::reset()
{
    if (applying() || anyBusy()) return;
    m_failures.clear();
    for (auto *host : {m_console, m_virtual, m_session}) {
        host->cancelCertificateEdit();
        host->discard();
        host->refresh();
    }
    m_authentication->discard();
    m_preferences->discard();
    m_preferences->reload();
    Q_EMIT draftsReplaced();
    Q_EMIT stateChanged();
}
void BrokerSettingsApply::useDefaults()
{
    if (applying() || anyBusy()) return;
    m_failures.clear();
    // Certificates are kept by the host models; the access policy has no default.
    for (auto *host : {m_console, m_virtual, m_session}) {
        host->cancelCertificateEdit();
        host->defaults();
    }
    m_preferences->defaults();
    Q_EMIT draftsReplaced();
    Q_EMIT stateChanged();
}
