// SPDX-FileCopyrightText: 2024 Akseli Lahtinen <akselmo@akselmo.dev>
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "kcmkrdpserver.h"
#include "ClientDisplayInfo.h"
#include "ServerCertificate.h"
#include "ServerSettingsPolicy.h"
#include "VideoCodecSupport.h"
#include "journalreader.h"
#include "krdpkcm_logging.h"
#include "krdpserverdata.h"
#include "krdpserversettings.h"
#include "settingsdefaults.h"
#include <PipeWireRecord>

#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>
#include <QClipboard>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QPointer>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>

#include <cerrno>
#include <csignal>

#include "org.freedesktop.impl.portal.PermissionStore.h"

using namespace Qt::StringLiterals;

K_PLUGIN_CLASS_WITH_JSON(KRDPServerConfig, "kcm_krdpserver.json")

static const QString dbusSystemdDestination = u"org.freedesktop.systemd1"_s;
static const QString dbusSystemdPath = u"/org/freedesktop/systemd1"_s;
static const QString krdpServerUnit = u"app-org.kde.krdpserver.service"_s;
static const QString dbusKrdpServerServicePath = u"/org/freedesktop/systemd1/unit/app_2dorg_2ekde_2ekrdpserver_2eservice"_s;
static const QString dbusSystemdUnitInterface = u"org.freedesktop.systemd1.Unit"_s;
static const QString dbusSystemdServiceInterface = u"org.freedesktop.systemd1.Service"_s;
static const QString dbusSystemdManagerInterface = u"org.freedesktop.systemd1.Manager"_s;
static const QString dbusSystemdPropertiesInterface = u"org.freedesktop.DBus.Properties"_s;

namespace
{
QString configFilePath()
{
    // Same expression as krdpserver's runtimeConfigPath (server/main.cpp).
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + u"/krdpserverrc"_s;
}

bool processAlive(qint64 pid)
{
    return pid > 0 && (::kill(pid_t(pid), 0) == 0 || errno == EPERM);
}

QDBusMessage unitPropertyGet(const QString &interface, const QString &property)
{
    auto msg = QDBusMessage::createMethodCall(dbusSystemdDestination, dbusKrdpServerServicePath, dbusSystemdPropertiesInterface, u"Get"_s);
    msg.setArguments({interface, property});
    return msg;
}

// ExecStart is a(sasbttttuii): path, argv, ignore-failure, timestamps, pid, codes.
QStringList execStartArgv(const QVariant &value)
{
    const auto argument = value.value<QDBusArgument>();
    QStringList result;
    argument.beginArray();
    while (!argument.atEnd()) {
        QString path;
        QStringList argv;
        bool ignoreFailure = false;
        quint64 t1 = 0, t2 = 0, t3 = 0, t4 = 0;
        quint32 pid = 0;
        qint32 code = 0, status = 0;
        argument.beginStructure();
        argument >> path >> argv >> ignoreFailure >> t1 >> t2 >> t3 >> t4 >> pid >> code >> status;
        argument.endStructure();
        if (result.isEmpty()) {
            result = argv;
        }
    }
    argument.endArray();
    return result;
}
}

