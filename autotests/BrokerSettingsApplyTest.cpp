// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// The module-wide Apply / Reset / Defaults transaction over the five scoped
// drafts, driven against fake helpers that count how many times they run.
#include "brokerauthenticationsettings.h"
#include "brokerhostsettings.h"
#include "brokerpreferences.h"
#include "brokersettingsapply.h"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
using namespace Qt::StringLiterals;
class BrokerSettingsApplyTest : public QObject {
    Q_OBJECT
    using Scope = BrokerHostSettings::Scope;
    struct Fixture {
        QTemporaryDir hostDir, authDir, prefsDir;
        BrokerHostSettings console, virtualHost, session;
        BrokerAuthenticationSettings authentication;
        BrokerPreferences preferences;
        BrokerSettingsApply apply;
        static QStringList host(const QTemporaryDir &d) { return {qEnvironmentVariable("FARSIDE_HOST_TEST_FIXTURE", QString::fromUtf8(HOST_PROTOCOL_FIXTURE)), d.path()}; }
        Fixture(int timeoutMs = 4000)
            : console(Scope::Console, u"/usr/bin/python3"_s, host(hostDir), timeoutMs),
              virtualHost(Scope::Virtual, u"/usr/bin/python3"_s, host(hostDir), timeoutMs),
              session(Scope::VirtualSession, u"/usr/bin/python3"_s, host(hostDir), timeoutMs),
              authentication(u"/usr/bin/python3"_s, {qEnvironmentVariable("FARSIDE_AUTH_TEST_FIXTURE", QString::fromUtf8(AUTH_FIXTURE)), authDir.path()}, nullptr),
              preferences(prefsDir.path()),
              apply(&authentication, &console, &virtualHost, &session, &preferences) {}
        bool load() {
            for (auto *h : {&console, &virtualHost, &session}) { if (!h->reload() || !QTest::qWaitFor([h] { return !h->busy(); }, 5000) || !h->loaded()) return false; }
            if (!authentication.reload() || !QTest::qWaitFor([this] { return !authentication.busy(); }, 5000)) return false;
            if (!preferences.reload() || !authentication.loaded()) return false;
            // The load itself invoked the helpers; only Apply is counted below.
            QFile::remove(hostDir.filePath(u"invocations"_s)); QFile::remove(authDir.filePath(u"invocations"_s));
            return true;
        }
        void editAll() {
            QVERIFY(console.setValue(u"Port"_s, u"3401"_s));
            QVERIFY(virtualHost.setValue(u"Port"_s, u"3405"_s));
            QVERIFY(session.setValue(u"VaapiDriver"_s, u"radeonsi"_s));
            QVERIFY(authentication.setPam(u"console"_s, u"any"_s, {}));
            QVERIFY(preferences.setValue(u"Quality"_s, u"70"_s));
        }
        void writeMode(const QString &name, const QByteArray &value, bool auth = false) {
            QFile f((auth ? authDir : hostDir).filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(value);
        }
        QStringList invocations(bool auth = false) const {
            QFile f((auth ? authDir : hostDir).filePath(u"invocations"_s));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).split(u'\n', Qt::SkipEmptyParts) : QStringList{};
        }
        bool settle() { QTest::qWait(0); return QTest::qWaitFor([this] { return !apply.applying() && !console.busy() && !virtualHost.busy() && !session.busy() && !authentication.busy(); }, 8000); }
    };
private Q_SLOTS:
    void editingAnyScopeSetsNeedsSaveAndResetClearsAll() {
        Fixture f; QVERIFY(f.load()); QVERIFY(!f.apply.needsSave()); QVERIFY(f.apply.representsDefaults());
        const QList<std::function<void()>> edits{
            [&] { f.console.setValue(u"Port"_s, u"3401"_s); }, [&] { f.virtualHost.setValue(u"Quality"_s, u"50"_s); },
            [&] { f.session.setValue(u"RenderPci"_s, u"0000:01:00.0"_s); }, [&] { f.authentication.setPam(u"virtual"_s, u"any"_s, {}); },
            [&] { f.preferences.setValue(u"Quality"_s, u"60"_s); }};
        for (const auto &edit : edits) {
            QSignalSpy spy(&f.apply, &BrokerSettingsApply::stateChanged);
            edit(); QVERIFY(f.apply.needsSave()); QVERIFY(!spy.isEmpty());
            f.apply.reset(); QVERIFY2(!f.apply.needsSave(), "one scope survived Reset");
        }
        f.editAll(); QVERIFY(f.apply.needsSave()); QVERIFY(!f.apply.representsDefaults());
        QVERIFY(f.console.modified() && f.virtualHost.modified() && f.session.modified() && f.authentication.modified() && f.preferences.modified());
        f.apply.reset();
        QVERIFY(!f.console.modified() && !f.virtualHost.modified() && !f.session.modified() && !f.authentication.modified() && !f.preferences.modified());
        QVERIFY(!f.apply.needsSave()); QVERIFY(f.invocations().isEmpty() && f.invocations(true).isEmpty()); // Reset never runs a helper
    }
    void oneHelperProcessSavesAllDirtyHostScopesThenOneForAccess() {
        Fixture f; QVERIFY(f.load()); f.editAll();
        f.apply.apply();
        // Hosts first: the access helper must not start (or prompt) while the host helper is still running.
        QVERIFY(f.apply.applying()); QVERIFY(f.console.busy() && f.virtualHost.busy() && f.session.busy()); QVERIFY(!f.authentication.busy());
        QVERIFY(!f.preferences.modified()); // my preferences need no authorization and save at once
        QVERIFY(f.settle());
        QCOMPARE(f.invocations(), QStringList{u"save-batch"_s});   // 3 host scopes, ONE process
        QCOMPARE(f.invocations(true).size(), 1);                    // access policy: its own helper
        QCOMPARE(f.apply.lastHelperInvocations(), 2);
        QVERIFY2(f.apply.failures().isEmpty(), qPrintable(f.apply.failures().join(u"|"_s)));
        QVERIFY(!f.apply.needsSave()); QVERIFY(f.console.applicationRequired() && f.virtualHost.applicationRequired() && f.session.applicationRequired());
        QCOMPARE(f.console.values()[u"Port"_s].toString(), u"3401"_s); QCOMPARE(f.virtualHost.values()[u"Port"_s].toString(), u"3405"_s);
        QCOMPARE(f.session.values()[u"VaapiDriver"_s].toString(), u"radeonsi"_s); QVERIFY(f.authentication.lastSaveRequiresRestart());
    }
    void singleDirtyHostScopeUsesThePlainProtocol() {
        Fixture f; QVERIFY(f.load()); QVERIFY(f.console.setValue(u"Quality"_s, u"60"_s));
        f.apply.apply(); QVERIFY(f.settle());
        QCOMPARE(f.invocations(), QStringList{u"save"_s}); QVERIFY(f.invocations(true).isEmpty());
        QCOMPARE(f.apply.lastHelperInvocations(), 1); QVERIFY(f.apply.failures().isEmpty()); QVERIFY(!f.apply.needsSave());
    }
    void nothingPendingRunsNoHelper() {
        Fixture f; QVERIFY(f.load()); f.apply.apply(); QVERIFY(f.settle());
        QVERIFY(f.invocations().isEmpty() && f.invocations(true).isEmpty()); QCOMPARE(f.apply.lastHelperInvocations(), 0);
    }
    void aRefusedScopeKeepsItsDraftAndTheOthersStillSave() {
        Fixture f; QVERIFY(f.load()); f.editAll();
        f.writeMode(u"mode-virtual"_s, "stale");
        f.apply.apply(); QVERIFY(f.settle());
        QCOMPARE(f.invocations(), QStringList{u"save-batch"_s});
        QVERIFY(!f.console.modified()); QVERIFY(!f.session.modified()); QVERIFY(!f.authentication.modified()); QVERIFY(!f.preferences.modified());
        QVERIFY(f.virtualHost.modified()); QCOMPARE(f.virtualHost.values()[u"Port"_s].toString(), u"3405"_s);
        QCOMPARE(f.apply.failures().size(), 1); QVERIFY2(f.apply.failures().first().startsWith(u"Virtual:"_s), qPrintable(f.apply.failures().first()));
        QVERIFY(f.apply.needsSave());
        // Retry after the cause is gone saves just that scope.
        f.writeMode(u"mode-virtual"_s, "success"); QVERIFY(f.virtualHost.reload()); QTRY_VERIFY(!f.virtualHost.busy());
        QVERIFY(!f.virtualHost.modified()); // reload adopts the stored state: the refused draft is gone only when the user chooses to
    }
    void cancelledAuthorizationNamesEveryScopeAndKeepsEdits() {
        Fixture f; QVERIFY(f.load()); f.editAll(); f.writeMode(u"mode"_s, "cancel");
        f.apply.apply(); QVERIFY(f.settle());
        QCOMPARE(f.invocations(), QStringList{u"save-batch"_s});
        QVERIFY(f.console.modified() && f.virtualHost.modified() && f.session.modified());
        QVERIFY(!f.console.outcomeUnknown()); QVERIFY(!f.preferences.modified()); // my preferences never needed it
        QStringList names; for (const auto &failure : f.apply.failures()) names.append(failure.section(u':', 0, 0));
        QVERIFY(names.contains(u"Console"_s) && names.contains(u"Virtual"_s) && names.contains(u"New desktop hardware"_s));
        QVERIFY(f.apply.failures().first().contains(u"cancelled"_s)); QVERIFY(f.apply.needsSave());
        // The access helper ran too (it has its own, separate authorization state) and was not cancelled.
        QVERIFY(!f.authentication.modified());
    }
    void failedAccessPolicyDoesNotBlockHostSaves() {
        Fixture f; QVERIFY(f.load()); f.editAll(); f.writeMode(u"mode"_s, "cancel", true);
        f.apply.apply(); QVERIFY(f.settle());
        QVERIFY(!f.console.modified() && !f.virtualHost.modified() && !f.session.modified());
        QVERIFY(f.authentication.modified()); QCOMPARE(f.apply.failures().size(), 1);
        QVERIFY(f.apply.failures().first().startsWith(u"Who can connect:"_s)); QVERIFY(f.apply.needsSave());
    }
    void crashedBatchMarksEveryHostScopeUncertain() {
        Fixture f; QVERIFY(f.load()); f.editAll(); f.writeMode(u"mode"_s, "crash");
        f.apply.apply(); QVERIFY(f.settle());
        QVERIFY(f.console.outcomeUnknown() && f.virtualHost.outcomeUnknown() && f.session.outcomeUnknown());
        QVERIFY(!f.console.canSave()); QVERIFY(f.console.modified());
        QVERIFY(f.apply.failures().size() >= 3);
    }
    void malformedBatchReplyKeepsDraftsAndIsUncertain() {
        Fixture f; QVERIFY(f.load()); f.editAll(); f.writeMode(u"mode"_s, "malformed");
        f.apply.apply(); QVERIFY(f.settle());
        QVERIFY(f.console.outcomeUnknown() && f.virtualHost.outcomeUnknown() && f.session.outcomeUnknown());
        QVERIFY(f.console.error().contains(u"unexpected reply"_s)); // wording changed in S6; the test was stale
    }
    void invalidScopeIsReportedAndTheValidOnesSaveWithoutIt() {
        Fixture f; QVERIFY(f.load());
        QVERIFY(f.console.setValue(u"Port"_s, u"70000"_s)); QVERIFY(f.virtualHost.setValue(u"Quality"_s, u"55"_s)); QVERIFY(f.session.setValue(u"VaapiDriver"_s, u"iHD"_s));
        QVERIFY(!f.console.canSave());
        f.apply.apply(); QVERIFY(f.settle());
        QCOMPARE(f.invocations(), QStringList{u"save-batch"_s});   // the two valid scopes still share one process
        QVERIFY(f.console.modified()); QVERIFY(!f.virtualHost.modified()); QVERIFY(!f.session.modified());
        QCOMPARE(f.apply.failures().size(), 1); QVERIFY(f.apply.failures().first().startsWith(u"Console:"_s));
    }
    void applyWhileApplyingIsIgnored() {
        Fixture f; QVERIFY(f.load()); f.editAll(); f.apply.apply(); f.apply.apply(); QVERIFY(f.settle());
        QCOMPARE(f.invocations().size(), 1); QCOMPARE(f.invocations(true).size(), 1);
    }
    void defaultsKeepCertificatesAndLeaveTheAccessPolicyAlone() {
        Fixture f; QVERIFY(f.load());
        QVERIFY(f.console.chooseTls(u"existing"_s)); QVERIFY(f.console.setValue(u"Certificate"_s, u"/etc/farside/own.crt"_s));
        QVERIFY(f.console.setValue(u"Port"_s, u"3401"_s)); QVERIFY(f.virtualHost.setValue(u"Quality"_s, u"50"_s));
        QVERIFY(f.session.setValue(u"VaapiDriver"_s, u"iHD"_s)); QVERIFY(f.preferences.setValue(u"Quality"_s, u"60"_s));
        QVERIFY(f.authentication.setPam(u"console"_s, u"any"_s, {}));
        QVERIFY(!f.apply.representsDefaults());
        f.apply.useDefaults();
        QVERIFY(!f.console.values().contains(u"Port"_s)); QVERIFY(!f.virtualHost.values().contains(u"Quality"_s)); QVERIFY(!f.session.values().contains(u"VaapiDriver"_s));
        QVERIFY(!f.preferences.values().contains(u"Quality"_s));
        QCOMPARE(f.console.tlsMode(), u"existing"_s); QCOMPARE(f.console.values()[u"Certificate"_s].toString(), u"/etc/farside/own.crt"_s);
        QVERIFY(f.authentication.modified()); // no default exists for who may connect
        QVERIFY(f.apply.representsDefaults()); QVERIFY(f.apply.needsSave());
        QVERIFY(f.invocations().isEmpty() && f.invocations(true).isEmpty());
    }
};
QTEST_GUILESS_MAIN(BrokerSettingsApplyTest)
#include "BrokerSettingsApplyTest.moc"
