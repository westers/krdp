// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 1: the QoS decision table: connection stats -> pacer inputs -> rate, for every mode and ceiling.

#include <QTest>

#include "ClipboardFileQos.h"

using namespace KRdp;
using namespace KRdp::ClipboardFilePacing;
using namespace std::chrono_literals;

namespace
{
constexpr double Mbit = 125000.0;

Stats::Snapshot quietLan()
{
    Stats::Snapshot s;
    s.capacityKbps = 100'000;
    s.capacitySource = QStringLiteral("tcp");
    s.sentKbps = 0;
    s.sendQueueBytes = 0;
    return s;
}

/// Drives a pacer for \a ticks with the snapshot (the previous one is the same but for grown counters).
double settle(const Policy &policy, const Stats::Snapshot &now, int ticks = 200, bool videoBacklog = false)
{
    FileTransferPacer pacer(policy);
    Clock::time_point t(std::chrono::seconds(100));
    pacer.reset(t);
    Stats::Snapshot previous = now;
    Stats::Snapshot current = now;
    for (int i = 0; i < ticks; ++i) {
        current.framesSent = previous.framesSent + (now.framesSent ? 1 : 0); // active video: frames grow every tick
        t += 100ms;
        pacer.tick(t, ClipboardFileQos::inputsFromStats(current, &previous, 0, videoBacklog));
        previous = current;
    }
    return pacer.rateBytesPerSecond();
}
}

class ClipboardFileQosTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void mapsTheSnapshotOntoInputs()
    {
        Stats::Snapshot previous = quietLan();
        Stats::Snapshot now = quietLan();
        now.sentKbps = 30'000;
        now.sendQueueBytes = 12345;
        now.framesSent = previous.framesSent + 3;
        now.slow = true;
        now.congested = true;
        now.audio = Stats::AudioFlow{.packets = 50, .late = 1};
        previous.audio = Stats::AudioFlow{.packets = 10, .late = 0};
        const auto in = ClipboardFileQos::inputsFromStats(now, &previous, 5000, true);
        QCOMPARE(in.capacityKbps, std::optional<quint32>(100'000));
        QCOMPARE(in.otherKbps, std::optional<quint32>(25'000)); // the file's own 5 Mbit/s is not "other traffic"
        QCOMPARE(in.sendQueueBytes, std::optional<qint64>(12345));
        QVERIFY(in.videoActive && in.audioActive && in.audioLate && in.slow && in.congested && in.videoBacklog);
    }
    void quietLinkAndMissingEvidence()
    {
        const Stats::Snapshot s = quietLan();
        auto in = ClipboardFileQos::inputsFromStats(s, &s);
        QVERIFY(!in.videoActive && !in.audioActive && !in.audioLate && !in.slow && !in.congested && !in.videoBacklog);
        in = ClipboardFileQos::inputsFromStats(s, nullptr); // no previous snapshot: activity unknown = quiet
        QVERIFY(!in.videoActive);
        Stats::Snapshot unknown; // a fresh connection: nothing is known
        in = ClipboardFileQos::inputsFromStats(unknown, nullptr);
        QVERIFY(!in.capacityKbps && !in.otherKbps && !in.sendQueueBytes);
        // the file's share can exceed the reading (rounding): never negative
        Stats::Snapshot low = quietLan();
        low.sentKbps = 100;
        QCOMPARE(ClipboardFileQos::inputsFromStats(low, nullptr, 5000).otherKbps, std::optional<quint32>(0));
    }
    void audioPressureCountersEachCount()
    {
        Stats::Snapshot previous = quietLan();
        previous.audio = Stats::AudioFlow{};
        for (int which = 0; which < 3; ++which) {
            Stats::Snapshot now = previous;
            now.audio = Stats::AudioFlow{};
            if (which == 0) now.audio->late = 1;
            if (which == 1) now.audio->overflowEvents = 1;
            if (which == 2) now.audio->producerGaps = 1;
            QVERIFY(ClipboardFileQos::inputsFromStats(now, &previous).audioLate);
        }
        Stats::Snapshot now = previous;
        now.audio->maxGapMs = 500; // history alone is not current pressure
        QVERIFY(!ClipboardFileQos::inputsFromStats(now, &previous).audioLate);
    }

    void decisionTable_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<qint64>("cap");
        QTest::addColumn<int>("ceilingKind");
        QTest::addColumn<qint64>("ceilingRate");
        QTest::addColumn<int>("scene"); // 0 idle LAN, 1 video active (20 Mbit/s), 2 queue backed up, 3 slow, 4 capacity unknown
        QTest::addColumn<double>("expectedMbit"); // -1: unbounded (checked separately)
        const int autoM = int(RequestedMode::Auto), balM = int(RequestedMode::Balanced), fix = int(RequestedMode::FixedCap);
        const int none = int(Ceiling::Kind::None), bal = int(Ceiling::Kind::Balanced), rate = int(Ceiling::Kind::Rate);
        const double floor = FloorBytesPerSecond / Mbit;
        // Auto
        QTest::newRow("auto idle") << autoM << qint64(0) << none << qint64(0) << 0 << 100.0;
        QTest::newRow("auto video") << autoM << qint64(0) << none << qint64(0) << 1 << 40.0;
        QTest::newRow("auto queue") << autoM << qint64(0) << none << qint64(0) << 2 << floor;
        QTest::newRow("auto slow") << autoM << qint64(0) << none << qint64(0) << 3 << floor;
        // Balanced
        QTest::newRow("balanced idle") << balM << qint64(0) << none << qint64(0) << 0 << 50.0;
        QTest::newRow("balanced video") << balM << qint64(0) << none << qint64(0) << 1 << 40.0; // half the leftover is already below half the capacity
        QTest::newRow("balanced queue") << balM << qint64(0) << none << qint64(0) << 2 << floor;
        // Fixed cap 4 MB/s = 32 Mbit/s
        QTest::newRow("cap idle") << fix << qint64(4'000'000) << none << qint64(0) << 0 << 32.0;
        QTest::newRow("cap video (leftover half is 40)") << fix << qint64(4'000'000) << none << qint64(0) << 1 << 32.0;
        QTest::newRow("cap queue") << fix << qint64(4'000'000) << none << qint64(0) << 2 << floor;
        // Ceilings
        QTest::newRow("auto under balanced ceiling, idle") << autoM << qint64(0) << bal << qint64(0) << 0 << 50.0;
        QTest::newRow("auto under 1 MB/s ceiling, idle") << autoM << qint64(0) << rate << qint64(1'000'000) << 0 << 8.0;
        QTest::newRow("cap 4 MB/s under 1 MB/s ceiling") << fix << qint64(4'000'000) << rate << qint64(1'000'000) << 0 << 8.0;
        QTest::newRow("cap 0.5 MB/s under 1 MB/s ceiling") << fix << qint64(500'000) << rate << qint64(1'000'000) << 0 << 4.0;
        QTest::newRow("ceiling under queue pressure") << autoM << qint64(0) << rate << qint64(1'000'000) << 2 << floor;
    }
    void decisionTable()
    {
        QFETCH(int, mode);
        QFETCH(qint64, cap);
        QFETCH(int, ceilingKind);
        QFETCH(qint64, ceilingRate);
        QFETCH(int, scene);
        QFETCH(double, expectedMbit);
        Stats::Snapshot s = quietLan();
        switch (scene) {
        case 1:
            s.sentKbps = 20'000;
            s.framesSent = 1;
            break;
        case 2:
            s.sendQueueBytes = 256 * 1024;
            break;
        case 3:
            s.slow = true;
            break;
        default:
            break;
        }
        const Policy policy = effectivePolicy(RequestedMode(mode), cap > 0 ? std::optional<quint64>(quint64(cap)) : std::nullopt, Ceiling{Ceiling::Kind(ceilingKind), quint64(ceilingRate)});
        const double rate = settle(policy, s, scene == 1 ? 300 : 200);
        QVERIFY2(qAbs(rate / Mbit - expectedMbit) < 0.01, qPrintable(QStringLiteral("got %1 Mbit/s, expected %2").arg(rate / Mbit).arg(expectedMbit)));
    }
    void videoBacklogAloneYieldsToTheFloor()
    {
        QCOMPARE(settle(Policy{}, quietLan(), 200, true), FloorBytesPerSecond);
    }
    void unknownCapacityProbesButTheQueueRuleStopsIt()
    {
        Stats::Snapshot s;
        s.sendQueueBytes = 0;
        const double probing = settle(Policy{}, s, 100);
        QVERIFY(probing > 128000.0 * 50);
        s.sendQueueBytes = 128 * 1024;
        QCOMPARE(settle(Policy{}, s, 100), FloorBytesPerSecond);
    }
    void aClientSlowToReadIsNotPressure()
    {
        // limit == "client": bounded by its request window, the host does not slow the file down for it
        Stats::Snapshot s = quietLan();
        s.limit = QStringLiteral("client");
        QCOMPARE(settle(Policy{}, s, 200), 100 * Mbit);
    }
};

QTEST_GUILESS_MAIN(ClipboardFileQosTest)

#include "ClipboardFileQosTest.moc"