KRDPServerConfig::KRDPServerConfig(QObject *parent, const KPluginMetaData &data)
    : KQuickManagedConfigModule(parent, data)
    // Deliberately not a child: KQuickManagedConfigModule would discover it
    // and treat Users/Certificate like any other setting in its Defaults
    // state (AUD-K1); this module manages it itself.
    , m_serverSettings(new KRDPServerSettings(nullptr))
    , m_usersModel(new UsersModel(m_serverSettings, this))
    , m_runtimeWatcher(new QFileSystemWatcher(this))
{
    setButtons(Help | Apply | Default);
    QQmlEngine::setObjectOwnership(m_serverSettings, QQmlEngine::CppOwnership);

    // Every generated <Key>Changed signal re-evaluates Apply/Defaults.
    const auto settingsChangedSlot = metaObject()->method(metaObject()->indexOfSlot("settingsChanged()"));
    const auto *settingsMeta = m_serverSettings->metaObject();
    for (int i = settingsMeta->methodOffset(); i < settingsMeta->methodCount(); ++i) {
        const auto method = settingsMeta->method(i);
        if (method.methodType() == QMetaMethod::Signal && method.name().endsWith("Changed")) {
            connect(m_serverSettings, method, this, settingsChangedSlot);
        }
    }

    auto recorder = PipeWireRecord();
    m_isH264Supported = recorder.suggestedEncoders().contains(PipeWireRecord::H264Baseline);

    auto store = new KeychainPasswordStore(this);
    m_userAccounts = new UserAccounts(
        store,
        [this]() {
            return m_usersModel->users();
        },
        [this](const QStringList &users) {
            m_usersModel->setUsers(users);
            // The user list is saved with its passwords, not with Apply, and
            // without saving other unapplied edits on the page.
            KConfigGroup group(m_serverSettings->config(), u"General"_s);
            group.writeEntry("Users", users);
            m_serverSettings->config()->sync();
            if (auto item = m_serverSettings->findItem(u"Users"_s)) {
                item->readConfig(m_serverSettings->config());
            }
            settingsChanged();
            refreshRestartState();
            Q_EMIT krdpServerSettingsChanged();
        },
        this);
    connect(m_userAccounts, &UserAccounts::keychainError, this, &KRDPServerConfig::keychainError);
    connect(m_userAccounts, &UserAccounts::passwordLoaded, this, &KRDPServerConfig::passwordLoaded);
    connect(m_userAccounts, &UserAccounts::passwordStored, this, &KRDPServerConfig::recordCredentialsChange);
    connect(m_userAccounts, &UserAccounts::busyChanged, this, [this](bool busy) {
        m_keychainBusy = busy;
        Q_EMIT keychainBusyChanged();
    });

    // The certificate shown is the one the current (unsaved) choice would use.
    connect(m_serverSettings, &KRDPServerSettings::AutogenerateCertificatesChanged, this, &KRDPServerConfig::refreshCertificateInfo);
    connect(m_serverSettings, &KRDPServerSettings::CertificateChanged, this, &KRDPServerConfig::refreshCertificateInfo);
    connect(m_serverSettings, &KRDPServerSettings::CertificateKeyChanged, this, &KRDPServerConfig::refreshCertificateInfo);

    // A server (re)start rewrites its loaded-state file.
    QDir().mkpath(KRdp::ServerSettings::runtimeDirectory());
    m_runtimeWatcher->addPath(KRdp::ServerSettings::runtimeDirectory());
    connect(m_runtimeWatcher, &QFileSystemWatcher::directoryChanged, this, &KRDPServerConfig::refreshRestartState);

    QDBusConnection::sessionBus().connect(dbusSystemdDestination,
                                          dbusKrdpServerServicePath,
                                          dbusSystemdPropertiesInterface,
                                          u"PropertiesChanged"_s,
                                          this,
                                          SLOT(servicePropertiesChanged()));
}

KRDPServerConfig::~KRDPServerConfig()
{
    delete m_serverSettings;
}

QString KRDPServerConfig::toLocalFile(const QUrl &url)
{
    return url.toLocalFile();
}

void KRDPServerConfig::readPasswordFromWallet(const QString &user)
{
    m_userAccounts->readPassword(user);
}

void KRDPServerConfig::addUser(const QString &username, const QString &password)
{
    m_userAccounts->addUser(username, password);
}

void KRDPServerConfig::modifyUser(const QString &oldUsername, const QString &newUsername, const QString &newPassword)
{
    m_userAccounts->modifyUser(oldUsername, newUsername, newPassword);
}

void KRDPServerConfig::deleteUser(const QString &username)
{
    m_userAccounts->deleteUser(username);
}

