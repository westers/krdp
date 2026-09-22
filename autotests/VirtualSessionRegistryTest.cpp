// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "VirtualSessionRegistry.h"

using Registry = KRdp::VirtualSessionRegistry;
using Phase = Registry::Phase;

class VirtualSessionRegistryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void ownerScopedDiscoveryAndActions()
    {
        Registry registry;
        QVERIFY(!registry.create(0));
        const auto first = registry.create(1000);
        const auto second = registry.create(1001);
        QVERIFY(first && second);
        QVERIFY(first->id != second->id);
        QCOMPARE(registry.list(1000).size(), 1);
        QCOMPARE(registry.list(1000).first().id, first->id);
        QVERIFY(registry.list(0).isEmpty());
        QVERIFY(registry.list(1002).isEmpty());
        QVERIFY(!registry.attach(1001, first->id, 1));
        QVERIFY(!registry.stop(1001, first->id));
        QVERIFY(!registry.recreate(1001, first->id));
        QVERIFY(!registry.forget(1001, first->id));
        QVERIFY(!registry.attach(1000, first->id, 1));
        QVERIFY(registry.ready(*first));
        QVERIFY(registry.attach(1000, first->id, 1));
    }

    void disconnectRetainsAndReattaches()
    {
        Registry registry;
        const auto first = registry.create(1000);
        const auto second = registry.create(1000);
        QVERIFY(first && second);
        QVERIFY(registry.ready(*first));
        QVERIFY(registry.ready(*second));
        QVERIFY(registry.attach(1000, first->id, 42));
        QVERIFY(!registry.attach(1000, second->id, 42));
        QVERIFY(!registry.attach(1000, first->id, 43));
        QVERIFY(!registry.disconnect(*first, 43));
        QVERIFY(!registry.forget(1000, first->id));
        QVERIFY(registry.disconnect(*first, 42));
        const auto resumed = registry.attach(1000, first->id, 43);
        QVERIFY(resumed);
        QCOMPARE(resumed->generation, first->generation);
        QVERIFY(!registry.disconnect(*first, 42));
        QVERIFY(registry.disconnect(*resumed, 43));
        QVERIFY(registry.attach(1000, second->id, 42));
    }

    void crashReplacementRejectsLateCallbacks()
    {
        Registry registry;
        const auto old = registry.create(1000);
        QVERIFY(old);
        QVERIFY(registry.ready(*old));
        QVERIFY(registry.exited(*old));
        QCOMPARE(registry.list(1000).first().phase, Phase::Failed);
        QVERIFY(!registry.attach(1000, old->id, 1));
        const auto next = registry.recreate(1000, old->id);
        QVERIFY(next);
        QVERIFY(next->generation > old->generation);
        QVERIFY(!registry.ready(*old));
        QVERIFY(!registry.exited(*old));
        QVERIFY(!registry.disconnect(*old, 1));
        QVERIFY(registry.ready(*next));
        Registry foreign;
        auto wrongManager = *next;
        wrongManager.manager = foreign.create(1000)->manager;
        QVERIFY(!registry.exited(wrongManager));
        auto wrongId = *next;
        wrongId.id = QStringLiteral("unknown");
        QVERIFY(!registry.exited(wrongId));
        QVERIFY(registry.attach(1000, next->id, 1));
    }

    void boundedAdmissionAndExplicitCleanup()
    {
        Registry registry(1, 2);
        const auto first = registry.create(1000);
        QVERIFY(first);
        QVERIFY(!registry.create(1000));
        QVERIFY(registry.create(1001));
        QVERIFY(!registry.create(1002));
        QVERIFY(!registry.forget(1000, first->id));
        const auto stopping = registry.stop(1000, first->id);
        QVERIFY(stopping);
        QVERIFY(!registry.ready(*first));
        QVERIFY(!registry.forget(1000, first->id));
        QVERIFY(!registry.create(1000));
        QVERIFY(registry.exited(*stopping));
        QVERIFY(registry.forget(1000, first->id));
        QVERIFY(!registry.ready(*first));
        QVERIFY(registry.create(1000));
        Registry disabled(0, 0);
        QVERIFY(!disabled.create(1000));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionRegistryTest)
#include "VirtualSessionRegistryTest.moc"
