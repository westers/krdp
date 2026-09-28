// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX12: the encoder watchdog (EncoderWatchdog.h) and the broker's silent-surface rule
// (FrameQueuePolicy::SurfaceSilence), alone and together: no surface stays silent for more than
// 2 s while frames are being captured, under a forced coalesce, whatever the encoder does.

#include <QTest>

#include <optional>

#include "EncoderWatchdog.h"
#include "FrameQueuePolicy.h"

using namespace KRdp;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using EncoderWatchdog::Action;

namespace
{
const Clock::time_point T0 = Clock::time_point{} + 1h;

/**
 * A capture + encoder at 30 fps as PlasmaScreencastV1Session drives it: a keyframe request
 * makes the next picture (or, on a still picture, a re-encode of the last one) a keyframe; a
 * wedged encoder (ace: KPipeWire's filter graph stuck after a failed VA-API import) produces
 * nothing and answers no request until it is restarted, which takes RestartTakes and opens with
 * an IDR. Its session's EncoderWatchdog runs as in the session (poll every TickInterval).
 */
struct Encoder {
    static constexpr auto Interval = 33ms;
    static constexpr auto RestartTakes = 300ms;
    EncoderWatchdog::Watchdog watchdog;
    bool moving = true; ///< the picture changes (else KWin sends no frames)
    bool wedged = false;
    int wedgeAfter = -1; ///< wedge after this many more packets (-1: never)
    bool keyWanted = false;
    bool refeed = false; ///< a request on a still picture re-encodes the last one at once
    std::optional<Clock::time_point> restartDone;
    Clock::time_point nextFrame = T0;
    Clock::time_point nextPoll = T0;
    int restarts = 0;
    int requests = 0;
    int probes = 0;

    void request(Clock::time_point now)
    {
        ++requests;
        watchdog.keyFrameRequested(now);
        askEncoder();
    }
    void askEncoder()
    {
        if (wedged || restartDone) return; // ace: KPipeWire's queue is full, the request is lost
        keyWanted = true;
        if (!moving) refeed = true;
    }
    /// Packets (true = keyframe) produced at \a now.
    std::optional<bool> tick(Clock::time_point now)
    {
        if (now >= nextPoll) {
            nextPoll = now + EncoderWatchdog::Watchdog::TickInterval;
            switch (watchdog.poll(now)) {
            case Action::Probe:
                ++probes;
                askEncoder();
                break;
            case Action::Retry:
                askEncoder();
                break;
            case Action::Restart:
                ++restarts;
                restartDone = now + RestartTakes;
                break;
            case Action::GiveUp:
            case Action::None:
                break;
            }
        }
        if (restartDone && now >= *restartDone) {
            restartDone.reset();
            wedged = false;
            wedgeAfter = -1;
            keyWanted = true;
            refeed = true; // the fresh encoder opens with an IDR of the current picture
        }
        if (wedged || restartDone) return std::nullopt;
        const bool due = (moving && now >= nextFrame) || refeed;
        if (!due) return std::nullopt;
        nextFrame = now + Interval;
        refeed = false;
        const bool key = std::exchange(keyWanted, false);
        watchdog.packet(now, key);
        if (wedgeAfter > 0 && --wedgeAfter == 0) wedged = true;
        return key;
    }
};

/**
 * The broker side of one surface (VideoStream): after a coalesce the surface is starved (every
 * P-frame dropped until a keyframe arrives); requests go out when KeyFrameRequestBackoff allows,
 * or when SurfaceSilence says the surface has dropped everything for SilentSurfaceLimit.
 */
struct Broker {
    FrameQueuePolicy::KeyFrameRequestBackoff backoff;
    FrameQueuePolicy::SurfaceSilence silence;
    bool starved = false;
    bool keyFrameInFlight = false; ///< an unacknowledged keyframe holds requests back
    std::optional<Clock::time_point> lastDelivered;
    Clock::duration longestGap{};
    int delivered = 0;
    int forced = 0;

    void coalesce(Clock::time_point now)
    {
        starved = true;
        backoff.dropped(now);
    }
    void frame(Clock::time_point now, bool key)
    {
        if (starved && !key) {
            silence.dropped(now);
            return;
        }
        starved = false;
        silence.delivered();
        if (lastDelivered) longestGap = std::max(longestGap, now - *lastDelivered);
        lastDelivered = now;
        ++delivered;
    }
    /// Whether to ask the encoder for a keyframe at \a now.
    bool poll(Clock::time_point now)
    {
        bool request = starved && backoff.shouldRequest(now, keyFrameInFlight);
        if (silence.shouldForce(now)) {
            ++forced;
            request = true;
        }
        return request;
    }
};

/// Runs \a encoder and \a broker from \a from to \a to in 5 ms steps; \a at(now) may intervene.
template<typename F>
void run(Encoder &encoder, Broker &broker, Clock::time_point from, Clock::time_point to, F &&at)
{
    for (auto now = from; now < to; now += 5ms) {
        at(now);
        if (const auto packet = encoder.tick(now)) broker.frame(now, *packet);
        if (broker.poll(now)) encoder.request(now);
    }
    if (broker.lastDelivered) broker.longestGap = std::max(broker.longestGap, to - *broker.lastDelivered);
}
}

class EncoderWatchdogTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void unansweredRequestRetriesThenRestarts()
    {
        EncoderWatchdog::Watchdog w;
        w.packet(T0, true);
        w.keyFrameRequested(T0 + 1s);
        QCOMPARE(w.poll(T0 + 1s + EncoderWatchdog::RetryAfter - 50ms), Action::None);
        QCOMPARE(w.poll(T0 + 1s + EncoderWatchdog::RetryAfter), Action::Retry);
        QCOMPARE(w.poll(T0 + 1s + (EncoderWatchdog::RetryAfter + EncoderWatchdog::RestartAfter) / 2), Action::None);
        QCOMPARE(w.poll(T0 + 1s + EncoderWatchdog::RestartAfter), Action::Restart);
        // The restarted encoder has RestartAnswerWithin to produce a keyframe; then once more.
        const auto first = T0 + 1s + EncoderWatchdog::RestartAfter;
        QCOMPARE(w.poll(first + 2s), Action::None);
        QCOMPARE(w.poll(first + EncoderWatchdog::RestartAnswerWithin), Action::Restart);
        QCOMPARE(w.poll(first + EncoderWatchdog::RestartAnswerWithin * 2), Action::GiveUp);
        QVERIFY(!w.pending());
        QCOMPARE(w.poll(first + 20s), Action::None);
    }

    void answeredRequestNeedsNothing()
    {
        EncoderWatchdog::Watchdog w;
        w.keyFrameRequested(T0);
        w.packet(T0 + 50ms, true);
        QVERIFY(!w.pending());
        for (auto t = T0; t < T0 + 20s; t += 100ms) QCOMPARE(w.poll(t), Action::None);
        // A delta is no answer.
        w.keyFrameRequested(T0 + 30s);
        w.packet(T0 + 30s + 30ms, false);
        QCOMPARE(w.poll(T0 + 30s + EncoderWatchdog::RetryAfter), Action::Retry);
        w.packet(T0 + 30s + 500ms, true);
        QCOMPARE(w.poll(T0 + 30s + 2s), Action::None);
    }

    void silentMovingStreamIsProbedOnlyWhenArmed()
    {
        // Not armed: a stream that stops moving is a still picture, nothing to do.
        EncoderWatchdog::Watchdog quiet;
        for (int i = 0; i < 30; ++i) quiet.packet(T0 + i * 33ms, i == 0);
        for (auto t = T0 + 1s; t < T0 + 20s; t += 100ms) QCOMPARE(quiet.poll(t), Action::None);

        // Armed (a frame-rate change): a moving stream silent for SilenceMin is probed, once.
        EncoderWatchdog::Watchdog w;
        w.arm(T0);
        for (int i = 0; i < 30; ++i) w.packet(T0 + i * 33ms, false);
        const auto last = T0 + 29 * 33ms;
        QCOMPARE(w.silenceLimit(), Clock::duration(EncoderWatchdog::SilenceMin));
        QCOMPARE(w.poll(last + EncoderWatchdog::SilenceMin - 100ms), Action::None);
        QCOMPARE(w.poll(last + EncoderWatchdog::SilenceMin), Action::Probe);
        // Answered (a healthy encoder re-encodes the still picture): no further probe.
        w.packet(last + EncoderWatchdog::SilenceMin + 50ms, true);
        for (auto t = last + EncoderWatchdog::SilenceMin + 100ms; t < last + 10s; t += 100ms) QCOMPARE(w.poll(t), Action::None);
        // New motion, then silence again (still armed): probed again.
        for (int i = 0; i < 5; ++i) w.packet(last + 10s + i * 33ms, false);
        QCOMPARE(w.poll(last + 10s + 4 * 33ms + EncoderWatchdog::SilenceMin), Action::Probe);
        // After ArmFor nothing is probed.
        EncoderWatchdog::Watchdog late;
        late.arm(T0);
        for (int i = 0; i < 30; ++i) late.packet(T0 + EncoderWatchdog::ArmFor + i * 33ms, false);
        QCOMPARE(late.poll(T0 + EncoderWatchdog::ArmFor + 3s), Action::None);
    }

    void throttledStreamWaitsLonger()
    {
        // 2 fps (the delivery throttle's low end): four intervals, 2 s.
        EncoderWatchdog::Watchdog w;
        w.arm(T0);
        for (int i = 0; i < 4; ++i) w.packet(T0 + i * 500ms, false);
        QCOMPARE(std::chrono::duration_cast<std::chrono::milliseconds>(w.silenceLimit()), 2000ms);
        QCOMPARE(w.poll(T0 + 1500ms + 1900ms), Action::None);
        QCOMPARE(w.poll(T0 + 1500ms + 2000ms), Action::Probe);
    }

    void surfaceSilenceForcesAfterALimit()
    {
        FrameQueuePolicy::SurfaceSilence s;
        QVERIFY(!s.shouldForce(T0 + 10s));
        s.dropped(T0);
        s.dropped(T0 + 500ms);
        QVERIFY(!s.shouldForce(T0 + 900ms));
        QVERIFY(s.shouldForce(T0 + FrameQueuePolicy::SilentSurfaceLimit));
        QVERIFY(!s.shouldForce(T0 + 1500ms));
        QVERIFY(s.shouldForce(T0 + FrameQueuePolicy::SilentSurfaceLimit + FrameQueuePolicy::SilentSurfaceRetry));
        QCOMPARE(s.forced(), 2);
        s.delivered();
        QVERIFY(!s.shouldForce(T0 + 10s));
        QCOMPARE(s.forced(), 0);
    }

    // The watchdog test: forced coalesces, and no surface silent for more than 2 s.
    void noSurfaceSilentUnderForcedCoalesce_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::newRow("ace: keyframe and one delta, then the encoder wedges") << 0;
        QTest::newRow("the encoder wedges at the coalesce (request lost)") << 1;
        QTest::newRow("back-off at 16 s from earlier coalesces") << 2;
        QTest::newRow("keyframe in flight never acknowledged") << 3;
        QTest::newRow("coalesce every 3 s for a minute") << 4;
    }
    void noSurfaceSilentUnderForcedCoalesce()
    {
        QFETCH(int, scenario);
        Encoder encoder;
        Broker broker;
        const auto coalesceAt = T0 + 5s;
        if (scenario == 2) {
            // Earlier coalesces pushed the back-off to its maximum.
            for (int i = 0; i < 6; ++i) {
                broker.backoff.dropped(T0 + i * 100ms);
                QVERIFY(broker.backoff.shouldRequest(T0 + 4s + FrameQueuePolicy::CoalesceKeyFrameMaxInterval * i, false));
            }
            QVERIFY(broker.backoff.interval() >= 16s);
        }
        run(encoder, broker, T0, T0 + 65s, [&](Clock::time_point now) {
            const bool coalesce = scenario == 4 ? now >= coalesceAt && (now - coalesceAt) % 3s == 0ms && now < coalesceAt + 60s : now == coalesceAt;
            if (!coalesce) return;
            encoder.watchdog.arm(now); // the delivery throttle's frame-rate change goes with it
            broker.coalesce(now);
            if (scenario == 0) encoder.wedgeAfter = 2;
            if (scenario == 1) encoder.wedged = true;
            if (scenario == 3) broker.keyFrameInFlight = true;
        });
        qInfo() << "longest gap" << std::chrono::duration_cast<std::chrono::milliseconds>(broker.longestGap).count() << "ms; delivered" << broker.delivered
                << "; requests" << encoder.requests << "(forced" << broker.forced << "); probes" << encoder.probes << "; restarts" << encoder.restarts;
        QVERIFY2(broker.longestGap <= 2s, "a surface was silent for more than 2 s");
        QVERIFY(broker.delivered > (scenario == 4 ? 1200 : 1500)); // 30 fps for a minute, less what the coalesces cost
        if (scenario <= 1) QCOMPARE(encoder.restarts, 1);
        else QCOMPARE(encoder.restarts, 0);
    }

    // A still picture after a reconfiguration costs one keyframe, not one a second.
    void stillPictureCostsOneKeyFrame()
    {
        Encoder encoder;
        Broker broker;
        run(encoder, broker, T0, T0 + 60s, [&](Clock::time_point now) {
            if (now == T0 + 5s) encoder.watchdog.arm(now);
            if (now == T0 + 6s) encoder.moving = false;
        });
        QCOMPARE(encoder.probes, 1);
        QCOMPARE(encoder.restarts, 0);
        QCOMPARE(broker.forced, 0);
    }
};

QTEST_GUILESS_MAIN(EncoderWatchdogTest)

#include "EncoderWatchdogTest.moc"