bool KRDPServerConfig::userExists(const QString &username)
{
    return m_usersModel->users().contains(username);
}

void KRDPServerConfig::recordCredentialsChange()
{
    const QString path = KRdp::ServerSettings::credentialsStampPath(configFilePath());
    QDir().mkpath(KRdp::ServerSettings::runtimeDirectory());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QByteArray::number(QDateTime::currentMSecsSinceEpoch()));
        file.commit();
    }
    refreshRestartState();
}

void KRDPServerConfig::load()
{
    KQuickManagedConfigModule::load();
    m_serverSettings->load();
    readAutostart();
    readServiceCommandLine();
    refreshRestartState();
    refreshCertificateInfo();
}

void KRDPServerConfig::save()
{
    KQuickManagedConfigModule::save();
    m_serverSettings->save();
    applyAutostart();
    refreshRestartState();
    Q_EMIT krdpServerSettingsChanged();
}

void KRDPServerConfig::defaults()
{
    // AUD-K1: everything on the page resets except the preserved settings.
    // Autostart is systemd state, not a setting, and is left alone too.
    KQuickManagedConfigModule::defaults();
    const auto preserved = PreservedSettings::capture(m_serverSettings);
    m_serverSettings->setDefaults();
    preserved.restore(m_serverSettings);
    settingsChanged();
}

bool KRDPServerConfig::isSaveNeeded() const
{
    return m_serverSettings->isSaveNeeded() || m_autostart != m_autostartActual;
}

bool KRDPServerConfig::isDefaults() const
{
    return resettableSettingsAreDefault(m_serverSettings);
}

void KRDPServerConfig::refreshRestartState()
{
    using namespace KRdp::ServerSettings;
    std::optional<LoadedState> loaded;
    QFile stateFile(loadedStatePath(configFilePath()));
    if (stateFile.open(QIODevice::ReadOnly)) {
        loaded = parseLoadedState(stateFile.read(64 * 1024));
        if (loaded && !processAlive(loaded->pid)) {
            loaded.reset();
        }
    }
    if (m_runtimeWatcher->files().isEmpty() && stateFile.exists()) {
        m_runtimeWatcher->addPath(stateFile.fileName());
    }

    std::optional<QDateTime> credentialsChangedAt;
    QFile stamp(credentialsStampPath(configFilePath()));
    if (stamp.open(QIODevice::ReadOnly)) {
        bool ok = false;
        const auto ms = stamp.read(32).trimmed().toLongLong(&ok);
        if (ok) {
            credentialsChangedAt = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
        }
    }

    // Compare with what is saved on disk, not with unsaved edits on the page.
    KRDPServerSettings saved(nullptr);
    saved.load();
    const auto reasons = KRdp::ServerSettings::restartReasons(loaded, startupSettingsFrom(&saved), saved.monitorMode(), credentialsChangedAt);
    QStringList labels;
    for (const auto &key : reasons) {
        const auto label = settingLabel(key);
        if (!labels.contains(label)) {
            labels << label;
        }
    }
    if (labels != m_restartReasons) {
        m_restartReasons = labels;
        Q_EMIT restartRequiredChanged();
    }
}

bool KRDPServerConfig::restartRequired() const
{
    return !m_restartReasons.isEmpty();
}

QStringList KRDPServerConfig::restartReasons() const
{
    return m_restartReasons;
}

