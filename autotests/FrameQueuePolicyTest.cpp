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

    // AUD-FIX4 D1: the in-flight window's size.
    void windowFollowsRateAndRtt()
    {
        using namespace KRdp::FrameQueuePolicy;
        using namespace std::chrono_literals;
        // A LAN at 60 fps: the floor.
        auto limits = windowLimits(60, 1, 1000us, 0);
        QCOMPARE(limits.frames, MinInFlightFrames);
        QCOMPARE(limits.bytes, MinInFlightBytes);
        // 30 fps over a 20 ms path: 30 x 0.08 = 2.4 -> the floor again.
        QCOMPARE(windowLimits(30, 1, 20000us, 0).frames, MinInFlightFrames);
        // 60 fps over 50 ms: 60 x 0.11 = 6.6 -> 7.
        QCOMPARE(windowLimits(60, 1, 50000us, 0).frames, 7);
        // Never more than the ceiling, however long the path.
        QCOMPARE(windowLimits(60, 1, 500000us, 0).frames, MaxInFlightFrames);
        // Per surface: two monitors get twice the window.
        QCOMPARE(windowLimits(60, 2, 1000us, 0).frames, 2 * MinInFlightFrames);
        // Bytes: 6 Mbit/s over 20 ms = 750 kB/s x 0.27 s = 202 kB.
        limits = windowLimits(30, 1, 20000us, 6000);
        QCOMPARE(limits.bytes, int64_t(6000 * 125 * 0.27));
        QCOMPARE(limits.suspendedLifetime, SuspendedFrameLifetime + 20000us);
    }

    // AUD-FIX4 D1: never more than the window in flight, by count or by bytes; a big keyframe
    // still goes out alone.
    void trackerCapsInFlightFrames()
    {
        using namespace KRdp::FrameQueuePolicy;
        using namespace std::chrono_literals;
        const auto t0 = FrameAckTracker::Clock::time_point(1h);
        const WindowLimits limits{.frames = 4, .bytes = 100000, .suspendedLifetime = 250ms};
        FrameAckTracker tracker;
        uint32_t id = 0;
        int maxInFlight = 0;
        int held = 0;
        // 300 frames arrive at 30 fps; the client acks one frame every 100 ms (a link a third
        // as fast as the stream). Whatever cannot go is held (and coalesced by VideoStream).
        uint32_t nextAck = 0;
        for (int tick = 0; tick < 300; ++tick) {
            const auto now = t0 + tick * 33ms;
            if (tick % 3 == 0 && nextAck < id) {
                QCOMPARE(tracker.acknowledge(nextAck++, 1), FrameAckTracker::Ack::Acknowledged);
            }
            tracker.expire(now, limits);
            if (tracker.canSend(limits)) {
                tracker.frameSent(id++, 10000, now);
            } else {
                ++held;
            }
            maxInFlight = std::max(maxInFlight, tracker.inFlightFrames());
            QVERIFY(tracker.inFlightFrames() <= limits.frames);
        }
        QCOMPARE(maxInFlight, limits.frames);
        QVERIFY(held > 150); // two thirds could not go
        QVERIFY(id >= 100);

        // Bytes: two 60 kB frames fill the 100 kB budget before the count does.
        FrameAckTracker bytes;
        QVERIFY(bytes.canSend(limits));
        bytes.frameSent(1, 60000, t0);
        QVERIFY(bytes.canSend(limits));
        bytes.frameSent(2, 60000, t0);
        QVERIFY(!bytes.canSend(limits));
        QCOMPARE(bytes.inFlightBytes(), int64_t(120000));
        bytes.acknowledge(1, 0);
        bytes.acknowledge(2, 0);
        // A 500 kB keyframe is over the budget on its own, and goes when nothing else is out.
        QVERIFY(bytes.canSend(limits));
        bytes.frameSent(3, 500000, t0);
        QVERIFY(!bytes.canSend(limits));
        QCOMPARE(bytes.acknowledge(3, 0), FrameAckTracker::Ack::Acknowledged);
        QCOMPARE(bytes.inFlightBytes(), int64_t(0));
    }

    // AUD-FIX4 D1: a client that suspended acks gets a time budget (and the socket's queue);
    // one that silently stopped acking is not waited on forever.
    void suspendedAndSilentClientsAreBudgeted()
    {
        using namespace KRdp::FrameQueuePolicy;
        using namespace std::chrono_literals;
        const auto t0 = FrameAckTracker::Clock::time_point(1h);
        const WindowLimits limits{.frames = 4, .bytes = 100000, .suspendedLifetime = 250ms};
        FrameAckTracker tracker;
        tracker.frameSent(1, 1000, t0);
        QCOMPARE(tracker.acknowledge(1, FrameAckTracker::SuspendFrameAcknowledgement), FrameAckTracker::Ack::Suspended);
        // Four frames within 250 ms fill the window; it opens again as they age out.
        for (uint32_t id = 2; id <= 5; ++id) {
            QVERIFY(tracker.canSend(limits));
            tracker.frameSent(id, 1000, t0 + (id - 2) * 10ms);
        }
        QCOMPARE(tracker.pending(), 0); // adaptive quality's view is unchanged (AUD-P6)
        QCOMPARE(tracker.inFlightFrames(), 4);
        QVERIFY(!tracker.canSend(limits));
        tracker.expire(t0 + 200ms, limits);
        QVERIFY(!tracker.canSend(limits));
        tracker.expire(t0 + 255ms, limits);
        QCOMPARE(tracker.inFlightFrames(), 3);
        QVERIFY(tracker.canSend(limits));
        // A full socket holds the stream even with the time budget free.
        QVERIFY(!tracker.canSend(limits, 150000));
        QVERIFY(tracker.canSend(limits, 1000));
        // Acked mode ignores the socket (acks are the signal there).
        QCOMPARE(tracker.acknowledge(9, 0), FrameAckTracker::Ack::Resumed);
        QCOMPARE(tracker.inFlightFrames(), 0);
        QVERIFY(tracker.canSend(limits, 150000));

        // Silent client: four frames never acked; after AckTimeout they stop counting.
        FrameAckTracker silent;
        for (uint32_t id = 1; id <= 4; ++id) {
            silent.frameSent(id, 1000, t0);
        }
        QVERIFY(!silent.canSend(limits));
        QCOMPARE(silent.expire(t0 + AckTimeout - 1ms, limits), 0);
        QVERIFY(!silent.canSend(limits));
        QCOMPARE(silent.expire(t0 + AckTimeout, limits), 4);
        QVERIFY(silent.canSend(limits));
        QCOMPARE(silent.acknowledge(2, 0), FrameAckTracker::Ack::Unknown);
    }

    // AUD-FIX4 D1: while the window is full a monitor keeps at most MaxHeldFramesPerMonitor
    // queued; beyond that its whole chain goes (a P-frame is useless without its predecessor)
    // and it is marked starved; other monitors are untouched.
    void heldFramesAreCoalescedPerMonitor()
    {
        using namespace KRdp::FrameQueuePolicy;
        QQueue<QueuedFrame> queue;
        int id = 0;
        for (int i = 0; i < MaxHeldFramesPerMonitor; ++i) {
            queue.enqueue({0, ++id});
            queue.enqueue({1, ++id});
        }
        std::vector<int> starved;
        QCOMPARE(coalesceHeldFrames(queue, MaxHeldFramesPerMonitor, starved), 0);
        QVERIFY(starved.empty());
        QCOMPARE(queue.size(), 2 * MaxHeldFramesPerMonitor);
        queue.enqueue({0, ++id}); // monitor 0 is one over
        QCOMPARE(coalesceHeldFrames(queue, MaxHeldFramesPerMonitor, starved), MaxHeldFramesPerMonitor + 1);
        QCOMPARE(starved, std::vector<int>{0});
        QCOMPARE(queue.size(), MaxHeldFramesPerMonitor);
        for (const auto &frame : queue) {
            QCOMPARE(frame.monitorIndex, 1);
        }
    }
};

QTEST_GUILESS_MAIN(FrameQueuePolicyTest)

#include "FrameQueuePolicyTest.moc"
