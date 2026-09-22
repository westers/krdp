// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <csignal>
#include "VirtualSessionSupervisor.h"

using Supervisor = KRdp::VirtualSessionSupervisor;
using Phase = KRdp::VirtualSessionState::Phase;

class VirtualSessionSupervisorTest : public QObject
{
    Q_OBJECT
    static std::optional<Supervisor::Launch> sleeper(quint32, const Supervisor::Handle &)
    {
        return Supervisor::Launch{QStringLiteral("/usr/bin/sleep"), {QStringLiteral("60")}, {}, {}};
    }
private Q_SLOTS:
    void disconnectRetainsActualProcess()
    {
        Supervisor supervisor(sleeper);
        const auto handle = supervisor.create(1000);
        QVERIFY(handle);
        QVERIFY(!supervisor.attach(1000, handle->id, 1));
        bool ready = false;
        QTRY_VERIFY(ready || (ready = supervisor.captureReady(*handle)));
        QVERIFY(supervisor.attach(1000, handle->id, 1));
        QVERIFY(supervisor.disconnect(*handle, 1));
        QCOMPARE(supervisor.list(1000).first().phase, Phase::Retained);
        QVERIFY(supervisor.attach(1000, handle->id, 2));
        QVERIFY(!supervisor.stop(1001, handle->id));
        QVERIFY(supervisor.stop(1000, handle->id));
        QVERIFY(!supervisor.attach(1000, handle->id, 3));
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Absent);
        QVERIFY(supervisor.forget(1000, handle->id));
    }
    void startupTimeoutAndLateReadiness()
    {
        Supervisor supervisor(sleeper, 30, 30);
        const auto old = supervisor.create(1000);
        QVERIFY(old);
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Failed);
        QVERIFY(!supervisor.captureReady(*old));
        const auto next = supervisor.recreate(1000, old->id);
        QVERIFY(next);
        QVERIFY(next->generation > old->generation);
        QVERIFY(!supervisor.captureReady(*old));
        QVERIFY(supervisor.stop(1000, next->id));
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Absent);
    }
    void failedSpawnAndUnsafeFactory()
    {
        Supervisor missing([](quint32, const Supervisor::Handle &) {
            return Supervisor::Launch{QStringLiteral("/nonexistent/krdp-test-program"), {}, {}, {}};
        });
        QVERIFY(missing.create(1000));
        QTRY_COMPARE(missing.list(1000).first().phase, Phase::Failed);
        Supervisor inherited([](quint32, const Supervisor::Handle &) {
            auto spec = sleeper(1000, {});
            spec->environment = QProcessEnvironment(QProcessEnvironment::InheritFromParent);
            return spec;
        });
        QVERIFY(inherited.create(1000));
        QCOMPARE(inherited.list(1000).first().phase, Phase::Failed);
        Supervisor rejected([](quint32, const Supervisor::Handle &) -> std::optional<Supervisor::Launch> { return {}; });
        QVERIFY(rejected.create(1000));
        QCOMPARE(rejected.list(1000).first().phase, Phase::Failed);
    }
    void processExitInvalidatesRetention()
    {
        Supervisor supervisor([](quint32, const Supervisor::Handle &) {
            return Supervisor::Launch{QStringLiteral("/usr/bin/true"), {}, {}, {}};
        });
        const auto handle = supervisor.create(1000);
        QVERIFY(handle);
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Failed);
        QVERIFY(!supervisor.attach(1000, handle->id, 1));
        QVERIFY(!supervisor.captureReady(*handle));
    }
    void forcedStopDoesNotTouchAnotherRuntime()
    {
        Supervisor supervisor([](quint32 uid, const Supervisor::Handle &handle) {
            auto spec = sleeper(uid, handle);
            spec->childSetup = [] { ::signal(SIGTERM, SIG_IGN); };
            return spec;
        }, 5000, 200);
        const auto first = supervisor.create(1000);
        const auto other = supervisor.create(1001);
        QVERIFY(first && other);
        bool readyFirst = false;
        bool readyOther = false;
        QTRY_VERIFY(readyFirst || (readyFirst = supervisor.captureReady(*first)));
        QTRY_VERIFY(readyOther || (readyOther = supervisor.captureReady(*other)));
        QVERIFY(supervisor.stop(1000, first->id));
        QTest::qWait(20);
        QCOMPARE(supervisor.list(1000).first().phase, Phase::Stopping);
        QTRY_COMPARE(supervisor.list(1000).first().phase, Phase::Absent);
        QVERIFY(supervisor.attach(1001, other->id, 9));
        QVERIFY(supervisor.disconnect(*other, 9));
        QCOMPARE(supervisor.list(1001).first().phase, Phase::Retained);
    }
};
QTEST_GUILESS_MAIN(VirtualSessionSupervisorTest)
#include "VirtualSessionSupervisorTest.moc"
