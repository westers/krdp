// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QQueue>
#include <QDebug>

#include <algorithm>
#include <chrono>
#include <deque>
#include <vector>
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

/**
 * The submission loop of VideoStream over the pure policy, in 1 ms steps. One monitor; the
 * encoder makes a frame every 1000 / rate ms (30 fps, or less while the AUD-FIX7 delivery
 * throttle holds it back); the first frame is a keyframe of \a keyFrameBytes, and so is every
 * \a gop-th one (0: none) and the answer to a request; the rest are P-frames of \a deltaBytes.
 * The link carries \a linkBytesPerSecond (0: unlimited) and the client acknowledges a frame
 * \a keyFrameAck (keyframes) or \a deltaAck after it arrived. \a fixed = the current rules;
 * false = the AUD-FIX4 ones (a window of four from the minimum RTT, a 128 KiB floor, four held
 * frames, a request a second), to show the loops they made.
 */
struct FlowRun {
    int keyFramesSent = 0;
    int requests = 0;
    int dropped = 0;
    int keyFrameAckedAtMs = -1;
    int maxInFlight = 0;
    int64_t maxInFlightBytes = 0;
    int minFrameRate = 30;
    int framesMade = 0;
    std::vector<int> delivered; // frame ids in send order
    std::vector<int> deliveredAtMs; // when they were sent
    std::vector<int> madeAtMs; // when they were made, by id
    std::vector<int> ackedAtMs; // acknowledgement times, in order
    /** Frames acknowledged in [fromMs, toMs), per second. */
    double ackRate(int fromMs, int toMs) const
    {
        const auto n = std::count_if(ackedAtMs.cbegin(), ackedAtMs.cend(), [&](int t) {
            return t >= fromMs && t < toMs;
        });
        return double(n) * 1000.0 / double(toMs - fromMs);
    }
};

struct FlowSetup {
    int64_t keyFrameBytes = 48000;
    int64_t deltaBytes = 12000;
    std::chrono::milliseconds keyFrameAck{20};
    std::chrono::milliseconds deltaAck{20};
    std::chrono::milliseconds duration{10000};
    std::chrono::milliseconds stallEvery{};
    std::chrono::milliseconds stallFor{};
    int gop = 0;
    int64_t linkBytesPerSecond = 0;
};

