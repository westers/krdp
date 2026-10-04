// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QObject>
#include <QStringList>

class BrokerAuthenticationSettings;
class BrokerHostSettings;
class BrokerPreferences;

// The module-wide Apply / Reset / Defaults transaction over the five scoped
// drafts. Apply saves every dirty scope, never stopping at a failed one:
//  1. my preferences (own file, no authorization);
//  2. all dirty host scopes (Console, Virtual, new-desktop hardware) through ONE
//     helper process, i.e. one authorization;
//  3. the access policy through its own helper once the host batch is done.
// Step 3 is a second helper process by design (different helper and polkit
// action); polkit's auth_admin_keep makes its authorization silent inside the
// retention window, and it is never started while step 2 is still prompting.
class BrokerSettingsApply : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool needsSave READ needsSave NOTIFY stateChanged)
    Q_PROPERTY(bool representsDefaults READ representsDefaults NOTIFY stateChanged)
    Q_PROPERTY(bool applying READ applying NOTIFY stateChanged)
    Q_PROPERTY(QStringList failures READ failures NOTIFY stateChanged)
    Q_PROPERTY(int lastHelperInvocations READ lastHelperInvocations NOTIFY stateChanged)
public:
    BrokerSettingsApply(BrokerAuthenticationSettings *authentication, BrokerHostSettings *console, BrokerHostSettings *virtualHost,
        BrokerHostSettings *session, BrokerPreferences *preferences, QObject *parent = nullptr);
    bool needsSave() const;
    bool representsDefaults() const;
    bool applying() const { return m_phase != Phase::Idle; }
    QStringList failures() const { return m_failures; }
    // Administrator helper processes started by the last apply() (hosts count 1 when batched).
    int lastHelperInvocations() const { return m_helpers; }
    Q_INVOKABLE void apply();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void useDefaults();
Q_SIGNALS:
    void stateChanged();
    // Reset or Defaults replaced the drafts: open local editors must close.
    void draftsReplaced();
private Q_SLOTS:
    void advance();
private:
    enum class Phase { Idle, Hosts, Access };
    void finish();
    bool anyBusy() const;
    BrokerAuthenticationSettings *m_authentication;
    BrokerHostSettings *m_console, *m_virtual, *m_session;
    BrokerPreferences *m_preferences;
    Phase m_phase = Phase::Idle;
    bool m_starting = false;
    int m_helpers = 0;
    QStringList m_failures;
    QList<QObject *> m_attempted;
};