QString KRDPServerConfig::settingLabel(const QString &key) const
{
    static const QHash<QString, KLocalizedString> labels{
        {u"ListenPort"_s, ki18nc("@info name of a setting", "Listening port")},
        {u"ListenAddress"_s, ki18nc("@info name of a setting", "Listening address")},
        {u"AutogenerateCertificates"_s, ki18nc("@info name of a setting", "Certificate")},
        {u"Certificate"_s, ki18nc("@info name of a setting", "Certificate")},
        {u"CertificateKey"_s, ki18nc("@info name of a setting", "Certificate")},
        {u"Users"_s, ki18nc("@info name of a setting", "Users")},
        {u"SystemUserEnabled"_s, ki18nc("@info name of a setting", "System user login")},
        {u"Passwords"_s, ki18nc("@info name of a setting", "Passwords")},
        {u"Backend"_s, ki18nc("@info name of a setting", "Display target (needs the Plasma capture backend)")},
        {u"MonitorMode"_s, ki18nc("@info name of a setting", "Display target")},
        {u"Quality"_s, ki18nc("@info name of a setting", "Video quality")},
    };
    const auto it = labels.constFind(key);
    return it == labels.constEnd() ? key : it->toString();
}

void KRDPServerConfig::refreshCertificateInfo()
{
    using namespace KRdp::ServerCertificate;
    const bool managed = m_serverSettings->autogenerateCertificates();
    const Paths paths = managed ? defaultPaths() : Paths{m_serverSettings->certificate(), m_serverSettings->certificateKey()};
    const auto info = inspect(paths);
    const auto decision = decide(info, QDateTime::currentDateTimeUtc());

    QString state;
    switch (decision) {
    case Decision::UseExisting:
        state = u"valid"_s;
        break;
    case Decision::GenerateMissing:
        state = u"missing"_s;
        break;
    case Decision::GenerateUnusable:
        state = u"unusable"_s;
        break;
    case Decision::GenerateExpired:
        state = u"expired"_s;
        break;
    case Decision::GenerateExpiringSoon:
        state = u"expiring"_s;
        break;
    }
    m_certificateState = state;
    m_certificatePath = paths.certificate;
    m_certificateFingerprint = info.certificateReadable ? info.sha256Fingerprint : QString();
    m_certificateAlgorithm = info.certificateReadable ? info.algorithm : QString();
    m_certificateExpiry = info.notAfter.isValid() ? QLocale().toString(info.notAfter.toLocalTime().date(), QLocale::LongFormat) : QString();
    Q_EMIT certificateInfoChanged();
}

QString KRDPServerConfig::certificateState() const
{
    return m_certificateState;
}

QString KRDPServerConfig::certificateFingerprint() const
{
    return m_certificateFingerprint;
}

QString KRDPServerConfig::certificateExpiry() const
{
    return m_certificateExpiry;
}

QString KRDPServerConfig::certificateAlgorithm() const
{
    return m_certificateAlgorithm;
}

QString KRDPServerConfig::certificatePath() const
{
    return m_certificatePath;
}

bool KRDPServerConfig::keychainBusy() const
{
    return m_keychainBusy;
}

bool KRDPServerConfig::isValidPort(const QString &text) const
{
    return KRdp::ServerSettings::parseListenPort(text.trimmed()).has_value();
}

bool KRDPServerConfig::isValidFallbackSize(const QString &text) const
{
    // Same rule as SessionController::parseSize().
    static const QRegularExpression rx(uR"(^\s*(\d+)\s*x\s*(\d+)\s*$)"_s);
    const auto match = rx.match(text);
    return match.hasMatch() && KRdp::ClientDisplay::usable(QSize(match.capturedView(1).toInt(), match.capturedView(2).toInt()));
}

bool KRDPServerConfig::isValidChromaPolicy(int motionGapMs, int restMs, int maxGapMs) const
{
    return KRdp::ChromaPolicy{motionGapMs, restMs, maxGapMs}.isValid();
}

bool KRDPServerConfig::monitorModeNeedsPlasma(const QString &mode) const
{
    return KRdp::ServerSettings::monitorModeNeedsPlasma(mode);
}