FlowRun simulateFlow(bool fixed, const FlowSetup &setup)
{
    using namespace KRdp::FrameQueuePolicy;
    using namespace std::chrono;
    using Clock = FrameAckTracker::Clock;
    struct Frame {
        int monitorIndex = 0;
        int id = 0;
        bool key = false;
        int64_t bytes = 0;
    };
    const auto t0 = Clock::time_point(hours(1));
    FlowRun run;
    FrameAckTracker tracker;
    KeyFramePeak peak;
    KeyFrameRequestBackoff backoff;
    DeliveryThrottle throttle;
    Clock::time_point lastLegacyRequest{};
    QQueue<Frame> queue;
    std::vector<std::pair<Clock::time_point, uint32_t>> acks;
    bool starved = false;
    bool keyRequested = false;
    int nextId = 0;
    uint32_t frameId = 0;
    const int cap = 30;
    int rate = cap;
    milliseconds nextFrameAt(0);
    int sinceKey = 0;
    int64_t averageDelta = 0;
    // The link: bytes still to deliver, and when the last byte sent so far arrives.
    milliseconds linkFreeAt(0);
    std::deque<std::pair<milliseconds, int64_t>> onLink; // (arrives at, bytes)
    int64_t onLinkBytes = 0;
    // One adaptive interval: held time and coalesced frames.
    milliseconds heldThisInterval(0);
    int coalescedThisInterval = 0;
    int ackedThisInterval = 0;
    int madeThisInterval = 0;
    for (milliseconds t(0); t < setup.duration; ++t) {
        const auto now = t0 + t;
        while (!onLink.empty() && onLink.front().first <= t) {
            onLinkBytes -= onLink.front().second;
            onLink.pop_front();
        }
        // The encoder.
        if (t >= nextFrameAt) {
            nextFrameAt = t + milliseconds(1000 / std::max(1, rate));
            const bool gopKey = setup.gop > 0 && sinceKey + 1 >= setup.gop;
            Frame frame{.monitorIndex = 0, .id = nextId++, .key = nextId == 1 || keyRequested || gopKey, .bytes = 0};
            frame.bytes = frame.key ? setup.keyFrameBytes : setup.deltaBytes;
            sinceKey = frame.key ? 0 : sinceKey + 1;
            keyRequested = keyRequested && !frame.key;
            run.madeAtMs.push_back(int(t.count()));
            ++run.framesMade;
            ++madeThisInterval;
            if (fixed && frame.key) {
                peak.record(frame.bytes, now);
            }
            // queueFrame()
            if (starved && !frame.key) {
                ++run.dropped;
            } else {
                starved = false;
                if (frame.key) {
                    dropSupersededFrames(queue, 0);
                }
                queue.enqueue(frame);
            }
        }
        // The client's acknowledgements.
        for (auto it = acks.begin(); it != acks.end();) {
            if (it->first <= now) {
                tracker.acknowledge(it->second, 1, now);
                if (tracker.acknowledgedKeyFrame() == 0 && run.keyFrameAckedAtMs < 0) {
                    run.keyFrameAckedAtMs = int(t.count());
                }
                run.ackedAtMs.push_back(int(t.count()));
                ++ackedThisInterval;
                it = acks.erase(it);
            } else {
                ++it;
            }
        }
        // updateAdaptiveQuality(): the delivery throttle, every 1.5 s.
        if (fixed && t.count() > 0 && t.count() % 1500 == 0) {
            const bool pressure = heldThisInterval >= milliseconds(200) || coalescedThisInterval > 0;
            rate = throttle.update(now, cap, madeThisInterval / 1.5, ackedThisInterval / 1.5, pressure);
            run.minFrameRate = std::min(run.minFrameRate, rate);
            heldThisInterval = milliseconds(0);
            coalescedThisInterval = 0;
            ackedThisInterval = 0;
            madeThisInterval = 0;
        }
        const auto limitsNow = [&] {
            if (!fixed) {
                return windowLimits(cap, 1, microseconds(0), 0, 0);
            }
            const bool clear = linkClear(setup.linkBytesPerSecond > 0 ? onLinkBytes : 0);
            return windowLimits(rate, 1, microseconds(0), 0, peak.largest(now), tracker.ackLatency(now), averageDelta, clear);
        };
        // The submission thread.
        const auto requestIfStarved = [&] {
            if (!starved) {
                return;
            }
            bool request = false;
            if (fixed) {
                tracker.expire(now, limitsNow());
                request = backoff.shouldRequest(now, tracker.keyFrameInFlight(0));
            } else if (lastLegacyRequest == Clock::time_point{} || now - lastLegacyRequest >= CoalesceKeyFrameMinInterval) {
                lastLegacyRequest = now;
                request = true;
            }
            if (request) {
                ++run.requests;
                keyRequested = true;
            }
        };
        requestIfStarved();
        while (!queue.isEmpty()) {
            const auto limits = limitsNow();
            tracker.expire(now, limits);
            if (!tracker.canSend(limits)) {
                ++heldThisInterval;
                std::vector<int> starvedMonitors;
                int dropped = 0;
                if (fixed) {
                    dropped = coalesceHeldFrames(
                        queue,
                        [&](int, int count) {
                            return shouldCoalesce(count, keyFrameHoldFrames(rate));
                        },
                        starvedMonitors);
                } else {
                    dropped = coalesceHeldFrames(queue, MaxHeldFramesPerMonitor, starvedMonitors);
                }
                run.dropped += dropped;
                coalescedThisInterval += dropped;
                if (!starvedMonitors.empty()) {
                    starved = true;
                    backoff.dropped(now);
                }
                requestIfStarved();
                break;
            }
            const Frame frame = queue.dequeue();
            tracker.frameSent(frameId, frame.bytes, now, frame.key, frame.monitorIndex);
            if (!frame.key) {
                averageDelta = averageDelta == 0 ? frame.bytes : averageDelta + (frame.bytes - averageDelta) / 16;
            }
            run.maxInFlight = std::max(run.maxInFlight, tracker.inFlightFrames());
            run.maxInFlightBytes = std::max(run.maxInFlightBytes, tracker.inFlightBytes());
            // On the link, then the client's own delay.
            milliseconds arrives = t;
            if (setup.linkBytesPerSecond > 0) {
                linkFreeAt = std::max(linkFreeAt, t) + milliseconds(frame.bytes * 1000 / setup.linkBytesPerSecond);
                arrives = linkFreeAt;
                onLink.push_back({arrives, frame.bytes});
                onLinkBytes += frame.bytes;
            }
            // A client that pauses for stallFor every stallEvery acknowledges nothing meanwhile.
            auto ackAt = arrives + (frame.key ? setup.keyFrameAck : setup.deltaAck);
            if (setup.stallEvery.count() > 0 && ackAt % setup.stallEvery < setup.stallFor) {
                ackAt += setup.stallFor - ackAt % setup.stallEvery;
            }
            acks.push_back({t0 + ackAt, frameId});
            ++frameId;
            run.keyFramesSent += frame.key ? 1 : 0;
            run.delivered.push_back(frame.id);
            run.deliveredAtMs.push_back(int(t.count()));
        }
    }
    return run;
}

