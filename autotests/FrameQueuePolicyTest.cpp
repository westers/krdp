// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QQueue>
#include <QTest>

#include "FrameQueuePolicy.h"

using KRdp::FrameQueuePolicy::dropSupersededFrames;
using KRdp::FrameQueuePolicy::supersededByKeyframe;

namespace
{
// Stands in for VideoFrame: the policy only ever looks at monitorIndex, and
// `id` makes it visible which entries survived and in what order.
struct QueuedFrame {
    int monitorIndex = 0;
    int id = 0;
};

QList<int> idsOf(const QQueue<QueuedFrame> &queue)
{
    QList<int> ids;
    ids.reserve(queue.size());
    for (const auto &frame : queue) {
        ids.push_back(frame.id);
    }
    return ids;
}
}

class FrameQueuePolicyTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Every mode but MonitorMode=multi stamps every frame with index 0, so a
    // keyframe still empties the whole queue - the behaviour this rule has
    // always had, and the reason the single-surface wire output is unchanged.
    void singleSurfaceQueueIsClearedWholesale()
    {
        QQueue<QueuedFrame> queue;
        queue.append({.monitorIndex = 0, .id = 1});
        queue.append({.monitorIndex = 0, .id = 2});
        queue.append({.monitorIndex = 0, .id = 3});

        dropSupersededFrames(queue, 0);

        QVERIFY(queue.isEmpty());
    }

    // The bug this replaces: one monitor's keyframe threw away the frames of
    // every other monitor, breaking their decode until their own next IDR.
    void keyframeOnlySupersedesItsOwnMonitor()
    {
        QQueue<QueuedFrame> queue;
        queue.append({.monitorIndex = 0, .id = 1});
        queue.append({.monitorIndex = 1, .id = 2});
        queue.append({.monitorIndex = 0, .id = 3});
        queue.append({.monitorIndex = 1, .id = 4});

        dropSupersededFrames(queue, 1);

        QCOMPARE(idsOf(queue), QList<int>({1, 3}));
    }

    // ... and the survivors keep their order, because each one is a P-frame
    // referencing the one before it.
    void survivorsKeepTheirOrder()
    {
        QQueue<QueuedFrame> queue;
        queue.append({.monitorIndex = 2, .id = 10});
        queue.append({.monitorIndex = 0, .id = 11});
        queue.append({.monitorIndex = 2, .id = 12});
        queue.append({.monitorIndex = 1, .id = 13});
        queue.append({.monitorIndex = 2, .id = 14});

        dropSupersededFrames(queue, 0);

        QCOMPARE(idsOf(queue), QList<int>({10, 12, 13, 14}));
    }

    // A keyframe for a monitor with nothing queued drops nothing at all.
    void keyframeForAnIdleMonitorDropsNothing()
    {
        QQueue<QueuedFrame> queue;
        queue.append({.monitorIndex = 0, .id = 1});
        queue.append({.monitorIndex = 1, .id = 2});

        dropSupersededFrames(queue, 2);

        QCOMPARE(idsOf(queue), QList<int>({1, 2}));
    }

    void emptyQueueIsLeftAlone()
    {
        QQueue<QueuedFrame> queue;
        dropSupersededFrames(queue, 0);
        QVERIFY(queue.isEmpty());
    }

    void predicateMatchesOnlyTheSameIndex()
    {
        QVERIFY(supersededByKeyframe(0, 0));
        QVERIFY(supersededByKeyframe(3, 3));
        QVERIFY(!supersededByKeyframe(0, 1));
        QVERIFY(!supersededByKeyframe(1, 0));
    }

    // AUD-P6: SUSPEND_FRAME_ACKNOWLEDGEMENT.
    void suspendedAcksStopTrackingPendingFrames()
    {
        using KRdp::FrameQueuePolicy::FrameAckTracker;
        FrameAckTracker tracker;
        for (uint32_t id = 1; id <= 5; ++id) {
            tracker.frameSent(id);
        }
        QCOMPARE(tracker.pending(), 5);
        QCOMPARE(tracker.acknowledge(1, 3), FrameAckTracker::Ack::Acknowledged);
        QCOMPARE(tracker.pending(), 4);
        QCOMPARE(tracker.acknowledge(1, 3), FrameAckTracker::Ack::Unknown);

        // The client suspends: nothing is outstanding, and later frames are
        // not tracked, so the backlog can never grow while it stays silent.
        QCOMPARE(tracker.acknowledge(2, FrameAckTracker::SuspendFrameAcknowledgement), FrameAckTracker::Ack::Suspended);
        QVERIFY(tracker.suspended());
        QCOMPARE(tracker.pending(), 0);
        for (uint32_t id = 6; id <= 1000; ++id) {
            tracker.frameSent(id);
        }
        QCOMPARE(tracker.pending(), 0);
        QCOMPARE(tracker.acknowledge(7, FrameAckTracker::SuspendFrameAcknowledgement), FrameAckTracker::Ack::Unknown);

        // An ordinary ack resumes tracking.
        QCOMPARE(tracker.acknowledge(1000, 0), FrameAckTracker::Ack::Resumed);
        QVERIFY(!tracker.suspended());
        tracker.frameSent(1001);
        QCOMPARE(tracker.pending(), 1);
        tracker.clear();
        QCOMPARE(tracker.pending(), 0);
    }
};

QTEST_GUILESS_MAIN(FrameQueuePolicyTest)

#include "FrameQueuePolicyTest.moc"