void KRDPServerConfig::setPortalPreauthorized(bool preauthorized)
{
    // Only the xdg-desktop-portal backend uses this; it lets the server start
    // screen sharing without an interactive prompt.
    auto iface = new OrgFreedesktopImplPortalPermissionStoreInterface(u"org.freedesktop.impl.portal.PermissionStore"_s,
                                                                      u"/org/freedesktop/impl/portal/PermissionStore"_s,
                                                                      QDBusConnection::sessionBus(),
                                                                      this);
    // WARNING: The app_id org.kde.krdpserver must match the service name on the systemd side!
    auto reply = preauthorized ? iface->SetPermission(u"kde-authorized"_s, /* create = */ true, u"remote-desktop"_s, u"org.kde.krdpserver"_s, {u"yes"_s})
                               : iface->DeletePermission(u"kde-authorized"_s, u"remote-desktop"_s, u"org.kde.krdpserver"_s);
    auto watcher = new QDBusPendingCallWatcher(reply, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, iface, preauthorized]() {
        watcher->deleteLater();
        iface->deleteLater();
        QDBusPendingReply<> reply(*watcher);
        if (reply.isError()) {
            qCWarning(KRDPKCM) << "Failed to" << (preauthorized ? "set" : "revoke") << "the portal pre-authorization" << reply.error().message();
        } else {
            qCDebug(KRDPKCM) << (preauthorized ? "Set" : "Revoked") << "the portal pre-authorization";
        }
    });
}

bool KRDPServerConfig::isH264Supported()
{
    return m_isH264Supported;
}

QStringList KRDPServerConfig::listenAddressList()
{
    // With a ListenAddress the server is reachable on that address only.
    const QString configured = m_serverSettings->listenAddress().trimmed();
    if (!configured.isEmpty()) {
        return {configured};
    }
    QStringList addressList;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto &interface : interfaces) {
        if (!interface.flags().testAnyFlag(QNetworkInterface::IsLoopBack)) {
            for (auto &address : interface.addressEntries()) {
                // Show only private ip addresses
                if (address.ip().isPrivateUse()) {
                    addressList.append(address.ip().toString());
                }
            }
        }
    }
    return addressList;
}

QStringList KRDPServerConfig::interfaceAddresses() const
{
    QStringList addresses;
    const auto all = QNetworkInterface::allAddresses();
    for (const auto &address : all) {
        if (address.protocol() == QAbstractSocket::IPv6Protocol && !address.scopeId().isEmpty()) {
            continue; // link-local needs a scope; not useful as a bind address here
        }
        addresses << address.toString();
    }
    return addresses;
}

QStringList KRDPServerConfig::availableMonitorIds() const
{
    QStringList monitors;
    const auto screens = qGuiApp->screens();
    monitors.reserve(screens.size());

    auto *primary = qGuiApp->primaryScreen();
    for (int i = 0; i < screens.size(); ++i) {
        const auto *screen = screens.at(i);
        const auto geometry = screen->geometry();
        // AUD-K12: whole sentences, so translators can order them.
        if (screen == primary) {
            monitors.push_back(i18nc("@item:inlistbox monitor index: name [width x height]", "%1: %2 [%3×%4] (primary)", i, screen->name(), geometry.width(), geometry.height()));
        } else {
            monitors.push_back(i18nc("@item:inlistbox monitor index: name [width x height]", "%1: %2 [%3×%4]", i, screen->name(), geometry.width(), geometry.height()));
        }
    }
    return monitors;
}

QString KRDPServerConfig::hostName() const
{
    return QHostInfo::localHostName();
}

bool KRDPServerConfig::managementAvailable() const
{
    static bool managementAvailable = QDBusConnection::sessionBus().interface()->isServiceRegistered(u"org.freedesktop.systemd1"_s);
    return managementAvailable;
}

bool KRDPServerConfig::plasmaBackendAvailable() const
{
#ifdef WITH_PLASMA_SESSION
    return true;
#else
    return false;
#endif
}

bool KRDPServerConfig::autostart() const
{
    return m_autostart;
}

