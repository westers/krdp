// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include <QUuid>
#include "VirtualSessionRegistry.h"

using Registry = KRdp::VirtualSessionRegistry;
using Phase = Registry::Phase;

class VirtualSessionRegistryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // AUD-FIX F5: failed desktops stay listed but hold no slot; restarting
    // one takes a slot again.
    void failedEntriesHoldNoSlot()
    {
        Registry registry(4, 6);
        QList<Registry::Handle> failed;
        for (int i = 0; i < 4; ++i) {
            const auto handle = registry.create(1000);
            QVERIFY(handle);
            QVERIFY(registry.unavailable(*handle));
            failed.append(*handle);
        }
        QCOMPARE(registry.list(1000).size(), 4);
        // Four failed records (Sol, 2026-09-27) no longer lock the user out.
        const auto fresh = registry.create(1000);
        QVERIFY(fresh);
        QVERIFY(registry.canReserve({{1000, QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
        for (int i = 0; i < 3; ++i) QVERIFY(registry.create(1000));
        // Four live desktops: the per-user limit holds for them.
        QVERIFY(!registry.create(1000));
        QVERIFY(!registry.recreate(1000, failed.first().id));
        // The host total (6) counts live desktops only: 4 failed + 4 live so far.
        QVERIFY(registry.create(1001));
        QVERIFY(registry.create(1001));
        QVERIFY(!registry.create(1001)); // 6 live
        QCOMPARE(registry.list(1000).size(), 8);
        QVERIFY(!registry.create(1000));
        // Once a live one is gone a failed one can be restarted.
        QVERIFY(registry.stop(1000, fresh->id));
        QVERIFY(registry.exited(*fresh));
        QVERIFY(registry.forget(1000, fresh->id));
        QVERIFY(registry.recreate(1000, failed.first().id));
    }
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
