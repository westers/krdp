// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "coexistence.h"
#include "farsideidentity.h"
#include "krdpserversettings.h"
#include "serviceinfo.h"
#include "useraccounts.h"
#include "usersmodel.h"
#include <KQuickManagedConfigModule>

class QAbstractItemModel;
class QFileSystemWatcher;
class QTimer;

class KRDPServerConfig : public KQuickManagedConfigModule
{
    Q_OBJECT
public:
    explicit KRDPServerConfig(QObject *parent, const KPluginMetaData &data);
    ~KRDPServerConfig() override;

    Q_PROPERTY(SystemdService::Status serverStatus READ serverStatus NOTIFY serverStatusChanged)
    Q_PROPERTY(bool serverRunning READ isServerRunning NOTIFY serverStatusChanged)
    // AUD-K9: systemd is starting or stopping the unit.
    Q_PROPERTY(bool serverBusy READ isServerBusy NOTIFY serverStatusChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // AUD-K6: the saved configuration differs from what the running server
    // loaded (it records that in $XDG_RUNTIME_DIR), independent of this page's
    // lifetime.
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY restartRequiredChanged)
    Q_PROPERTY(QStringList restartReasons READ restartReasons NOTIFY restartRequiredChanged)
    // AUD-K5: options in the unit's ExecStart that override settings here.
    Q_PROPERTY(QStringList commandLineOverrides READ commandLineOverrides NOTIFY serviceCommandLineChanged)
    Q_PROPERTY(QStringList overriddenSettings READ overriddenSettings NOTIFY serviceCommandLineChanged)
    Q_PROPERTY(QStringList serviceDropIns READ serviceDropIns NOTIFY serviceCommandLineChanged)
    // AUD-K11: mirrors systemd's unit-file state; applied on Apply.
    Q_PROPERTY(bool autostart READ autostart WRITE setAutostart NOTIFY autostartChanged)
    Q_PROPERTY(bool keychainBusy READ keychainBusy NOTIFY keychainBusyChanged)
    // AUD-K3: the certificate the server uses (or will create).
    Q_PROPERTY(QString certificateState READ certificateState NOTIFY certificateInfoChanged)
    Q_PROPERTY(QString certificateFingerprint READ certificateFingerprint NOTIFY certificateInfoChanged)
    Q_PROPERTY(QString certificateExpiry READ certificateExpiry NOTIFY certificateInfoChanged)
    Q_PROPERTY(QString certificateAlgorithm READ certificateAlgorithm NOTIFY certificateInfoChanged)
    Q_PROPERTY(QString certificatePath READ certificatePath NOTIFY certificateInfoChanged)

    Q_PROPERTY(QString hostName READ hostName CONSTANT)
    Q_PROPERTY(bool managementAvailable READ managementAvailable CONSTANT)
    Q_PROPERTY(bool plasmaBackendAvailable READ plasmaBackendAvailable CONSTANT)

    Q_PROPERTY(QAbstractItemModel *users READ usersModel CONSTANT)
    // The account the system-password sign-in belongs to.
    Q_PROPERTY(QString systemUserName READ systemUserName CONSTANT)
    // KDE's own remote desktop next to Farside (REBRAND-PLAN.md §3/§4).
    Q_PROPERTY(Coexistence::Controller *coexistence READ coexistence CONSTANT)

    Q_INVOKABLE QString toLocalFile(const QUrl &url);

    Q_INVOKABLE void modifyUser(const QString &oldUsername, const QString &newUsername, const QString &newPassword);
    Q_INVOKABLE void addUser(const QString &username, const QString &password);
    Q_INVOKABLE void deleteUser(const QString &username);
    Q_INVOKABLE bool userExists(const QString &username);
    Q_INVOKABLE void readPasswordFromWallet(const QString &user);

