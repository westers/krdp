// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 1: FileTransferPacer driven by a fake clock: idle to full speed, pressure halves per tick, the floor never reaches zero,
// recovery in about 2 s, window and slice derivation, unknown-capacity start, cap/ceiling precedence, the head-of-line bound.

#include <QTest>

#include <algorithm>
#include <deque>

#include "ClipboardFilePacing.h"

using namespace KRdp::ClipboardFilePacing;
using namespace std::chrono_literals;
namespace Limits = KRdp::ClipboardFileLimits;

namespace
{
constexpr double Mbit = 125000.0; // bytes/s per Mbit/s

struct Clock0 {
    Clock::time_point now = Clock::time_point(std::chrono::seconds(1000));
    void advance(std::chrono::milliseconds d) { now += d; }
};

Inputs idleLan(quint32 capacityMbit = 100)
{
    Inputs in;
    in.capacityKbps = capacityMbit * 1000;
    in.otherKbps = 0;
    return in;
}
Inputs activeVideo(quint32 capacityMbit = 100, quint32 otherMbit = 20)
{
    Inputs in = idleLan(capacityMbit);
    in.otherKbps = otherMbit * 1000;
    in.videoActive = true;
    return in;
}
/// The socket holds 200 KiB with nothing else on the link (so recovery tests start from a clean average).
Inputs queueFull(quint32 capacityMbit = 100)
{
    Inputs in = idleLan(capacityMbit);
    in.sendQueueBytes = 200 * 1024;
    return in;
}
Inputs pressure(quint32 capacityMbit = 100)
{
    Inputs in = activeVideo(capacityMbit);
    in.sendQueueBytes = 200 * 1024;
    return in;
}

/// n ticks of 100 ms.
void run(FileTransferPacer &p, Clock0 &c, const Inputs &in, int ticks)
{
    for (int i = 0; i < ticks; ++i) {
        c.advance(100ms);
        p.tick(c.now, in);
    }
}

/// PACER ramp: a requester pulling file bytes through a simulated link (1 ms steps), the way the host's Fetcher does: request sizes and the in-flight byte budget come from
/// the pacer, a request is issued while the budget allows and the bucket has tokens, the response is serialised by a link of limited rate and arrives one round trip later, where
/// the pacer is told about it (noteDelivered). Nothing here is the code under test except the pacer itself.
struct Pipe {
    double rttSeconds = 0.001;
    double linkBytesPerSecond = 1e12;
    bool answers = true; ///< false: a peer that never acknowledges anything
    bool feedQueue = false; ///< the link's queue is the pacer's send-queue evidence (what a sender that can see its socket has)
    bool reportDelivered = true; ///< false: the owner does not call noteDelivered (the legacy behaviour)
    bool sender = false; ///< the pacer paces a SENDER that writes slices behind the head-of-line bound (the host serving a copy): the bytes written count as delivered
};
struct Outcome {
    double secondsToGoal = -1; ///< when `goal` bytes had been delivered (-1: not within the time)
    double secondsToShare = -1; ///< when the rate delivered over the last second first reached 80% of the link
    double peakRate = 0;
    double firstPressureAt = -1;
    double peakRateAfterPressure = 0;
    double peakQueue = 0;
    qint64 peakInflightBytes = 0;
    double rateAtEnd = 0;
    quint64 delivered = 0;
};
struct Response {
    double arrive; ///< at the requester
    quint32 bytes;
    double reachesLink; ///< the request reached the responder; serialisation starts then or when the link is free
    double serialised; ///< the last byte left the link
};
Outcome simulate(FileTransferPacer &p, Clock0 &c, const Pipe &pipe, quint64 goal, double seconds)
{
    Outcome out;
    std::deque<Response> flight;
    std::deque<std::pair<double, quint32>> lastSecond;
    double linkFree = 0;
    qint64 inflightBytes = 0;
    double window = 0;
    const int steps = int(seconds * 1000);
    for (int ms = 1; ms <= steps; ++ms) {
        const double t = ms / 1000.0;
        c.advance(1ms);
        Inputs in;
        double queue = 0; // bytes at the bottleneck: requests that reached the responder and are not yet serialised
        for (const auto &r : flight) {
            if (r.reachesLink <= t && r.serialised > t) {
                const double start = r.serialised - double(r.bytes) / pipe.linkBytesPerSecond;
                queue += double(r.bytes) * std::min(1.0, (r.serialised - std::max(t, start)) / (r.serialised - start));
            }
        }
        if (pipe.feedQueue) {
            in.sendQueueBytes = qint64(queue);
        }
        out.peakQueue = std::max(out.peakQueue, queue);
        p.tick(c.now, in);
        while (!flight.empty() && flight.front().arrive <= t) {
            const auto r = flight.front();
            flight.pop_front();
            inflightBytes -= r.bytes;
            out.delivered += r.bytes;
            lastSecond.push_back({t, r.bytes});
            if (pipe.reportDelivered) {
                p.noteDelivered(c.now, r.bytes);
            }
            if (out.secondsToGoal < 0 && out.delivered >= goal) {
                out.secondsToGoal = t;
            }
        }
        while (!lastSecond.empty() && lastSecond.front().first < t - 1.0) {
            lastSecond.pop_front();
        }
        window = 0;
        for (const auto &d : lastSecond) {
            window += d.second;
        }
        if (out.secondsToShare < 0 && t >= 1.0 && window >= 0.8 * pipe.linkBytesPerSecond) {
            out.secondsToShare = t;
        }
        if (pipe.sender) {
            // the responder side of a copy: slices go into the bottleneck queue while the pacer and the head-of-line bound allow; what was written is "delivered"
            while (true) {
                const quint32 slice = p.sliceBytes();
                if (!p.maySend(c.now, qint64(queue), slice)) {
                    break;
                }
                p.consume(c.now, slice);
                if (pipe.reportDelivered) {
                    p.noteDelivered(c.now, slice);
                }
                const double start = std::max(linkFree, t);
                linkFree = start + double(slice) / pipe.linkBytesPerSecond;
                queue += slice;
                flight.push_back({linkFree + pipe.rttSeconds / 2, slice, t, linkFree});
                inflightBytes += slice;
                out.peakInflightBytes = std::max(out.peakInflightBytes, inflightBytes);
            }
        }
        // issue
        while (!pipe.sender) {
            const quint32 length = quint32(std::clamp(p.rateBytesPerSecond() * WindowSeconds, double(MinSliceBytes), double(Limits::RangeBytes)));
            if (!Limits::requesterAdmits(Limits::NegotiatedWindow, p.window(Limits::NegotiatedWindow), int(flight.size()), inflightBytes, length)) {
                break;
            }
            if (!p.maySend(c.now, std::nullopt, length)) {
                break;
            }
            p.consume(c.now, length);
            inflightBytes += length;
            out.peakInflightBytes = std::max(out.peakInflightBytes, inflightBytes);
            const double start = std::max(linkFree, t + pipe.rttSeconds / 2);
            linkFree = start + double(length) / pipe.linkBytesPerSecond;
            flight.push_back({pipe.answers ? linkFree + pipe.rttSeconds / 2 : 1e18, length, t + pipe.rttSeconds / 2, linkFree});
        }
        out.peakRate = std::max(out.peakRate, p.rateBytesPerSecond());
        if (p.underPressure() && out.firstPressureAt < 0) {
            out.firstPressureAt = t;
        }
        if (out.firstPressureAt >= 0) {
            out.peakRateAfterPressure = std::max(out.peakRateAfterPressure, p.rateBytesPerSecond());
        }
    }
    out.rateAtEnd = p.rateBytesPerSecond();
    return out;
}
}

class ClipboardFilePacingTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void constantsMatchTheSpec()
    {
        QCOMPARE(FloorBitsPerSecond, quint64(256000));
        QCOMPARE(FloorBytesPerSecond, 32000.0);
        QCOMPARE(StartBytesPerSecond, 128000.0); // 4 x floor = 1 Mbit/s
        QCOMPARE(PressureQueueBytes, qint64(64 * 1024));
    }
    void startsAtFourTimesTheFloorWhateverTheCapacity()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        QCOMPARE(p.rateBitsPerSecond(), quint64(1'024'000));
        QCOMPARE(p.window(), 1);
        QCOMPARE(p.sliceBytes(), 16u * 1024);
    }

    void idleLinkRampsToFullSpeedInAboutTwoSeconds()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, idleLan(), 1);
        QVERIFY(p.rateBytesPerSecond() > 128000.0 + 0.04 * 100 * Mbit); // +5% of base per tick
        run(p, c, idleLan(), 19);
        QCOMPARE(p.rateBytesPerSecond(), 100 * Mbit);                   // base = capacity, nothing else on the link
        run(p, c, idleLan(), 50);
        QCOMPARE(p.rateBytesPerSecond(), 100 * Mbit);                   // and it stays there
        QCOMPARE(p.window(), 5);                                        // spec 6.3: 100 Mbit/s -> W = 5
        QCOMPARE(p.sliceBytes(), 64u * 1024);
    }
    void activeVideoTakesHalfOfTheLeftover()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, idleLan(), 30);
        // video becomes active with 20 Mbit/s of its own: leftover = 80, half = 40
        run(p, c, activeVideo(100, 20), 100);
        // the other-traffic estimate is smoothed over 2 s: after 10 s it has converged
        QVERIFY(qAbs(p.rateBytesPerSecond() - 40 * Mbit) < 0.5 * Mbit);
        QVERIFY(!p.underPressure());
    }
    void audioAloneCountsAsActive()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        Inputs in = idleLan();
        in.audioActive = true;
        run(p, c, in, 60);
        QVERIFY(qAbs(p.rateBytesPerSecond() - 50 * Mbit) < 0.01 * Mbit);
    }

    void pressureHalvesPerTickAndReachesTheFloorWithinASecond()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, idleLan(), 30);
        QCOMPARE(p.rateBytesPerSecond(), 100 * Mbit);
        double previous = p.rateBytesPerSecond();
        int ticks = 0;
        while (p.rateBytesPerSecond() > FloorBytesPerSecond && ticks < 20) {
            run(p, c, pressure(), 1);
            ++ticks;
            QVERIFY(p.underPressure());
            QVERIFY2(p.rateBytesPerSecond() <= previous / 2.0 + 1e-6 || p.rateBytesPerSecond() == FloorBytesPerSecond, "halves or hits the floor");
            previous = p.rateBytesPerSecond();
        }
        QVERIFY(ticks <= 10); // 12.5 MB/s -> 32 kB/s is 8.6 halvings: under a second
        QCOMPARE(p.rateBytesPerSecond(), FloorBytesPerSecond);
        QCOMPARE(p.window(), 1);
        QCOMPARE(p.sliceBytes(), 16u * 1024);
    }
    void theFloorNeverReachesZeroHoweverLongThePressureLasts()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, pressure(), 1000);
        QCOMPARE(p.rateBytesPerSecond(), FloorBytesPerSecond);
        QCOMPARE(p.rateBitsPerSecond(), FloorBitsPerSecond);
        // and bytes still flow at the floor: a 16 KiB slice every ~0.5 s
        p.consume(c.now, 16 * 1024);
        QVERIFY(p.delayUntilSend(c.now) > 0ms);
        c.advance(p.delayUntilSend(c.now));
        QVERIFY(p.maySend(c.now, 0, 16 * 1024));
    }
    void everyPressureSignalDrivesTheRateDown()
    {
        struct Case {
            const char *name;
            void (*apply)(Inputs &);
        };
        const Case cases[] = {
            {"send queue 64 KiB", [](Inputs &in) { in.sendQueueBytes = 64 * 1024; }},
            {"video backlog", [](Inputs &in) { in.videoBacklog = true; }},
            {"audio late", [](Inputs &in) { in.audioLate = true; }},
            {"slow link", [](Inputs &in) { in.slow = true; }},
            {"congested", [](Inputs &in) { in.congested = true; }},
        };
        for (const Case &testCase : cases) {
            Clock0 c;
            FileTransferPacer p;
            p.reset(c.now);
            run(p, c, idleLan(), 30);
            Inputs in = idleLan();
            testCase.apply(in);
            run(p, c, in, 12);
            QVERIFY2(p.underPressure(), testCase.name);
            QCOMPARE(p.rateBytesPerSecond(), FloorBytesPerSecond);
        }
        // Just under the queue threshold is not pressure.
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        Inputs in = idleLan();
        in.sendQueueBytes = 64 * 1024 - 1;
        run(p, c, in, 30);
        QVERIFY(!p.underPressure());
        QCOMPARE(p.rateBytesPerSecond(), 100 * Mbit);
    }
    void recoversInAboutTwoSecondsAfterPressureEnds()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, idleLan(), 30);
        run(p, c, queueFull(), 12);
        QCOMPARE(p.rateBytesPerSecond(), FloorBytesPerSecond);
        run(p, c, idleLan(), 19);
        QVERIFY(p.rateBytesPerSecond() < 100 * Mbit);
        run(p, c, idleLan(), 2);
        QCOMPARE(p.rateBytesPerSecond(), 100 * Mbit); // +5% of base per tick: 20 ticks
    }
    void aTransferOnABusyLinkNeverStarvesVideo_floorIsAlsoTheCeilingUnderPressure()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        // 100 Mbit capacity, video takes 99.9: base would be below the floor; the floor holds
        Inputs in = activeVideo(100, 99);
        in.otherKbps = 99'900;
        run(p, c, in, 100);
        QVERIFY(p.rateBytesPerSecond() >= FloorBytesPerSecond);
        QVERIFY(p.rateBytesPerSecond() <= FloorBytesPerSecond * 1.01); // base clamps to the floor, half of it is below it
    }

    void unknownCapacityProbesUpFivePercentPerTick()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        Inputs in; // nothing known
        run(p, c, in, 1);
        QVERIFY(qAbs(p.rateBytesPerSecond() - 128000.0 * 1.05) < 1.0);
        run(p, c, in, 99);
        QVERIFY(p.rateBytesPerSecond() > 128000.0 * 100); // compounding: 1.05^100 = 131
        // Pressure during the probe ends it immediately
        in.sendQueueBytes = 1 << 20;
        run(p, c, in, 20);
        QCOMPARE(p.rateBytesPerSecond(), FloorBytesPerSecond);
    }
    void probeStopsAtTheCap()
    {
        Clock0 c;
        FileTransferPacer p(effectivePolicy(RequestedMode::FixedCap, 5'000'000, Ceiling::none()));
        p.reset(c.now);
        run(p, c, Inputs{}, 300);
        QCOMPARE(p.rateBytesPerSecond(), 5'000'000.0);
    }

    void windowFollowsTheRate()
    {
        // W = clamp(ceil(R * 0.1 s / 256 KiB), 1, 16)
        struct Row { double mbit; int window; };
        for (const Row r : {Row{0.256, 1}, Row{1, 1}, Row{20, 1}, Row{22, 2}, Row{41, 2}, Row{43, 3}, Row{100, 5}, Row{500, 16}, Row{1000, 16}, Row{10000, 16}}) {
            Clock0 c;
            Policy policy;
            policy.capBytesPerSecond = quint64(r.mbit * Mbit);
            FileTransferPacer p(policy);
            p.reset(c.now);
            run(p, c, idleLan(100000), 5000);
            const double wanted = std::ceil(std::min(r.mbit * Mbit, 1e18) * 0.1 / (256 * 1024));
            QCOMPARE(p.window(), std::clamp(int(wanted), 1, 16));
            QCOMPARE(p.window(), r.window);
        }
    }
    void windowOfAStockClientIsAtMostFour()
    {
        Clock0 c;
        FileTransferPacer p(effectivePolicy(RequestedMode::Unlimited, std::nullopt, Ceiling::none(), true));
        QVERIFY(!p.policy().unlimited); // a stock client runs Auto
        p.reset(c.now);
        run(p, c, idleLan(10000), 500);
        QCOMPARE(p.window(), Limits::StockClientWindow);
    }
    void sliceFollowsTheBurst()
    {
        Clock0 c;
        for (const auto &row : {std::pair<double, quint32>{0.256, 16384}, {5, 31250}, {8, 50000}, {20, 65536}, {1000, 65536}}) {
            Policy policy;
            policy.capBytesPerSecond = quint64(row.first * Mbit);
            FileTransferPacer p(policy);
            p.reset(c.now);
            run(p, c, idleLan(100000), 5000);
            QCOMPARE(p.sliceBytes(), row.second);
        }
    }

    void tokenBucketAveragesTheRate()
    {
        Clock0 c;
        Policy policy;
        policy.capBytesPerSecond = 1'000'000; // 8 Mbit/s
        FileTransferPacer p(policy);
        p.reset(c.now);
        run(p, c, idleLan(100000), 200); // at the cap
        QCOMPARE(p.rateBytesPerSecond(), 1'000'000.0);
        quint64 sent = 0;
        const auto start = c.now;
        for (int ms = 0; ms < 10'000; ms += 5) {
            c.advance(5ms);
            while (p.maySend(c.now, 0, p.sliceBytes())) {
                p.consume(c.now, p.sliceBytes());
                sent += p.sliceBytes();
            }
        }
        const double seconds = std::chrono::duration<double>(c.now - start).count();
        const double rate = double(sent) / seconds;
        QVERIFY2(rate > 950'000 && rate < 1'060'000, qPrintable(QString::number(rate)));
    }
    void overdrawnBucketWaitsItOut()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        QVERIFY(p.maySend(c.now, 0, 16384));
        p.consume(c.now, 16384);
        QVERIFY(!p.maySend(c.now, 0, 16384));
        const auto wait = p.delayUntilSend(c.now);
        QVERIFY(wait > 100ms && wait < 200ms); // 16 KiB at 128 kB/s
        c.advance(wait - 1ms);
        QVERIFY(!p.maySend(c.now, 0, 1));
        c.advance(2ms);
        QVERIFY(p.maySend(c.now, 0, 1));
        // The bucket never banks more than 50 ms of rate
        c.advance(60s);
        p.consume(c.now, 100'000);
        QVERIFY(!p.maySend(c.now, 0, 1));
    }

    void headOfLineBound_data()
    {
        QTest::addColumn<qint64>("queued");
        QTest::addColumn<quint32>("slice");
        QTest::addColumn<bool>("allowed");
        // refuse when queued + slice > max(64 KiB, 2 x slice)
        QTest::newRow("empty") << qint64(0) << quint32(65536) << true;
        QTest::newRow("64K queued, 64K slice: 128K <= 128K") << qint64(65536) << quint32(65536) << true;
        QTest::newRow("64K+1 queued, 64K slice") << qint64(65537) << quint32(65536) << false;
        QTest::newRow("16K slice: limit 64K, queued 48K") << qint64(48 * 1024) << quint32(16384) << true;
        QTest::newRow("16K slice: queued 48K+1") << qint64(48 * 1024 + 1) << quint32(16384) << false;
        QTest::newRow("32K slice: limit 64K, queued 32K") << qint64(32 * 1024) << quint32(32768) << true;
        QTest::newRow("32K slice: queued 32K+1") << qint64(32 * 1024 + 1) << quint32(32768) << false;
        QTest::newRow("unknown queue (-1) is not blocking") << qint64(-1) << quint32(65536) << true;
        QTest::newRow("huge queue") << qint64(10) * 1024 * 1024 << quint32(16384) << false;
    }
    void headOfLineBound()
    {
        QFETCH(qint64, queued);
        QFETCH(quint32, slice);
        QFETCH(bool, allowed);
        Clock0 c;
        Policy unlimited;
        unlimited.unlimited = true; // tokens are out of the way
        FileTransferPacer p(unlimited);
        p.reset(c.now);
        QCOMPARE(p.maySend(c.now, queued, slice), allowed);
        // an absent queue reading does not block either
        QVERIFY(p.maySend(c.now, std::nullopt, slice));
    }

    void unlimitedHasNoTokensButKeepsWindowAndQueueRule()
    {
        Clock0 c;
        FileTransferPacer p(effectivePolicy(RequestedMode::Unlimited, std::nullopt, Ceiling::none()));
        QVERIFY(p.unlimited());
        p.reset(c.now);
        run(p, c, pressure(), 50); // pressure does not slow it
        QCOMPARE(p.window(), Limits::MaxWindow);
        QCOMPARE(p.sliceBytes(), 64u * 1024);
        for (int i = 0; i < 100; ++i) {
            QVERIFY(p.maySend(c.now, 0, 65536));
            p.consume(c.now, 65536);
        }
        QVERIFY(!p.maySend(c.now, 200 * 1024, 65536)); // the head-of-line bound still holds
        QCOMPARE(p.delayUntilSend(c.now), 0ms);
    }

    void effectivePolicyTable_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<qint64>("cap");
        QTest::addColumn<int>("ceilingKind");
        QTest::addColumn<qint64>("ceilingRate");
        QTest::addColumn<bool>("unlimited");
        QTest::addColumn<bool>("balanced");
        QTest::addColumn<qint64>("expectedCap"); // -1 = none
        const int none = int(Ceiling::Kind::None), bal = int(Ceiling::Kind::Balanced), rate = int(Ceiling::Kind::Rate);
        const int autoM = int(RequestedMode::Auto), balM = int(RequestedMode::Balanced), fix = int(RequestedMode::FixedCap), unl = int(RequestedMode::Unlimited);
        QTest::newRow("auto, no ceiling") << autoM << qint64(0) << none << qint64(0) << false << false << qint64(-1);
        QTest::newRow("balanced, no ceiling") << balM << qint64(0) << none << qint64(0) << false << true << qint64(-1);
        QTest::newRow("fixed 5 MB/s, no ceiling") << fix << qint64(5'000'000) << none << qint64(0) << false << false << qint64(5'000'000);
        QTest::newRow("fixed without a value") << fix << qint64(0) << none << qint64(0) << false << false << qint64(-1);
        QTest::newRow("unlimited, no ceiling") << unl << qint64(0) << none << qint64(0) << true << false << qint64(-1);
        QTest::newRow("auto under a balanced ceiling") << autoM << qint64(0) << bal << qint64(0) << false << true << qint64(-1);
        QTest::newRow("unlimited under a balanced ceiling") << unl << qint64(0) << bal << qint64(0) << false << true << qint64(-1);
        QTest::newRow("fixed under a balanced ceiling") << fix << qint64(5'000'000) << bal << qint64(0) << false << true << qint64(5'000'000);
        QTest::newRow("auto under a rate ceiling") << autoM << qint64(0) << rate << qint64(2'000'000) << false << false << qint64(2'000'000);
        QTest::newRow("unlimited under a rate ceiling") << unl << qint64(0) << rate << qint64(2'000'000) << false << false << qint64(2'000'000);
        QTest::newRow("fixed below the ceiling") << fix << qint64(1'000'000) << rate << qint64(2'000'000) << false << false << qint64(1'000'000);
        QTest::newRow("fixed above the ceiling") << fix << qint64(9'000'000) << rate << qint64(2'000'000) << false << false << qint64(2'000'000);
        QTest::newRow("balanced under a rate ceiling") << balM << qint64(0) << rate << qint64(2'000'000) << false << true << qint64(2'000'000);
    }
    void effectivePolicyTable()
    {
        QFETCH(int, mode);
        QFETCH(qint64, cap);
        QFETCH(int, ceilingKind);
        QFETCH(qint64, ceilingRate);
        QFETCH(bool, unlimited);
        QFETCH(bool, balanced);
        QFETCH(qint64, expectedCap);
        Ceiling ceiling{Ceiling::Kind(ceilingKind), quint64(ceilingRate)};
        const Policy p = effectivePolicy(RequestedMode(mode), cap > 0 ? std::optional<quint64>(quint64(cap)) : std::nullopt, ceiling);
        QCOMPARE(p.unlimited, unlimited);
        QCOMPARE(p.balanced, balanced);
        QCOMPARE(p.capBytesPerSecond, expectedCap < 0 ? std::optional<quint64>() : std::optional<quint64>(quint64(expectedCap)));
        QVERIFY(!p.stockClient);
    }
    void balancedNeverExceedsHalfTheCapacityEvenWhenIdle()
    {
        Clock0 c;
        FileTransferPacer p(effectivePolicy(RequestedMode::Balanced, std::nullopt, Ceiling::none()));
        p.reset(c.now);
        run(p, c, idleLan(), 100);
        QCOMPARE(p.rateBytesPerSecond(), 50 * Mbit);
    }
    void capWinsOverTheAdaptiveRateButCongestionStillYieldsBelowIt()
    {
        Clock0 c;
        FileTransferPacer p(effectivePolicy(RequestedMode::FixedCap, 2'000'000, Ceiling::none()));
        p.reset(c.now);
        run(p, c, idleLan(), 100);
        QCOMPARE(p.rateBytesPerSecond(), 2'000'000.0); // 100 Mbit link, cap 16 Mbit/s
        run(p, c, pressure(), 12);
        QCOMPARE(p.rateBytesPerSecond(), FloorBytesPerSecond); // yields to congestion below the cap
        run(p, c, idleLan(), 100);
        QCOMPARE(p.rateBytesPerSecond(), 2'000'000.0);
    }
    void ceilingBeatsTheUsersCapAndUnlimited()
    {
        Clock0 c;
        FileTransferPacer p(effectivePolicy(RequestedMode::FixedCap, 9'000'000, Ceiling::rate(1'000'000)));
        p.reset(c.now);
        run(p, c, idleLan(), 100);
        QCOMPARE(p.rateBytesPerSecond(), 1'000'000.0);
        FileTransferPacer q(effectivePolicy(RequestedMode::Unlimited, std::nullopt, Ceiling::rate(1'000'000)));
        q.reset(c.now);
        run(q, c, idleLan(), 100);
        QCOMPARE(q.rateBytesPerSecond(), 1'000'000.0);
        QVERIFY(!q.unlimited());
    }
    void aCapBelowTheFloorWinsOverTheFloor()
    {
        Clock0 c;
        FileTransferPacer p(effectivePolicy(RequestedMode::FixedCap, 10'000, Ceiling::none())); // 80 kbit/s
        p.reset(c.now);
        run(p, c, pressure(), 50);
        QCOMPARE(p.rateBytesPerSecond(), 10'000.0);
        run(p, c, idleLan(), 50);
        QCOMPARE(p.rateBytesPerSecond(), 10'000.0);
        QVERIFY(p.rateBytesPerSecond() > 0);
    }
    void changingThePolicyLowersTheRateAtOnce()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, idleLan(), 50);
        QCOMPARE(p.rateBytesPerSecond(), 100 * Mbit);
        p.setPolicy(effectivePolicy(RequestedMode::FixedCap, 1'000'000, Ceiling::none()));
        QCOMPARE(p.rateBytesPerSecond(), 1'000'000.0);
    }

    void tickIsIdempotentWithinOneTickAndCatchesUpAfterAGap()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        c.advance(100ms);
        p.tick(c.now, idleLan());
        const double after1 = p.rateBytesPerSecond();
        c.advance(30ms);
        p.tick(c.now, idleLan()); // less than a tick later: nothing
        QCOMPARE(p.rateBytesPerSecond(), after1);
        c.advance(70ms);
        p.tick(c.now, idleLan());
        QVERIFY(p.rateBytesPerSecond() > after1);
        // a 5 s stall replays at most MaxTicksPerCall ticks
        FileTransferPacer q;
        q.reset(c.now);
        c.advance(100ms);
        q.tick(c.now, idleLan());
        c.advance(5s);
        q.tick(c.now, pressure());
        QCOMPARE(q.rateBytesPerSecond(), FloorBytesPerSecond);
        // time going backwards is harmless
        q.tick(c.now - 10s, idleLan());
    }
    void resetStartsAgain()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, idleLan(), 50);
        p.reset(c.now);
        QCOMPARE(p.rateBytesPerSecond(), StartBytesPerSecond);
        QVERIFY(!p.underPressure());
    }
    void otherTrafficIsSmoothedOverTwoSeconds()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, activeVideo(100, 0), 100);
        const double before = p.rateBytesPerSecond();
        QCOMPARE(before, 50 * Mbit);
        // video suddenly uses 60 Mbit/s: the target falls gradually as the average catches up (not in one tick)
        run(p, c, activeVideo(100, 60), 1);
        QVERIFY(p.rateBytesPerSecond() > 40 * Mbit);
        run(p, c, activeVideo(100, 60), 100);
        QVERIFY(qAbs(p.rateBytesPerSecond() - 20 * Mbit) < 0.5 * Mbit);
    }

    // ---- OPT-078 small files: the byte budget the pacer derives from its rate ----
    void byteBudgetAdmitsExactlyTheOldNumberOfFullSizeRequestsAtEveryRate()
    {
        // The budget is about 100 ms of data; a requester sends while the bytes in flight are below it, so with 256 KiB requests the number in flight is
        // ceil(budget / 256 KiB) = the old window, whatever the count cap says (and never more than the strict 4 MiB).
        struct Row { double mbit; qint64 budget; };
        for (const Row r : {Row{0.256, 16384}, Row{1, 16384}, Row{20, 250000}, Row{22, 275000}, Row{43, 537500}, Row{100, 1'250'000}, Row{500, 4 * 1024 * 1024}, Row{10000, 4 * 1024 * 1024}}) {
            Clock0 c;
            Policy policy;
            policy.capBytesPerSecond = quint64(r.mbit * Mbit);
            FileTransferPacer p(policy);
            p.reset(c.now);
            run(p, c, idleLan(100000), 5000);
            const auto w = p.window(Limits::NegotiatedWindow);
            QCOMPARE(w.maxBytes, r.budget);
            QCOMPARE(w.maxCount, 128);
            int admitted = 0;
            while (Limits::requesterAdmits(Limits::NegotiatedWindow, w, admitted, qint64(admitted) * 256 * 1024, 256 * 1024)) ++admitted;
            QCOMPARE(admitted, p.window()); // the old count window
            // the legacy limits (16 requests, no byte rule) admit the same
            const auto legacy = p.window(Limits::LegacyWindow);
            QCOMPARE(legacy.maxCount, 16);
            int legacyAdmitted = 0;
            while (Limits::requesterAdmits(Limits::LegacyWindow, legacy, legacyAdmitted, qint64(legacyAdmitted) * 256 * 1024, 256 * 1024)) ++legacyAdmitted;
            QCOMPARE(legacyAdmitted, p.window());
        }
    }
    void aTinyRequestRampsLikeTheOldCountDid()
    {
        // The Auto ramp relies on the one-request overshoot: with 20 KB requests and a 48 KB budget three go out (ceil), not two (floor).
        const Limits::Window budget{48 * 1024, 128};
        int admitted = 0;
        while (Limits::requesterAdmits(Limits::NegotiatedWindow, budget, admitted, qint64(admitted) * 20000, 20000)) ++admitted;
        QCOMPARE(admitted, 3);
    }
    void aBiggerCountCannotRaiseTheBytesOfACapOrBalancedMode()
    {
        // Fixed cap 1 MB/s: about 100 ms of data in flight (one 100 KB budget), the same with 16 or 128 requests allowed.
        Clock0 c;
        Policy cap;
        cap.capBytesPerSecond = 1'000'000;
        FileTransferPacer capped(cap);
        capped.reset(c.now);
        run(capped, c, idleLan(100000), 5000);
        QCOMPARE(capped.window(Limits::LegacyWindow).maxBytes, capped.window(Limits::NegotiatedWindow).maxBytes);
        QCOMPARE(capped.window(Limits::NegotiatedWindow).maxBytes, qint64(100'000));
        // Balanced on a 100 Mbit link: 50% of the capacity = 6.25 MB/s -> 625 kB, 3 x 256 KiB
        Policy balanced;
        balanced.balanced = true;
        FileTransferPacer half(balanced);
        half.reset(c.now);
        run(half, c, idleLan(100), 500);
        QCOMPARE(half.window(Limits::NegotiatedWindow).maxBytes, half.window(Limits::LegacyWindow).maxBytes);
        QVERIFY(half.window(Limits::NegotiatedWindow).maxBytes <= 4 * 256 * 1024);
        // The floor: 16 KiB, one small request or a few tiny ones; never the 4 MiB
        Policy floor;
        floor.capBytesPerSecond = 1; // below the floor: the cap wins
        FileTransferPacer slow(floor);
        slow.reset(c.now);
        QCOMPARE(slow.window(Limits::NegotiatedWindow).maxBytes, qint64(MinSliceBytes));
        // Pressure drives any window to the floor rate: the budget follows
        FileTransferPacer pressured;
        pressured.reset(c.now);
        run(pressured, c, idleLan(100), 100);
        const auto before = pressured.window(Limits::NegotiatedWindow).maxBytes;
        run(pressured, c, pressure(100), 100);
        QVERIFY(pressured.window(Limits::NegotiatedWindow).maxBytes < before);
        QCOMPARE(pressured.window(Limits::NegotiatedWindow).maxBytes, qint64(MinSliceBytes));
    }
    void unlimitedGetsTheLimitsAndAStockClientTheOldCount()
    {
        Clock0 c;
        Policy unlimited;
        unlimited.unlimited = true;
        FileTransferPacer free(unlimited);
        QCOMPARE(free.window(Limits::NegotiatedWindow), Limits::NegotiatedWindow);
        QCOMPARE(free.window(Limits::LegacyWindow), Limits::LegacyWindow);
        // A stock client: the old count (<= 4), with no byte rule beyond it, whatever window it is given
        FileTransferPacer stock(effectivePolicy(RequestedMode::Unlimited, std::nullopt, Ceiling::none(), true));
        stock.reset(c.now);
        run(stock, c, idleLan(10000), 500);
        QCOMPARE(stock.window(Limits::NegotiatedWindow).maxCount, Limits::StockClientWindow);
        QCOMPARE(stock.window(Limits::LegacyWindow).maxCount, Limits::StockClientWindow);
        QCOMPARE(stock.window(Limits::NegotiatedWindow).maxBytes, qint64(Limits::StockClientWindow) * Limits::MaxRangeBytes);
    }

    // ---- PACER ramp: slow start (the start-up of an Auto transfer is ack-clocked instead of a fixed 5% per tick) ----

    void noKnowledgeFiveMiBOnALoopbackTakesUnderASecond()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        Pipe pipe; // 1 ms round trip, nothing limits the link
        const auto o = simulate(p, c, pipe, 5 * 1024 * 1024, 5);
        QVERIFY2(o.secondsToGoal > 0 && o.secondsToGoal < 1.0, qPrintable(QStringLiteral("5 MiB took %1 s").arg(o.secondsToGoal)));
        // the same transfer with the owner not reporting deliveries is the legacy 5% ramp: about 6 s (the control)
        Clock0 c2;
        FileTransferPacer legacy;
        legacy.reset(c2.now);
        Pipe silent = pipe;
        silent.reportDelivered = false;
        const auto old = simulate(legacy, c2, silent, 5 * 1024 * 1024, 10);
        QVERIFY2(old.secondsToGoal > 5.5 && old.secondsToGoal < 7.0, qPrintable(QStringLiteral("legacy took %1 s").arg(old.secondsToGoal)));
    }
    void noKnowledgeFiveMiBOverALanTakesUnderTwoSeconds()
    {
        for (const double rtt : {0.005, 0.02, 0.05}) {
            Clock0 c;
            FileTransferPacer p;
            p.reset(c.now);
            Pipe pipe;
            pipe.rttSeconds = rtt;
            pipe.linkBytesPerSecond = 125e6 / 10; // a 1 Gbit/s LAN shared: 100 Mbit/s
            const auto o = simulate(p, c, pipe, 5 * 1024 * 1024, 5);
            QVERIFY2(o.secondsToGoal > 0 && o.secondsToGoal < 2.0, qPrintable(QStringLiteral("rtt %1: 5 MiB took %2 s").arg(rtt).arg(o.secondsToGoal)));
        }
    }
    void aSlowLinkReachesEightyPercentNoLaterThanTheOldRampAndNeverFloodsIt()
    {
        // 20 Mbit/s, 100 ms round trip, the sender sees its socket queue (the host serving a copy): pressure at 64 KiB queued
        struct Row { bool reports; Outcome out; };
        Outcome results[2];
        for (const bool reports : {false, true}) {
            Clock0 c;
            FileTransferPacer p;
            p.reset(c.now);
            Pipe pipe;
            pipe.sender = true;
            pipe.feedQueue = true;
            pipe.rttSeconds = 0.1;
            pipe.linkBytesPerSecond = 2.5e6;
            pipe.reportDelivered = reports;
            results[reports ? 1 : 0] = simulate(p, c, pipe, 5 * 1024 * 1024, 20);
        }
        const Outcome &old = results[0];
        const Outcome &now = results[1];
        QVERIFY(old.secondsToShare > 0);
        QVERIFY2(now.secondsToShare > 0 && now.secondsToShare <= old.secondsToShare, qPrintable(QStringLiteral("80%% at %1 s, the old ramp at %2 s").arg(now.secondsToShare).arg(old.secondsToShare)));
        QVERIFY(now.secondsToShare < 3.0);
        QVERIFY(now.secondsToGoal < old.secondsToGoal);
        // never floods the link: the queue the pacer left on it is the head-of-line bound, the same as with the old ramp
        QVERIFY2(now.peakQueue <= old.peakQueue * 1.1, qPrintable(QStringLiteral("queue %1 vs %2").arg(now.peakQueue).arg(old.peakQueue)));
        // and the rate stays within twice the link rate at its peak (the old ramp: 1.2 x) and within 1.25 x after the first congestion signal
        QVERIFY(now.peakRate < 2.5 * 2.5e6);
        QVERIFY(now.firstPressureAt > 0);
        QVERIFY2(now.peakRateAfterPressure < 2.0 * 2.5e6, qPrintable(QStringLiteral("after the first signal %1").arg(now.peakRateAfterPressure)));
        QVERIFY(now.rateAtEnd < 2.5e6);
    }
    void aBlindRequesterOnASlowLinkIsNoWorseThanTheOldRamp()
    {
        // The host pulling a copy from the client sees no queue (the uplink is the client's): the keep-up check ends slow start; the queue never exceeds the old ramp's
        Outcome results[2];
        for (const bool reports : {false, true}) {
            Clock0 c;
            FileTransferPacer p;
            p.reset(c.now);
            Pipe pipe;
            pipe.rttSeconds = 0.1;
            pipe.linkBytesPerSecond = 2.5e6;
            pipe.reportDelivered = reports;
            results[reports ? 1 : 0] = simulate(p, c, pipe, 5 * 1024 * 1024, 20);
        }
        QVERIFY(results[1].secondsToShare > 0 && results[1].secondsToShare <= results[0].secondsToShare);
        QVERIFY(results[1].secondsToGoal < results[0].secondsToGoal);
        QVERIFY(results[1].peakQueue <= results[0].peakQueue * 1.05);
        QVERIFY(results[1].peakInflightBytes <= results[0].peakInflightBytes);
    }
    void aSilentPeerGetsOnlyTheLegacyRampAndABoundedWindow()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        Pipe pipe;
        pipe.answers = false; // never acknowledges anything
        const auto o = simulate(p, c, pipe, 1, 10);
        QCOMPARE(o.delivered, quint64(0));
        // after 10 s the rate is exactly the legacy probe: 100 ticks of 5% (the first tick is the one that starts the loop)
        QVERIFY(qAbs(o.rateAtEnd / (128000.0 * std::pow(1.05, 100)) - 1.0) < 0.05);
        // the bytes in flight never passed the byte budget of the rate plus one request (and the 4 MiB hard limit)
        QVERIFY(o.peakInflightBytes <= p.window(Limits::NegotiatedWindow).maxBytes + qint64(Limits::RangeBytes));
        QVERIFY(o.peakInflightBytes <= Limits::NegotiatedWindow.maxBytes);
        QVERIFY(p.inSlowStart()); // it is still waiting for the first acknowledgement
    }
    void deliveringNothingChangesNothing()
    {
        // no noteDelivered(): every legacy expectation above holds (the existing cases are the controls); here the exact compounding
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, Inputs{}, 30);
        QVERIFY(qAbs(p.rateBytesPerSecond() - 128000.0 * std::pow(1.05, 30)) < 10.0);
    }
    void deliveredBytesRaiseTheRateAtOncePerWindow()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, Inputs{}, 1);
        const double before = p.rateBytesPerSecond();
        p.noteDelivered(c.now, 16 * 1024); // the window (R x 0.1 s) grows by the bytes delivered: R by bytes / 0.1 s x gain
        QVERIFY(qAbs(p.rateBytesPerSecond() - (before + SlowStartGain * 16384.0 / WindowSeconds)) < 1.0);
        QVERIFY(p.inSlowStart());
    }
    void aCongestionSignalEndsSlowStartAtOnceAndTheCutIsTheOldOne()
    {
        for (int which = 0; which < 4; ++which) {
            Clock0 c;
            FileTransferPacer p;
            p.reset(c.now);
            run(p, c, Inputs{}, 1);
            p.noteDelivered(c.now, 1 << 20);
            const double grown = p.rateBytesPerSecond();
            QVERIFY(grown > 1e6);
            Inputs in;
            switch (which) {
            case 0: in.sendQueueBytes = 200 * 1024; break;
            case 1: in.videoBacklog = true; break;
            case 2: in.congested = true; break;
            case 3: in.slow = true; break;
            }
            run(p, c, in, 1);
            QVERIFY(!p.inSlowStart());
            QVERIFY(p.underPressure());
            QCOMPARE(p.rateBytesPerSecond(), grown / 2.0); // halves per tick, exactly the AIMD rule
            const double cut = p.rateBytesPerSecond();
            p.noteDelivered(c.now, 1 << 20); // more deliveries do not raise it any more
            QCOMPARE(p.rateBytesPerSecond(), cut);
            run(p, c, in, 30);
            QCOMPARE(p.rateBytesPerSecond(), FloorBytesPerSecond);
            run(p, c, Inputs{}, 1); // pressure over: only the legacy 5% probe is left
            QVERIFY(qAbs(p.rateBytesPerSecond() - FloorBytesPerSecond * 1.05) < 1.0);
            p.noteDelivered(c.now, 1 << 20);
            QVERIFY(qAbs(p.rateBytesPerSecond() - FloorBytesPerSecond * 1.05) < 1.0);
        }
    }
    void aGrowingSendQueueHoldsSlowStartBeforeThePressureThreshold()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        Inputs in;
        in.sendQueueBytes = SlowStartHoldQueueBytes;
        run(p, c, in, 1);
        QVERIFY(!p.underPressure());
        const double before = p.rateBytesPerSecond();
        p.noteDelivered(c.now, 1 << 20);
        QCOMPARE(p.rateBytesPerSecond(), before); // held
        in.sendQueueBytes = 0;
        run(p, c, in, 1);
        const double next = p.rateBytesPerSecond();
        p.noteDelivered(c.now, 1 << 20);
        QVERIFY(p.rateBytesPerSecond() > next);
    }
    void aKnownCapacitySeedsTheStartRateWithoutPassingTheTarget()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, idleLan(100), 1);
        QVERIFY(p.rateBytesPerSecond() >= SlowStartSeedShare * 100 * Mbit);
        QVERIFY(p.rateBytesPerSecond() <= 100 * Mbit);
        // video active halves the leftover: the seed is a share of THAT target
        FileTransferPacer busy;
        busy.reset(c.now);
        run(busy, c, activeVideo(100, 20), 1);
        const double target = 0.5 * 80 * Mbit;
        QVERIFY(busy.rateBytesPerSecond() >= SlowStartSeedShare * target);
        QVERIFY(busy.rateBytesPerSecond() <= target);
        // a slow link: the seed never exceeds what Auto would use there, and deliveries cannot take it past the target either
        FileTransferPacer slow;
        slow.reset(c.now);
        run(slow, c, idleLan(1), 1);
        QVERIFY(slow.rateBytesPerSecond() <= 1 * Mbit);
        slow.noteDelivered(c.now, 100 << 20);
        QVERIFY(slow.rateBytesPerSecond() <= 1 * Mbit);
        QVERIFY(slow.rateBytesPerSecond() >= SlowStartSeedShare * Mbit);
        // reaching the target ends slow start
        run(slow, c, idleLan(1), 40);
        QCOMPARE(slow.rateBytesPerSecond(), 1 * Mbit);
        QVERIFY(!slow.inSlowStart());
    }
    void slowStartNeverPassesTheCapOrTheOwnersCeiling()
    {
        for (const auto &policy : {effectivePolicy(RequestedMode::FixedCap, 5'000'000, Ceiling::none()),
                                   effectivePolicy(RequestedMode::Auto, std::nullopt, Ceiling::rate(3'000'000)),
                                   effectivePolicy(RequestedMode::FixedCap, 8'000'000, Ceiling::rate(2'000'000))}) {
            Clock0 c;
            FileTransferPacer p(policy);
            p.reset(c.now);
            run(p, c, Inputs{}, 1);
            for (int i = 0; i < 50; ++i) {
                p.noteDelivered(c.now, 1 << 20);
                QVERIFY(p.rateBytesPerSecond() <= double(*policy.capBytesPerSecond));
            }
            QCOMPARE(p.rateBytesPerSecond(), double(*policy.capBytesPerSecond)); // and it gets there at once
        }
        // a cap below the start rate: the start rate IS the cap
        FileTransferPacer tiny(effectivePolicy(RequestedMode::FixedCap, 50'000, Ceiling::none()));
        Clock0 c;
        tiny.reset(c.now);
        tiny.noteDelivered(c.now, 1 << 20);
        QCOMPARE(tiny.rateBytesPerSecond(), 50'000.0);
        // the rate a policy change lowers is not raised again by slow start beyond the new cap
        FileTransferPacer q;
        q.reset(c.now);
        q.noteDelivered(c.now, 1 << 20);
        q.setPolicy(effectivePolicy(RequestedMode::FixedCap, 1'000'000, Ceiling::none()));
        QCOMPARE(q.rateBytesPerSecond(), 1'000'000.0);
        q.noteDelivered(c.now, 1 << 20);
        QCOMPARE(q.rateBytesPerSecond(), 1'000'000.0);
    }
    void balancedAndUnlimitedDoNotSlowStart()
    {
        Clock0 c;
        Policy balanced;
        balanced.balanced = true;
        FileTransferPacer b(balanced);
        b.reset(c.now);
        QVERIFY(!b.inSlowStart());
        run(b, c, idleLan(100), 1);
        const double before = b.rateBytesPerSecond();
        b.noteDelivered(c.now, 100 << 20);
        QCOMPARE(b.rateBytesPerSecond(), before);
        run(b, c, idleLan(100), 200);
        QCOMPARE(b.rateBytesPerSecond(), 50 * Mbit); // the share rule, as before
        Policy unlimited;
        unlimited.unlimited = true;
        FileTransferPacer u(unlimited);
        u.reset(c.now);
        QVERIFY(!u.inSlowStart());
        const double rate = u.rateBytesPerSecond();
        u.noteDelivered(c.now, 100 << 20);
        QCOMPARE(u.rateBytesPerSecond(), rate);
        // a stock client is Auto: it may slow start (its window stays at most 4)
        FileTransferPacer stock(effectivePolicy(RequestedMode::Balanced, std::nullopt, Ceiling::none(), true));
        stock.reset(c.now);
        QVERIFY(stock.inSlowStart());
        stock.noteDelivered(c.now, 1 << 20);
        QVERIFY(stock.window() <= Limits::StockClientWindow);
    }
    void aRateEstablishedEarlierInTheConnectionIsCarriedOverUnchanged()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, Inputs{}, 1);
        p.noteDelivered(c.now, 1 << 20);
        run(p, c, Inputs{}, 1);
        const double established = p.rateBytesPerSecond();
        QVERIFY(established > 5e6);
        QVERIFY(p.inSlowStart());
        // the transfer ends; the next one starts three seconds later on the same pacer (Serve does not reset it): no slow start, the rate stays what it was
        c.advance(3s);
        p.tick(c.now, Inputs{});
        QVERIFY(!p.inSlowStart());
        const double after = p.rateBytesPerSecond();
        QVERIFY(after >= established);
        QVERIFY(after <= established * std::pow(1.05, MaxTicksPerCall) * 1.0001); // only the legacy catch-up of the missed ticks
        p.noteDelivered(c.now, 1 << 20);
        QCOMPARE(p.rateBytesPerSecond(), after);
        // while a transfer keeps running (gaps of one tick) it stays in slow start
        FileTransferPacer q;
        q.reset(c.now);
        for (int i = 0; i < 8; ++i) {
            c.advance(100ms);
            q.tick(c.now, Inputs{});
            for (int chunk = 0; chunk < 10; ++chunk) {
                q.noteDelivered(c.now, quint64(q.rateBytesPerSecond() * 0.01)); // a transfer that delivers what its rate allows, spread over the tick like real acknowledgements
            }
        }
        QVERIFY(q.inSlowStart());
    }
    void slowStartEndsWhenTheDeliveryDoesNotKeepUp()
    {
        Clock0 c;
        FileTransferPacer p;
        p.reset(c.now);
        run(p, c, Inputs{}, 1); // the baseline interval starts here
        const double start = p.rateBytesPerSecond();
        p.noteDelivered(c.now, 8 * 1024); // grows
        QVERIFY(p.rateBytesPerSecond() > start);
        c.advance(100ms);
        p.tick(c.now, Inputs{}); // 80 kB/s delivered against 134 kB/s: keeps up
        QVERIFY(p.inSlowStart());
        const double baseline = p.rateBytesPerSecond();
        // the next interval delivers well under half of the rate it began with: the link or the peer limits, not R
        c.advance(100ms);
        p.noteDelivered(c.now, 100); // 1 kB/s
        p.tick(c.now, Inputs{});
        QVERIFY(!p.inSlowStart());
        QVERIFY(p.rateBytesPerSecond() <= baseline * 1.0001); // the interval's growth is undone (the legacy 5% of the tick is part of it)
        // an interval with nothing delivered at all is a lull, not a verdict
        FileTransferPacer q;
        q.reset(c.now);
        run(q, c, Inputs{}, 5);
        QVERIFY(q.inSlowStart());
    }
    void thePureRulesNeedNoTimersOrGlobals()
    {
        // two pacers driven by different clocks do not influence each other
        Clock0 a;
        Clock0 b;
        b.advance(std::chrono::hours(5));
        FileTransferPacer x;
        FileTransferPacer y;
        x.reset(a.now);
        y.reset(b.now);
        run(x, a, Inputs{}, 1);
        run(y, b, Inputs{}, 1);
        x.noteDelivered(a.now, 1 << 20);
        QCOMPARE(y.rateBytesPerSecond(), 128000.0 * 1.05);
    }
};

QTEST_GUILESS_MAIN(ClipboardFilePacingTest)

#include "ClipboardFilePacingTest.moc"
