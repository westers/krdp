#include <QTest>
#include "VirtualSessionState.h"

using KRdp::VirtualSessionState;
using Phase = VirtualSessionState::Phase;

class VirtualSessionStateTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void disconnectRetainsSameGeneration()
    {
        VirtualSessionState state(1000);
        const auto generation = state.create(1000);
        QVERIFY(generation);
        QVERIFY(!state.attach(1000, generation, 1));
        QVERIFY(state.ready(generation));
        for (quint64 client = 1; client <= 3; ++client) {
            QVERIFY(state.attach(1000, generation, client));
            QVERIFY(!state.disconnect(generation, client + 1));
            QVERIFY(state.disconnect(generation, client));
            QCOMPARE(state.phase(), Phase::Retained);
            QCOMPARE(state.generation(), generation);
            QCOMPARE(state.create(1000), quint64(0));
        }
    }
    void ownershipAndSingleController()
    {
        VirtualSessionState state(1000);
        QCOMPARE(state.create(1001), quint64(0));
        const auto generation = state.create(1000);
        QVERIFY(state.ready(generation));
        QVERIFY(!state.attach(1001, generation, 1));
        QVERIFY(!state.attach(1000, generation, 0));
        QVERIFY(!state.stop(1001, generation));
        QVERIFY(state.attach(1000, generation, 1));
        QVERIFY(!state.attach(1000, generation, 2));
        QCOMPARE(state.client(), quint64(1));
        VirtualSessionState root(0);
        QCOMPARE(root.create(0), quint64(0));
    }
    void stopWaitsForExitAndRejectsLateCallbacks()
    {
        VirtualSessionState state(1000);
        const auto old = state.create(1000);
        QVERIFY(state.stop(1000, old));
        QVERIFY(!state.ready(old));
        QCOMPARE(state.create(1000), quint64(0));
        QVERIFY(state.exited(old));
        QCOMPARE(state.phase(), Phase::Absent);
        const auto next = state.create(1000);
        QVERIFY(next > old);
        QVERIFY(!state.ready(old));
        QVERIFY(!state.exited(old));
        QVERIFY(!state.stop(1000, old));
        QVERIFY(state.ready(next));
        QVERIFY(state.attach(1000, next, 1));
        QVERIFY(!state.disconnect(old, 1));
        QCOMPARE(state.phase(), Phase::Attached);
    }
    void crashIsNotRetention()
    {
        VirtualSessionState state(1000);
        const auto generation = state.create(1000);
        QVERIFY(state.ready(generation));
        QVERIFY(state.attach(1000, generation, 1));
        QVERIFY(state.exited(generation));
        QCOMPARE(state.phase(), Phase::Failed);
        QCOMPARE(state.client(), quint64(0));
        QVERIFY(!state.attach(1000, generation, 2));
        QVERIFY(!state.ready(generation));
        QVERIFY(!state.exited(generation));
        QVERIFY(state.create(1000) > generation);
    }
};
QTEST_GUILESS_MAIN(VirtualSessionStateTest)
#include "VirtualSessionStateTest.moc"
