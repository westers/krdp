// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-K1/K2/K5/K6/K9/K10: the KCM's logic, without loading the QML plugin.

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

#include <KSharedConfig>

#include "ServerSettingsPolicy.h"
#include "krdpserversettings.h"
#include "serviceinfo.h"
#include "settingsdefaults.h"
#include "useraccounts.h"
#include "usersmodel.h"

using namespace Qt::StringLiterals;

using namespace KRdp::ServerSettings;

namespace
{
// Completes each keychain operation asynchronously, like QtKeychain does, and
// fails the ones it was told to.
class FakePasswordStore : public PasswordStore
{
public:
    QHash<QString, QString> entries;
    QSet<QString> failWrite;
    QSet<QString> failDelete;
    QStringList log;

    void writePassword(const QString &user, const QString &password, Done done) override
    {
        log << u"write "_s + user;
        QTimer::singleShot(0, this, [this, user, password, done]() {
            if (failWrite.contains(user)) {
                done(false, QStringLiteral("wallet closed"));
                return;
            }
            entries.insert(user, password);
            done(true, {});
        });
    }
    void deletePassword(const QString &user, Done done) override
    {
        log << u"delete "_s + user;
        QTimer::singleShot(0, this, [this, user, done]() {
            if (failDelete.contains(user)) {
                done(false, QStringLiteral("wallet closed"));
                return;
            }
            entries.remove(user);
            done(true, {});
        });
    }
    void readPassword(const QString &user, ReadDone done) override
    {
        QTimer::singleShot(0, this, [this, user, done]() {
            if (!entries.contains(user)) {
                done(false, {}, QStringLiteral("not found"));
                return;
            }
            done(true, entries.value(user), {});
        });
    }
};

struct Accounts {
    FakePasswordStore store;
    QStringList users;
    int commits = 0;
    UserAccounts accounts{&store,
                          [this]() {
                              return users;
                          },
                          [this](const QStringList &u) {
                              users = u;
                              ++commits;
                          }};

    bool settle()
    {
        return QTest::qWaitFor([this]() {
            return accounts.pendingOperations() == 0;
        });
    }
};
}

class KcmSettingsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + u"/krdpserverrc"_s);
    }

    // --- K1 -------------------------------------------------------------
    void defaultsPreserveCertificateAndUsers()
    {
        KRDPServerSettings settings(nullptr);
        settings.setCertificate(u"/home/u/my.crt"_s);
        settings.setCertificateKey(u"/home/u/my.key"_s);
        settings.setUsers({u"alice"_s, u"bob"_s});
        settings.setSystemUserEnabled(true);
        settings.setQuality(60);
        settings.setCodec(u"avc420"_s);
        settings.setSoftwareEncoding(u"prefer"_s);
        settings.setAv1Tiles(u"8"_s);
        settings.setMonitorMode(u"virtual"_s);
        settings.setListenAddress(u"192.168.1.5"_s);
        settings.setAutogenerateCertificates(false);
        QVERIFY(!resettableSettingsAreDefault(&settings));

        const auto preserved = PreservedSettings::capture(&settings);
        settings.setDefaults();
        preserved.restore(&settings);

        QCOMPARE(settings.certificate(), u"/home/u/my.crt"_s);
        QCOMPARE(settings.certificateKey(), u"/home/u/my.key"_s);
        QCOMPARE(settings.users(), QStringList({u"alice"_s, u"bob"_s}));
        QVERIFY(settings.systemUserEnabled());
        QCOMPARE(settings.quality(), settings.defaultQualityValue());
        QCOMPARE(settings.codec(), u"auto"_s);
        QCOMPARE(settings.softwareEncoding(), u"auto"_s);
        QCOMPARE(settings.av1Tiles(), u"auto"_s);
        QCOMPARE(settings.monitorMode(), u"workspace"_s);
        QCOMPARE(settings.listenAddress(), QString());
        QVERIFY(settings.autogenerateCertificates());
        // Defaults is "done" even though the preserved settings kept their values.
        QVERIFY(resettableSettingsAreDefault(&settings));
    }

    void everyKeyIsEitherResetOrPreserved()
    {
        // A new kcfg key is reset by Defaults unless it is added to the
        // preserved list on purpose.
        KRDPServerSettings settings(nullptr);
        const auto preserved = preservedSettingNames();
        for (const auto &name : preserved) {
            QVERIFY2(settings.findItem(name), qPrintable(name));
        }
        QVERIFY(!preserved.contains(u"ListenAddress"_s));
        QVERIFY(settings.findItem(u"ListenAddress"_s));
        QVERIFY(!settings.findItem(u"Autostart"_s)); // systemd owns autostart now (K11)
    }

    // --- K6 -------------------------------------------------------------
    void restartDetection()
    {
        StartupSettings saved;
        saved.listenPort = 3389;
        saved.users = {u"alice"_s};
        saved.systemUserEnabled = true;

        LoadedState loaded;
        loaded.pid = 42;
        loaded.loadedAt = QDateTime::fromSecsSinceEpoch(1000, QTimeZone::UTC);
        loaded.backend = Backend::Portal;
        loaded.settings = saved;

        // Nothing recorded (server not running): nothing to restart.
        QVERIFY(restartReasons(std::nullopt, saved, u"workspace"_s, std::nullopt).isEmpty());
        // Same settings: nothing.
        QVERIFY(restartReasons(loaded, saved, u"workspace"_s, std::nullopt).isEmpty());

        auto changed = saved;
        changed.listenPort = 3390;
        changed.listenAddress = u"10.0.0.2"_s;
        changed.users << u"bob"_s;
        QCOMPARE(restartReasons(loaded, changed, u"workspace"_s, std::nullopt), QStringList({u"ListenPort"_s, u"ListenAddress"_s, u"Users"_s}));

        // Certificate paths only matter when the server does not manage its own.
        changed = saved;
        changed.certificate = u"/x.crt"_s;
        QVERIFY(restartReasons(loaded, changed, u"workspace"_s, std::nullopt).isEmpty());
        changed.autogenerateCertificates = false;
        QCOMPARE(restartReasons(loaded, changed, u"workspace"_s, std::nullopt), QStringList({u"AutogenerateCertificates"_s, u"Certificate"_s}));

        // A password stored after the server started needs a restart; one before does not.
        QVERIFY(restartReasons(loaded, saved, u"workspace"_s, loaded.loadedAt.addSecs(-5)).isEmpty());
        QCOMPARE(restartReasons(loaded, saved, u"workspace"_s, loaded.loadedAt.addSecs(5)), QStringList({u"Passwords"_s}));

        // multi/virtual on a portal-backed server needs a restart (K5); a
        // Plasma-backed one serves every mode live.
        QCOMPARE(restartReasons(loaded, saved, u"multi"_s, std::nullopt), QStringList({u"Backend"_s}));
        loaded.backend = Backend::Plasma;
        QVERIFY(restartReasons(loaded, saved, u"virtual"_s, std::nullopt).isEmpty());
    }

    void loadedStateRoundTrip()
    {
        LoadedState state;
        state.pid = 1234;
        state.loadedAt = QDateTime::fromMSecsSinceEpoch(1'700'000'000'123, QTimeZone::UTC);
        state.backend = Backend::Plasma;
        state.settings = {3391, u"192.168.1.9"_s, false, u"/c.crt"_s, u"/c.key"_s, {u"ü-user"_s}, true};
        const auto parsed = parseLoadedState(serializeLoadedState(state));
        QVERIFY(parsed);
        QCOMPARE(parsed->pid, state.pid);
        QCOMPARE(parsed->loadedAt, state.loadedAt);
        QCOMPARE(parsed->backend, Backend::Plasma);
        QVERIFY(parsed->settings == state.settings);

        QVERIFY(!parseLoadedState("not json"));
        QVERIFY(!parseLoadedState(R"({"v":2,"settings":{"ListenPort":1}})"));
        // Per config file: a test instance never reads the live service's state.
        QVERIFY(loadedStatePath(u"/a/krdpserverrc"_s) != loadedStatePath(u"/b/krdpserverrc"_s));
        QCOMPARE(loadedStatePath(u"/a/krdpserverrc"_s), loadedStatePath(u"/a/krdpserverrc"_s));
    }

    void startupSettingsComeFromTheSavedKeys()
    {
        KRDPServerSettings settings(nullptr);
        settings.setListenPort(4000);
        settings.setListenAddress(u"::1"_s);
        settings.setUsers({u"a"_s});
        const auto s = startupSettingsFrom(&settings);
        QCOMPARE(s.listenPort, 4000);
        QCOMPARE(s.listenAddress, u"::1"_s);
        QCOMPARE(s.users, QStringList({u"a"_s}));
    }

    // --- K2 -------------------------------------------------------------
    void addUserStoresPasswordFirst()
    {
        Accounts a;
        QSignalSpy stored(&a.accounts, &UserAccounts::passwordStored);
        a.accounts.addUser(u"alice"_s, u"pw"_s);
        QCOMPARE(a.commits, 0); // nothing before the keychain answered
        QVERIFY(a.settle());
        QCOMPARE(a.users, QStringList({u"alice"_s}));
        QCOMPARE(a.store.entries.value(u"alice"_s), u"pw"_s);
        QCOMPARE(stored.count(), 1);
    }

    void addUserWriteErrorIsReportedAndUserNotAdded()
    {
        Accounts a;
        a.store.failWrite << u"alice"_s;
        QSignalSpy errors(&a.accounts, &UserAccounts::keychainError);
        a.accounts.addUser(u"alice"_s, u"pw"_s);
        QVERIFY(a.settle());
        QCOMPARE(errors.count(), 1);
        QVERIFY(errors.at(0).at(0).toString().contains(u"wallet closed"_s));
        QVERIFY(a.users.isEmpty());
        QCOMPARE(a.commits, 0);
    }

    void renameDeletesOldEntryOnlyAfterWrite()
    {
        Accounts a;
        a.users = {u"alice"_s, u"carol"_s};
        a.store.entries = {{u"alice"_s, u"old"_s}, {u"carol"_s, u"c"_s}};
        a.accounts.modifyUser(u"alice"_s, u"alicia"_s, u"new"_s);
        QVERIFY(a.settle());
        QCOMPARE(a.store.log, QStringList({u"write alicia"_s, u"delete alice"_s}));
        QCOMPARE(a.users, QStringList({u"alicia"_s, u"carol"_s})); // position kept
        QVERIFY(!a.store.entries.contains(u"alice"_s));
        QCOMPARE(a.store.entries.value(u"alicia"_s), u"new"_s);
    }

    void renameWriteFailureKeepsOldUserAndEntry()
    {
        Accounts a;
        a.users = {u"alice"_s};
        a.store.entries = {{u"alice"_s, u"old"_s}};
        a.store.failWrite << u"alicia"_s;
        QSignalSpy errors(&a.accounts, &UserAccounts::keychainError);
        a.accounts.modifyUser(u"alice"_s, u"alicia"_s, u"new"_s);
        QVERIFY(a.settle());
        QCOMPARE(errors.count(), 1);
        QCOMPARE(a.store.log, QStringList({u"write alicia"_s})); // never deleted
        QCOMPARE(a.users, QStringList({u"alice"_s}));
        QCOMPARE(a.store.entries.value(u"alice"_s), u"old"_s);
    }

    void renameDeleteFailureIsReported()
    {
        Accounts a;
        a.users = {u"alice"_s};
        a.store.entries = {{u"alice"_s, u"old"_s}};
        a.store.failDelete << u"alice"_s;
        QSignalSpy errors(&a.accounts, &UserAccounts::keychainError);
        a.accounts.modifyUser(u"alice"_s, u"alicia"_s, u"new"_s);
        QVERIFY(a.settle());
        QCOMPARE(errors.count(), 1);
        QCOMPARE(a.users, QStringList({u"alicia"_s}));
    }

    void passwordChangeAndDeleteErrors()
    {
        Accounts a;
        a.users = {u"alice"_s, u"bob"_s};
        a.store.failWrite << u"alice"_s;
        a.store.failDelete << u"bob"_s;
        QSignalSpy errors(&a.accounts, &UserAccounts::keychainError);
        a.accounts.modifyUser(u"alice"_s, QString(), u"pw2"_s);
        a.accounts.deleteUser(u"bob"_s);
        QVERIFY(a.settle());
        QCOMPARE(errors.count(), 2);
        QCOMPARE(a.users, QStringList({u"alice"_s})); // bob removed; its stale entry reported
    }

    void readErrorIsReported()
    {
        Accounts a;
        QSignalSpy errors(&a.accounts, &UserAccounts::keychainError);
        QSignalSpy loaded(&a.accounts, &UserAccounts::passwordLoaded);
        a.accounts.readPassword(u"nobody"_s);
        QVERIFY(a.settle());
        QCOMPARE(errors.count(), 1);
        QCOMPARE(loaded.count(), 0);
    }

    // --- K10 ------------------------------------------------------------
    void usersModelCountsOnlyRealLoginMethods()
    {
        KRDPServerSettings settings(nullptr);
        settings.setUsers({});
        settings.setSystemUserEnabled(false);
        UsersModel model(&settings);
        QSignalSpy changed(&model, &UsersModel::loginMethodsChanged);

        QCOMPARE(model.rowCount(), 1); // the system-user row is always there
        QCOMPARE(model.additionalUserCount(), 0);
        QCOMPARE(model.loginMethodCount(), 0);

        QVERIFY(model.setData(model.index(0), true, UsersModel::SystemUserEnabledRole));
        QCOMPARE(model.loginMethodCount(), 1);
        QCOMPARE(model.additionalUserCount(), 0);

        model.setUsers({u"alice"_s, u"bob"_s});
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.additionalUserCount(), 2);
        QCOMPARE(model.loginMethodCount(), 3);

        // Defaults/load change the settings directly; the counts follow.
        settings.setSystemUserEnabled(false);
        QCOMPARE(model.loginMethodCount(), 2);
        QVERIFY(changed.count() >= 3);
    }

    // --- K9 / K5 --------------------------------------------------------
    void activeStateMapping()
    {
        using namespace SystemdService;
        QCOMPARE(statusFromActiveState(u"active"_s), Running);
        QCOMPARE(statusFromActiveState(u"reloading"_s), Running);
        QCOMPARE(statusFromActiveState(u"activating"_s), Busy);
        QCOMPARE(statusFromActiveState(u"deactivating"_s), Busy);
        QCOMPARE(statusFromActiveState(u"failed"_s), Failed);
        QCOMPARE(statusFromActiveState(u"inactive"_s), Stopped);
        QCOMPARE(statusFromActiveState(u"what"_s), Unknown);
    }

    void commandLineOverrides()
    {
        using namespace SystemdService;
        QVERIFY(SystemdService::commandLineOverrides({u"/usr/bin/krdpserver"_s}).isEmpty());
        QVERIFY(SystemdService::commandLineOverrides({u"krdpserver"_s, u"--plasma"_s}).isEmpty());

        const auto overrides = SystemdService::commandLineOverrides(
            {u"krdpserver"_s, u"--monitor"_s, u"1"_s, u"--port=3390"_s, u"-quality"_s, u"90"_s, u"--password"_s, u"s3cret"_s, u"--monitor"_s, u"2"_s});
        QCOMPARE(overrides.size(), 4);
        QCOMPARE(overrides.at(0).option, u"--monitor"_s);
        QCOMPARE(overrides.at(0).setting, u"MonitorMode"_s);
        QCOMPARE(overrides.at(1).option, u"--port"_s);
        QCOMPARE(overrides.at(1).setting, u"ListenPort"_s);
        QCOMPARE(overrides.at(2).option, u"--quality"_s);
        QCOMPARE(overrides.at(3).setting, u"Users"_s);
        for (const auto &o : overrides) {
            QVERIFY(!o.option.contains(u"s3cret"_s)); // values are never returned
        }
    }
};

QTEST_GUILESS_MAIN(KcmSettingsTest)
#include "KcmSettingsTest.moc"