void KRDPServerConfig::setAutostart(bool autostart)
{
    if (m_autostart == autostart) {
        return;
    }
    m_autostart = autostart;
    Q_EMIT autostartChanged();
    settingsChanged();
}

void KRDPServerConfig::readAutostart()
{
    if (!managementAvailable()) {
        return;
    }
    auto msg = QDBusMessage::createMethodCall(dbusSystemdDestination, dbusSystemdPath, dbusSystemdManagerInterface, u"GetUnitFileState"_s);
    msg.setArguments({krdpServerUnit});
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<QString> reply(*w);
        if (reply.isError()) {
            qCWarning(KRDPKCM) << "Cannot read the unit file state of" << krdpServerUnit << reply.error().message();
            return;
        }
        const bool enabled = reply.value() == "enabled"_L1 || reply.value() == "enabled-runtime"_L1;
        m_autostartActual = enabled;
        if (m_autostart != enabled) {
            m_autostart = enabled;
            Q_EMIT autostartChanged();
        }
        settingsChanged();
    });
}

void KRDPServerConfig::applyAutostart()
{
    if (!managementAvailable() || m_autostart == m_autostartActual) {
        return;
    }
    const bool enabled = m_autostart;
    qCDebug(KRDPKCM) << "Setting KRDP Server service autostart on login to" << enabled << "over QDBus";
    auto msg = QDBusMessage::createMethodCall(dbusSystemdDestination,
                                              dbusSystemdPath,
                                              dbusSystemdManagerInterface,
                                              enabled ? u"EnableUnitFiles"_s : u"DisableUnitFiles"_s);
    if (enabled) {
        msg.setArguments({QStringList(krdpServerUnit), false, true});
    } else {
        msg.setArguments({QStringList(krdpServerUnit), false});
    }
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, enabled](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<> reply(*w);
        if (reply.isError()) {
            setErrorMessage(enabled ? i18nc("@info", "Could not enable autostart: %1", reply.error().message())
                                    : i18nc("@info", "Could not disable autostart: %1", reply.error().message()));
        } else {
            setPortalPreauthorized(enabled || isServerRunning());
        }
        // Show what systemd actually has now.
        readAutostart();
    });
}

void KRDPServerConfig::toggleServer(const bool enabled)
{
    auto msg = QDBusMessage::createMethodCall(dbusSystemdDestination, dbusKrdpServerServicePath, dbusSystemdUnitInterface, enabled ? u"Start"_s : u"Stop"_s);
    msg.setArguments({u"replace"_s});
    qCDebug(KRDPKCM) << "Toggling KRDP Server to" << enabled << "over QDBus";
    // Grant before starting so the portal backend does not prompt; revoke
    // only once nothing will start the server again.
    if (enabled || !m_autostartActual) {
        setPortalPreauthorized(enabled);
    }
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, enabled](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> reply(*w);
        if (reply.isError()) {
            setErrorMessage(enabled ? i18nc("@info", "Could not start the server: %1", reply.error().message())
                                    : i18nc("@info", "Could not stop the server: %1", reply.error().message()));
        }
        updateServerStatus();
    });
}

void KRDPServerConfig::restartServer()
{
    qCDebug(KRDPKCM) << "Restarting KRDP Server";
    auto restartMsg = QDBusMessage::createMethodCall(dbusSystemdDestination, dbusKrdpServerServicePath, dbusSystemdUnitInterface, u"Restart"_s);
    restartMsg.setArguments({u"replace"_s});
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(restartMsg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> reply(*w);
        if (reply.isError()) {
            setErrorMessage(i18nc("@info", "Could not restart the server: %1", reply.error().message()));
        }
        updateServerStatus();
    });
}