    Q_INVOKABLE bool isH264Supported();
    Q_INVOKABLE QStringList listenAddressList();
    Q_INVOKABLE QStringList interfaceAddresses() const;
    Q_INVOKABLE QStringList availableMonitorIds() const;
    Q_INVOKABLE void toggleServer(const bool enabled);
    Q_INVOKABLE void restartServer();
    Q_INVOKABLE void copyAddressToClipboard(const QString &address);
    // Monitors as [{index, name, text}], in the server's index order.
    Q_INVOKABLE QVariantList monitors() const;
    // Installed VAAPI drivers the server can be told to use.
    Q_INVOKABLE QStringList vaapiDrivers() const;
    // v4l2loopback devices as [{path, name}].
    Q_INVOKABLE QVariantList loopbackCameras() const;
    // Saves ListenPort at once (other unapplied edits stay unapplied) and
    // starts or restarts the server on it.
    Q_INVOKABLE void applyListenPort(int port);
    Q_INVOKABLE KRDPServerSettings *settings() const
    {
        return m_serverSettings;
    };

    // Validation shared with krdpserver (server/ServerSettingsPolicy.h and
    // the server's own parsers), so the page never saves what the server
    // would refuse or silently replace.
    Q_INVOKABLE bool isValidPort(const QString &text) const;
    Q_INVOKABLE bool isValidFallbackSize(const QString &text) const;
    Q_INVOKABLE bool isValidChromaPolicy(int motionGapMs, int restMs, int maxGapMs) const;
    Q_INVOKABLE bool monitorModeNeedsPlasma(const QString &mode) const;
    Q_INVOKABLE QString settingLabel(const QString &key) const;

    Q_INVOKABLE void updateServerStatus();
    SystemdService::Status serverStatus() const;
    Q_INVOKABLE [[nodiscard]] bool isServerRunning() const;
    bool isServerBusy() const;

    QString errorMessage() const;
    bool restartRequired() const;
    QStringList restartReasons() const;
    QStringList commandLineOverrides() const;
    QStringList overriddenSettings() const;
    QStringList serviceDropIns() const;
    bool autostart() const;
    void setAutostart(bool autostart);
    bool keychainBusy() const;
    QString certificateState() const;
    QString certificateFingerprint() const;
    QString certificateExpiry() const;
    QString certificateAlgorithm() const;
    QString certificatePath() const;

    QString hostName() const;
    QString systemUserName() const;
    Coexistence::Controller *coexistence() const
    {
        return m_coexistence;
    }
    bool managementAvailable() const;
    bool plasmaBackendAvailable() const;
    QAbstractItemModel *usersModel() const
    {
        return m_usersModel;
    };

public Q_SLOTS:
    void load() override;
    void save() override;
    void defaults() override;

protected:
    bool isSaveNeeded() const override;
    bool isDefaults() const override;

Q_SIGNALS:
    void krdpServerSettingsChanged();
    void passwordLoaded(const QString &user, const QString &password);
    void keychainError(const QString &errorText);
    void serverStatusChanged();
    void errorMessageChanged();
    void restartRequiredChanged();
    void serviceCommandLineChanged();
    void autostartChanged();
    void keychainBusyChanged();
    void certificateInfoChanged();

private:
    void setServerStatus(SystemdService::Status status);
    void setErrorMessage(const QString &errorMessage);
    void setPortalPreauthorized(bool preauthorized);
    void readAutostart();
    void applyAutostart();
    void readServiceCommandLine();
    void refreshRestartState();
    void refreshCertificateInfo();
    void recordCredentialsChange();
    void fetchJournal(const QString &invocationId);
    Q_SLOT void servicePropertiesChanged();

    KRDPServerSettings *m_serverSettings;
    Farside::Identity m_identity;
    QString m_unitPath;
    Coexistence::Controller *m_coexistence;
    QTimer *m_coexistenceTimer;
    UsersModel *m_usersModel;
    UserAccounts *m_userAccounts;
    QFileSystemWatcher *m_runtimeWatcher;
    bool m_isH264Supported{false};
    SystemdService::Status m_currentServerStatus{SystemdService::Unknown};
    QString m_lastErrorMessage;
    QStringList m_restartReasons;
    QStringList m_commandLineOverrides;
    QStringList m_overriddenSettings;
    QStringList m_serviceDropIns;
    bool m_autostart{false};
    bool m_autostartActual{false};
    bool m_keychainBusy{false};
    QString m_certificateState;
    QString m_certificateFingerprint;
    QString m_certificateExpiry;
    QString m_certificateAlgorithm;
    QString m_certificatePath;
};