FlowRun simulateFlow(bool fixed, int64_t keyFrameBytes, int64_t deltaBytes, std::chrono::milliseconds keyFrameAck, std::chrono::milliseconds deltaAck,
                     std::chrono::milliseconds duration, std::chrono::milliseconds stallEvery = {}, std::chrono::milliseconds stallFor = {})
{
    return simulateFlow(fixed,
                        FlowSetup{.keyFrameBytes = keyFrameBytes,
                                  .deltaBytes = deltaBytes,
                                  .keyFrameAck = keyFrameAck,
                                  .deltaAck = deltaAck,
                                  .duration = duration,
                                  .stallEvery = stallEvery,
                                  .stallFor = stallFor});
}

/// How long after it was made the last frame sent went out.
int lastFrameLatencyMs(const FlowRun &run)
{
    return run.deliveredAtMs.back() - run.madeAtMs[size_t(run.delivered.back())];
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

    // AUD-FIX6 F2: the budget is at least twice the largest recent keyframe.
    void byteBudgetCoversKeyFrames()
    {
        using namespace KRdp::FrameQueuePolicy;
        using namespace std::chrono_literals;
        QCOMPARE(windowLimits(30, 1, 0us, 0, 0).bytes, MinInFlightBytes);
        QCOMPARE(windowLimits(30, 1, 0us, 0, 50000).bytes, MinInFlightBytes);
        QCOMPARE(windowLimits(30, 1, 0us, 0, 265000).bytes, int64_t(530000));
        QCOMPARE(windowLimits(30, 1, 0us, 0, 1 << 20).bytes, int64_t(2 << 20));
        // A measured rate larger than that still wins.
        QCOMPARE(windowLimits(30, 1, 20000us, 100000, 265000).bytes, int64_t(100000 * 125 * 0.27));

        const auto t0 = KeyFramePeak::Clock::time_point(1h);
        KeyFramePeak peak;
        QCOMPARE(peak.largest(t0), int64_t(0));
        peak.record(265000, t0);
        peak.record(40000, t0 + 1s);
        QCOMPARE(peak.largest(t0 + 2s), int64_t(265000));
        peak.record(300000, t0 + 3s); // the current one is the largest
        QCOMPARE(peak.largest(t0 + 3s), int64_t(300000));
        QCOMPARE(peak.largest(t0 + 3s + KeyFrameSizeHorizon - 1ms), int64_t(300000));
        QCOMPARE(peak.largest(t0 + 3s + KeyFrameSizeHorizon), int64_t(0));

        // AUD-FIX7 F2: coalescing (which costs a keyframe) only beyond the hold, the latency
        // bound; below it frames wait and the source is throttled instead.
        QVERIFY(!shouldCoalesce(MaxHeldFramesPerMonitor, 60));
        QVERIFY(!shouldCoalesce(5, 60));
        QVERIFY(!shouldCoalesce(60, 60));
        QVERIFY(shouldCoalesce(61, 60));
        QVERIFY(shouldCoalesce(MaxHeldFramesPerMonitor + 1, 1)); // never fewer than MaxHeldFramesPerMonitor held

        QCOMPARE(keyFrameHoldFrames(30), 60);
        QCOMPARE(keyFrameHoldFrames(1), MaxHeldFramesPerMonitor);
        QCOMPARE(keyFrameHoldFrames(1000), MaxKeyFrameHoldFrames);
    }

    // AUD-FIX6 F2: the tracker knows which monitor still has a keyframe out.
    void trackerKnowsKeyFramesInFlight()
    {
        using namespace KRdp::FrameQueuePolicy;
        using namespace std::chrono_literals;
        const auto t0 = FrameAckTracker::Clock::time_point(1h);
        const WindowLimits limits;
        FrameAckTracker tracker;
        tracker.frameSent(1, 265000, t0, true, 0);
        tracker.frameSent(2, 5000, t0, false, 0);
        tracker.frameSent(3, 90000, t0, true, 1);
        QVERIFY(tracker.keyFrameInFlight(0));
        QVERIFY(tracker.keyFrameInFlight(1));
        QVERIFY(!tracker.keyFrameInFlight(2));
        tracker.acknowledge(2, 1);
        QCOMPARE(tracker.acknowledgedKeyFrame(), -1);
        tracker.acknowledge(1, 1);
        QCOMPARE(tracker.acknowledgedKeyFrame(), 0);
        QVERIFY(!tracker.keyFrameInFlight(0));
        tracker.acknowledge(1, 1); // again: unknown
        QCOMPARE(tracker.acknowledgedKeyFrame(), -1);
        // One never acknowledged stops counting after AckTimeout.
        tracker.expire(t0 + AckTimeout, limits);
        QVERIFY(!tracker.keyFrameInFlight(1));
    }

    // AUD-FIX6 F2: keyframe requests after a drop wait for the keyframe in flight and back off
    // 1 s, 2 s, 4 s ... up to the cap. AUD-FIX7 F2: an acknowledged keyframe does not reset
    // that; BackoffQuietReset without a drop does.
    void keyFrameRequestsBackOff()
    {
        using namespace KRdp::FrameQueuePolicy;
        using namespace std::chrono_literals;
        const auto t0 = KeyFrameRequestBackoff::Clock::time_point(1h);
        KeyFrameRequestBackoff backoff;
        QVERIFY(!backoff.shouldRequest(t0, true)); // a keyframe is still out
        QVERIFY(backoff.shouldRequest(t0, false));
        auto last = t0;
        for (const auto gap : {1000ms, 2000ms, 4000ms, 8000ms, 16000ms, 16000ms}) {
            QVERIFY(!backoff.shouldRequest(last + gap - 1ms, false));
            QVERIFY(backoff.shouldRequest(last + gap, false));
            last += gap;
        }
        QVERIFY(!backoff.shouldRequest(last + 1h, true));
        // Drops that keep coming keep the back-off at the cap.
        backoff.dropped(last + 1s);
        backoff.dropped(last + 9s);
        QCOMPARE(backoff.interval(), std::chrono::milliseconds(16000));
        QVERIFY(!backoff.shouldRequest(last + 15s, false));
        // The first drop after 10 s without one starts it over.
        backoff.dropped(last + 19s);
        QCOMPARE(backoff.interval(), std::chrono::milliseconds(1000));
        QVERIFY(backoff.shouldRequest(last + 19s, false));
        QVERIFY(!backoff.shouldRequest(last + 19s + 999ms, false));
        QVERIFY(backoff.shouldRequest(last + 20s, false));
    }

    // AUD-FIX7 F2: the window follows the frame-ack latency, and a clear link lets the byte
    // budget follow it; a congested one keeps the link's budget.
    void windowFollowsAckLatency()
    {
        using namespace KRdp::FrameQueuePolicy;
        using namespace std::chrono_literals;
        // Sol's stock client on :3391: 285 ms from send to ack on a LAN; 30 fps x 0.345 s = 11.
        auto limits = windowLimits(30, 1, 1000us, 0, 130000, 285000us, 65536, true);
        QCOMPARE(limits.frames, 11);
        QCOMPARE(limits.bytes, int64_t(11 * 65536 * 1.25) + 130000);
        // The same ack latency behind a congested link is the link's queue: neither follows it.
        limits = windowLimits(30, 1, 1000us, 0, 130000, 285000us, 65536, false);
        QCOMPARE(limits.frames, MinInFlightFrames);
        QCOMPARE(limits.bytes, int64_t(260000));
        // An ack latency under the RTT changes nothing; the ceiling holds.
        QCOMPARE(windowLimits(60, 1, 50000us, 0, 0, 10000us, 65536, true), windowLimits(60, 1, 50000us, 0));
        QCOMPARE(windowLimits(60, 1, 1000us, 0, 0, 2s, 1000, true).frames, MaxInFlightFrames);
        QCOMPARE(windowLimits(30, 2, 1000us, 0, 0, 2s, 1000, true).frames, 2 * MaxInFlightFrames);

        QVERIFY(linkClear(0));
        QVERIFY(linkClear(LinkClearBytes - 1));
        QVERIFY(!linkClear(LinkClearBytes));
        QVERIFY(!linkClear(-1)); // unknown

        // Frames a steady client is behind by nature: 285 ms at 30 fps -> 9 (+1).
        QCOMPARE(backlogFrames(30, 1, 285000us, 4), 10);
        QCOMPARE(backlogFrames(60, 1, 20000us, 4), 4);
        QCOMPARE(backlogFrames(30, 1, 0us, 4), 4);

        // The tracker's view: p75 of the last 2 s, floor of the last 10 s.
        const auto t0 = FrameAckTracker::Clock::time_point(1h);
        FrameAckTracker tracker;
        QCOMPARE(tracker.ackLatency(t0), 0us);
        for (uint32_t id = 0; id < 40; ++id) {
            const auto sent = t0 + id * 33ms;
            tracker.frameSent(id, 1000, sent);
            tracker.acknowledge(id, 1, sent + (id % 4 == 3 ? 400ms : 280ms));
        }
        const auto end = t0 + 39 * 33ms + 400ms;
        QCOMPARE(tracker.ackLatency(end), 400000us); // one in four is slow: the p75
        QCOMPARE(tracker.baseAckLatency(end), 280000us);
        // Nothing acked lately: the last eight of the horizon (ids 32..39: two of them slow).
        QCOMPARE(tracker.ackLatency(end + AckLatencyRecent), 400000us);
        QCOMPARE(tracker.ackLatency(end + AckLatencyHorizon), 0us); // nothing left: unknown
        QCOMPARE(tracker.baseAckLatency(end + AckLatencyHorizon), 0us);
    }

    // AUD-FIX7 F2: the delivery throttle.
    void deliveryThrottleFollowsTheClient()
    {
        using namespace KRdp::FrameQueuePolicy;
        using namespace std::chrono_literals;
        const auto t0 = DeliveryThrottle::Clock::time_point(1h);
        DeliveryThrottle throttle;
        QCOMPARE(throttle.update(t0, 30, 30, 30, false), 30);
        QVERIFY(!throttle.active());
        // A quiet desktop whose keyframe waited: it offered no more than got through.
        QCOMPARE(throttle.update(t0 + 1500ms, 30, 2, 2, true), 30);
        QVERIFY(!throttle.active());
        // Behind a full window, 30 made and 14 taken: ask for 12.
        QCOMPARE(throttle.update(t0 + 3s, 30, 30, 14, true), 12);
        QVERIFY(throttle.active());
        // Still behind: 12 made, 10 taken -> 9.
        QCOMPARE(throttle.update(t0 + 4500ms, 30, 12, 10, true), 9);
        // No acks at all (a stall): no change.
        QCOMPARE(throttle.update(t0 + 6s, 30, 9, 0, true), 9);
        // Never below the floor, never above the cap.
        QCOMPARE(throttle.update(t0 + 7500ms, 30, 9, 2, true), MinThrottledFrameRate);
        QCOMPARE(throttle.rate(3), 3);
        // Quiet for ThrottleRaiseAfter: up by half at a time, then off.
        QCOMPARE(throttle.update(t0 + 9s, 30, 5, 5, false), MinThrottledFrameRate);
        QCOMPARE(throttle.update(t0 + 10500ms, 30, 5, 5, false), 8);
        QCOMPARE(throttle.update(t0 + 12s, 30, 8, 8, false), 8);
        QCOMPARE(throttle.update(t0 + 13500ms, 30, 8, 8, false), 12);
        // Pressure right after a raise doubles the wait before the next one.
        QCOMPARE(throttle.update(t0 + 15s, 30, 12, 10, true), 9);
        QCOMPARE(throttle.raiseWait(), std::chrono::milliseconds(6000));
        QCOMPARE(throttle.update(t0 + 19500ms, 30, 9, 9, false), 9);
        QCOMPARE(throttle.update(t0 + 21s, 30, 9, 9, false), 14);
        for (int i = 0; i < 8; ++i) {
            throttle.update(t0 + 21s + (i + 1) * 7s, 30, 30, 30, false);
        }
        QVERIFY(!throttle.active());
        QCOMPARE(throttle.rate(30), 30);
        // A minute without pressure: the raise wait is back to its start.
        throttle.update(t0 + 200s, 30, 30, 30, false);
        QCOMPARE(throttle.raiseWait(), std::chrono::milliseconds(3000));
    }

    // AUD-FIX7 F2, the Sol :3391 case (2026-09-28): 30 fps of 64 KiB P-frames, a 130 KB IDR
    // every 100 frames, a client that acknowledges 285 ms after a frame arrives, on a link with
    // plenty of room. The AUD-FIX4 window of four capped it at 14 fps and rekeyed about once a
    // second; now the window covers the client's delay: the full rate, no rekey.
    void slowAckingClientKeepsFullRate()
    {
        using namespace std::chrono_literals;
        const FlowSetup sol{.keyFrameBytes = 130000,
                            .deltaBytes = 65536,
                            .keyFrameAck = 285ms,
                            .deltaAck = 285ms,
                            .duration = 60s,
                            .gop = 100};
        const auto before = simulateFlow(false, sol);
        qInfo() << "AUD-FIX4 rules:" << before.ackRate(10000, 60000) << "fps," << before.requests << "requests in 60 s," << before.dropped << "dropped";
        QVERIFY(before.requests >= 30); // the loop this fixes
        QVERIFY(before.ackRate(10000, 60000) < 20);

        const auto run = simulateFlow(true, sol);
        qInfo() << "current rules:" << run.ackRate(10000, 60000) << "fps," << run.requests << "requests in 60 s," << run.dropped << "dropped, max"
                << run.maxInFlight << "in flight, lowest frame rate" << run.minFrameRate;
        QVERIFY(run.requests <= 1); // at most one rekey a minute
        QCOMPARE(run.dropped, 0);
        QVERIFY(run.ackRate(10000, 60000) >= 29.0);
        QVERIFY(run.maxInFlight <= KRdp::FrameQueuePolicy::MaxInFlightFrames);
        QVERIFY(lastFrameLatencyMs(run) <= 66);
    }

    // AUD-FIX7 F2: a link that carries a sixth of the stream (0.5 Mbit/s for 2.9). The window's
    // bytes stay the link's (it is not clear), the source slows to what gets through instead of
    // frames being dropped after encoding, and the latency stays bounded.
    void throttledLinkSlowsTheSource()
    {
        using namespace std::chrono_literals;
        const FlowSetup thin{.keyFrameBytes = 48000, .deltaBytes = 12000, .keyFrameAck = 20ms, .deltaAck = 20ms, .duration = 30s, .gop = 100,
                             .linkBytesPerSecond = 64 * 1024};
        const auto before = simulateFlow(false, thin);
        qInfo() << "AUD-FIX4 rules:" << before.requests << "requests," << before.dropped << "dropped," << before.ackRate(10000, 30000) << "fps";
        const auto run = simulateFlow(true, thin);
        qInfo() << "current rules:" << run.requests << "requests," << run.dropped << "dropped," << run.ackRate(10000, 30000) << "fps, lowest frame rate"
                << run.minFrameRate << ", max" << run.maxInFlightBytes << "bytes in flight, last frame" << lastFrameLatencyMs(run) << "ms old";
        QVERIFY(before.requests >= 10);
        QVERIFY(run.requests <= 2);
        QVERIFY(run.minFrameRate <= 8); // the source followed the link
        QVERIFY(run.ackRate(10000, 30000) >= 3.5); // and what it makes gets through
        // The link's budget (the floor, plus the frame that crossed it), not the frame window's.
        QVERIFY(run.maxInFlightBytes <= KRdp::FrameQueuePolicy::MinInFlightBytes + 48000);
        QVERIFY(lastFrameLatencyMs(run) <= 1500);
    }

    // AUD-FIX6 F2, the Sol :3391 case: a 265 KB keyframe (over the 128 KiB floor) the client
    // takes 400 ms to acknowledge, 6 kB P-frames at 30 fps. Before, the P-frames behind it were
    // dropped and another 265 KB keyframe requested, once a second; now they wait, go out after
    // the ack, and no keyframe is ever requested.
    void keyFrameOverBudgetDoesNotLoop()
    {
        using namespace std::chrono_literals;
        const auto before = simulateFlow(false, 265000, 6000, 400ms, 20ms, 10s);
        qInfo() << "AUD-FIX4 rules:" << before.keyFramesSent << "keyframes," << before.requests << "requests," << before.dropped << "dropped,"
                << before.delivered.size() << "sent";
        QVERIFY(before.requests >= 5); // the loop this fixes
        QVERIFY(before.keyFramesSent >= 5);

        const auto run = simulateFlow(true, 265000, 6000, 400ms, 20ms, 10s);
        qInfo() << "current rules:" << run.keyFramesSent << "keyframes," << run.requests << "requests," << run.dropped << "dropped," << run.delivered.size()
                << "sent";
        QCOMPARE(run.keyFramesSent, 1);
        QCOMPARE(run.requests, 0);
        QCOMPARE(run.dropped, 0);
        QCOMPARE(run.keyFrameAckedAtMs, 400);
        // Every frame, in order: the deltas behind the keyframe were delivered, not replaced.
        QVERIFY(run.delivered.size() >= 295);
        for (size_t i = 0; i < run.delivered.size(); ++i) {
            QCOMPARE(run.delivered[i], int(i));
        }
        // Frames sent after the ack keep up with the encoder: the last one went out within
        // two frame intervals of being made.
        QVERIFY(lastFrameLatencyMs(run) <= 66);
    }

    // AUD-FIX6 F2: a 4K keyframe (1 MB) the client needs 1.5 s for: still one keyframe, no
    // request, and the deltas behind it follow once it is acknowledged.
    void hugeKeyFrameIsHeldBehind()
    {
        using namespace std::chrono_literals;
        const auto run = simulateFlow(true, 1 << 20, 20000, 1500ms, 20ms, 8s);
        QCOMPARE(run.keyFramesSent, 1);
        QCOMPARE(run.requests, 0);
        QCOMPARE(run.dropped, 0);
        for (size_t i = 0; i < run.delivered.size(); ++i) {
            QCOMPARE(run.delivered[i], int(i));
        }
        QVERIFY(lastFrameLatencyMs(run) <= 66);
    }

    // AUD-FIX6 F2: with a 1 MB keyframe out of the way, a client that pauses now and then
    // (busy: 300 ms without acks every 2 s) does not cost a 1 MB rekey each time.
    void ackJitterDoesNotRekeyABigKeyFrame()
    {
        using namespace std::chrono_literals;
        const auto run = simulateFlow(true, 1 << 20, 8000, 200ms, 20ms, 10s, 2s, 300ms);
        QCOMPARE(run.keyFramesSent, 1);
        QCOMPARE(run.requests, 0);
        QCOMPARE(run.dropped, 0);
        const auto before = simulateFlow(false, 1 << 20, 8000, 200ms, 20ms, 10s, 2s, 300ms);
        qInfo() << "AUD-FIX4 rules:" << before.keyFramesSent << "keyframes," << before.requests << "requests";
        QVERIFY(before.requests >= 3);
    }

    // AUD-FIX6 F2: a keyframe that is never acknowledged (a client that stopped acking) gets
    // the frames behind it coalesced after keyFrameHoldFrames(); the next request waits for
    // the ack timeout instead of queueing another keyframe behind it every second.
    void unacknowledgedKeyFrameDefersTheRequest()
    {
        using namespace KRdp::FrameQueuePolicy;
        using namespace std::chrono_literals;
        const auto run = simulateFlow(true, 265000, 6000, 1h, 1h, 15s);
        QVERIFY(run.dropped > 0);
        QCOMPARE(run.requests, 1); // after AckTimeout, not before
        QCOMPARE(run.keyFramesSent, 2);
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
