// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-D1: RDPECAM devices are keyed by VirtualChannelName. A repeated
// DeviceAddedNotification must not create a second device, and
// DeviceRemovedNotification must hand exactly that device back for teardown
// outside the lock.

#include "RemoteCameraSet.h"

#include <QTest>

#include <atomic>
#include <thread>

namespace
{
struct FakeCamera {
    explicit FakeCamera(int *destroyed, int id = 0)
        : destroyed(destroyed)
        , id(id)
    {
    }
    ~FakeCamera()
    {
        ++*destroyed;
    }
    int *destroyed;
    int id;
};
}

class RemoteCameraSetTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void deduplicatesByChannelName()
    {
        int destroyed = 0;
        KRdp::RemoteCameraSet<FakeCamera> set;
        QVERIFY(set.insert("RDCamera_Device_0", std::make_unique<FakeCamera>(&destroyed, 1)));
        QVERIFY(set.contains("RDCamera_Device_0"));
        // The rejected duplicate is destroyed by the caller's unique_ptr; the original stays.
        QVERIFY(!set.insert("RDCamera_Device_0", std::make_unique<FakeCamera>(&destroyed, 2)));
        QCOMPARE(destroyed, 1);
        QCOMPARE(set.size(), size_t(1));
        QVERIFY(set.insert("RDCamera_Device_1", std::make_unique<FakeCamera>(&destroyed, 3)));
        QCOMPARE(set.size(), size_t(2));
        QList<int> order;
        set.forEach([&order](FakeCamera *camera) {
            order.append(camera->id);
            return true;
        });
        QCOMPARE(order, (QList<int>{1, 3}));
        QVERIFY(!set.insert("RDCamera_Device_2", nullptr));
    }

    void removesOnlyTheNamedDevice()
    {
        int destroyed = 0;
        KRdp::RemoteCameraSet<FakeCamera> set;
        set.insert("a", std::make_unique<FakeCamera>(&destroyed, 1));
        set.insert("b", std::make_unique<FakeCamera>(&destroyed, 2));
        QVERIFY(!set.take("unknown"));
        auto removed = set.take("a");
        QVERIFY(removed);
        QCOMPARE(removed->id, 1);
        QCOMPARE(destroyed, 0); // the caller decides when, outside the lock
        removed.reset();
        QCOMPARE(destroyed, 1);
        QVERIFY(!set.contains("a"));
        QVERIFY(set.contains("b"));
        QVERIFY(!set.take("a"));
        // A device may come back under the same name after removal.
        QVERIFY(set.insert("a", std::make_unique<FakeCamera>(&destroyed, 3)));
        auto all = set.takeAll();
        QCOMPARE(all.size(), size_t(2));
        QCOMPARE(set.size(), size_t(0));
        all.clear();
        QCOMPARE(destroyed, 3);
    }

    void forEachStopsOnFailure()
    {
        int destroyed = 0;
        KRdp::RemoteCameraSet<FakeCamera> set;
        for (int i = 0; i < 3; ++i) {
            set.insert(QByteArray::number(i), std::make_unique<FakeCamera>(&destroyed, i));
        }
        int visited = 0;
        set.forEach([&visited](FakeCamera *) {
            ++visited;
            return visited < 2;
        });
        QCOMPARE(visited, 2);
    }

    void concurrentRemoveWhileSessionIterates()
    {
        // Enumerator thread adds and removes while the session thread walks
        // the devices; every visited pointer must still be alive.
        int destroyed = 0;
        std::atomic<int> liveMarker{0};
        KRdp::RemoteCameraSet<FakeCamera> set;
        std::atomic<bool> done{false};
        std::thread enumerator([&] {
            for (int i = 0; i < 2000; ++i) {
                const QByteArray name = QByteArray::number(i % 4);
                if (!set.insert(name, std::make_unique<FakeCamera>(&destroyed, 7))) {
                    auto removed = set.take(name);
                    removed.reset();
                }
            }
            done = true;
        });
        int visits = 0;
        while (!done) {
            set.forEach([&](FakeCamera *camera) {
                ++visits;
                liveMarker += camera->id; // reads freed memory under ASan if unsafe
                return true;
            });
        }
        enumerator.join();
        QVERIFY(liveMarker.load() == visits * 7);
        set.takeAll();
    }
};

QTEST_GUILESS_MAIN(RemoteCameraSetTest)
#include "RemoteCameraSetTest.moc"
