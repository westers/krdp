// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 1: FileTransferPacer driven by a fake clock: idle to full speed, pressure halves per tick, the floor never reaches zero,
// recovery in about 2 s, window and slice derivation, unknown-capacity start, cap/ceiling precedence, the head-of-line bound.

#include <QTest>

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
};

QTEST_GUILESS_MAIN(ClipboardFilePacingTest)

#include "ClipboardFilePacingTest.moc"