void KRDPServerConfig::readServiceCommandLine()
{
    if (!managementAvailable()) {
        return;
    }
    auto execWatcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(unitPropertyGet(dbusSystemdServiceInterface, u"ExecStart"_s)), this);
    connect(execWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply(*w);
        if (reply.isError()) {
            return;
        }
        QStringList options;
        QStringList settings;
        const auto overrides = SystemdService::commandLineOverrides(execStartArgv(reply.value().variant()));
        for (const auto &o : overrides) {
            options << o.option;
            const auto label = settingLabel(o.setting);
            if (!settings.contains(label)) {
                settings << label;
            }
        }
        m_commandLineOverrides = options;
        m_overriddenSettings = settings;
        Q_EMIT serviceCommandLineChanged();
    });

    auto dropInWatcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(unitPropertyGet(dbusSystemdUnitInterface, u"DropInPaths"_s)), this);
    connect(dropInWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply(*w);
        if (reply.isError()) {
            return;
        }
        m_serviceDropIns = reply.value().variant().toStringList();
        Q_EMIT serviceCommandLineChanged();
    });
}

QStringList KRDPServerConfig::commandLineOverrides() const
{
    return m_commandLineOverrides;
}

QStringList KRDPServerConfig::overriddenSettings() const
{
    return m_overriddenSettings;
}

QStringList KRDPServerConfig::serviceDropIns() const
{
    return m_serviceDropIns;
}

void KRDPServerConfig::updateServerStatus()
{
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(unitPropertyGet(dbusSystemdUnitInterface, u"ActiveState"_s)), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply(*w);
        if (reply.isError()) {
            qCWarning(KRDPKCM) << "Cannot read the state of" << krdpServerUnit << reply.error().message();
            setServerStatus(SystemdService::Unknown);
            return;
        }
        const QString activeState = reply.value().variant().toString();
        const auto status = SystemdService::statusFromActiveState(activeState);
        if (status == SystemdService::Unknown) {
            qCWarning(KRDPKCM) << "Systemd unit replied with unknown state:" << activeState;
        }
        if (status == SystemdService::Failed) {
            auto idWatcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(unitPropertyGet(dbusSystemdUnitInterface, u"InvocationID"_s)),
                                                         this);
            connect(idWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                const QDBusPendingReply<QDBusVariant> reply(*w);
                if (!reply.isError()) {
                    fetchJournal(QString::fromLatin1(reply.value().variant().toByteArray().toHex()));
                }
            });
        }
        setServerStatus(status);
        refreshRestartState();
    });
}

void KRDPServerConfig::fetchJournal(const QString &invocationId)
{
    // AUD-K7: sd_journal can take a while on a big journal; keep it off the GUI thread.
    QPointer self(this);
    QThreadPool::globalInstance()->start([self, invocationId]() {
        const auto lines = readServiceJournal(krdpServerUnit, invocationId, 20);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, lines]() {
                if (self) {
                    self->setErrorMessage(lines.join("\n"_L1));
                }
            },
            Qt::QueuedConnection);
    });
}

SystemdService::Status KRDPServerConfig::serverStatus() const
{
    return m_currentServerStatus;
}

void KRDPServerConfig::setServerStatus(SystemdService::Status status)
{
    if (m_currentServerStatus != status) {
        m_currentServerStatus = status;
        Q_EMIT serverStatusChanged();
    }
}

bool KRDPServerConfig::isServerRunning() const
{
    return m_currentServerStatus == SystemdService::Running;
}

bool KRDPServerConfig::isServerBusy() const
{
    return m_currentServerStatus == SystemdService::Busy;
}

QString KRDPServerConfig::errorMessage() const
{
    return m_lastErrorMessage;
}

void KRDPServerConfig::setErrorMessage(const QString &errorMessage)
{
    m_lastErrorMessage = errorMessage;
    Q_EMIT errorMessageChanged();
}

void KRDPServerConfig::copyAddressToClipboard(const QString &address)
{
    QGuiApplication::clipboard()->setText(address.trimmed());
}

void KRDPServerConfig::servicePropertiesChanged()
{
    updateServerStatus();
}

#include "kcmkrdpserver.moc"
