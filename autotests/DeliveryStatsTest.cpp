// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 0: the measurement counters of the file-transfer acceptance (spec 6.4): bounded histograms (frame delay at send, send-queue depth),
// the audio late/overflow/gap counters, the external audio queue's counting, and their appearance in the stats-sample record.

#include <QJsonObject>
#include <QTest>
#include <thread>
#include <vector>

#include "DeliveryStats.h"
#include "ExternalAudioQueue.h"
#include "StatsReporter.h"

using namespace KRdp;
using namespace std::chrono_literals;

namespace
{
constexpr int PacketBytes = 44100 * 4 / 50; // 20 ms
using Clock = Stats::AudioFlowCounters::Clock;
Clock::time_point t(int ms)
{
    return Clock::time_point(std::chrono::seconds(5000) + std::chrono::milliseconds(ms));
}
QByteArray pcm(int packets)
{
    return QByteArray(packets * PacketBytes, '\x01');
}
}

class DeliveryStatsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void histogramBucketsAndQuantiles()
    {
        Stats::FrameDelay h;
        QCOMPARE(h.counts().count, quint64(0));
        QCOMPARE(h.counts().quantile(0.99), quint64(0));
        for (int i = 0; i < 90; ++i) h.record(1); // <= 1 ms
        for (int i = 0; i < 9; ++i) h.record(15); // <= 20 ms
        h.record(900);                            // <= 1280 ms
        const auto c = h.counts();
        QCOMPARE(c.count, quint64(100));
        QCOMPARE(c.sum, quint64(90 + 9 * 15 + 900));
        QCOMPARE(c.max, quint64(900));
        QCOMPARE(c.quantile(0.5), quint64(1));
        QCOMPARE(c.quantile(0.90), quint64(1));
        QCOMPARE(c.quantile(0.915), quint64(20)); // the 15 ms samples are reported as their bucket's upper bound
        QCOMPARE(c.quantile(0.985), quint64(20));
        QCOMPARE(c.quantile(1.0), quint64(900)); // bound 1280 capped at the running maximum
    }
    void histogramOverflowBucketReportsTheMaximum()
    {
        Stats::FrameDelay h;
        h.record(5000);
        h.record(7000);
        QCOMPARE(h.counts().buckets.back(), quint64(2));
        QCOMPARE(h.counts().quantile(0.5), quint64(7000));
    }
    void histogramBoundariesAreInclusive()
    {
        Stats::SendQueue q;
        q.record(0);
        q.record(1);
        q.record(4096);
        q.record(4097);
        q.record(65536);
        const auto c = q.counts();
        QCOMPARE(c.buckets[0], quint64(1)); // <= 0
        QCOMPARE(c.buckets[1], quint64(2)); // 1, 4096
        QCOMPARE(c.buckets[2], quint64(1)); // 4097
        QCOMPARE(c.buckets[3], quint64(1)); // 65536
    }
    void histogramDelta()
    {
        Stats::FrameDelay h;
        h.record(3);
        const auto before = h.counts();
        h.record(30);
        h.record(30);
        const auto interval = h.counts().since(before);
        QCOMPARE(interval.count, quint64(2));
        QCOMPARE(interval.sum, quint64(60));
        QCOMPARE(interval.quantile(0.5), quint64(30)); // bound 40 capped at max 30
        // a restarted counter counts from zero
        Stats::FrameDelay fresh;
        fresh.record(1);
        QCOMPARE(fresh.counts().since(h.counts()).count, quint64(1));
    }
    void histogramIsThreadSafe()
    {
        Stats::SendQueue q;
        std::vector<std::thread> threads;
        for (int n = 0; n < 4; ++n) {
            threads.emplace_back([&q, n] {
                for (int i = 0; i < 10000; ++i) q.record(quint64(i) * 10 + quint64(n));
            });
        }
        for (auto &th : threads) th.join();
        QCOMPARE(q.counts().count, quint64(40000));
        quint64 total = 0;
        for (auto b : q.counts().buckets) total += b;
        QCOMPARE(total, quint64(40000));
    }
    void ceilMilliseconds()
    {
        QCOMPARE(Stats::ceilMilliseconds(-5ms), quint64(0));
        QCOMPARE(Stats::ceilMilliseconds(0ns), quint64(0));
        QCOMPARE(Stats::ceilMilliseconds(1ns), quint64(1));
        QCOMPARE(Stats::ceilMilliseconds(1ms), quint64(1));
        QCOMPARE(Stats::ceilMilliseconds(1ms + 1ns), quint64(2));
        QCOMPARE(Stats::ceilMilliseconds(40ms), quint64(40));
    }

    void audioProducerGapsAreCountedAbove100ms()
    {
        Stats::AudioFlowCounters c;
        c.noteProduced(t(0)); // the first call has no predecessor
        c.noteProduced(t(20));
        c.noteProduced(t(40));
        QCOMPARE(c.flow().producerGaps, quint64(0));
        c.noteProduced(t(140)); // 100 ms is not above the limit
        QCOMPARE(c.flow().producerGaps, quint64(0));
        c.noteProduced(t(241)); // 101 ms
        QCOMPARE(c.flow().producerGaps, quint64(1));
        c.noteProduced(t(1241)); // 1 s
        const auto flow = c.flow();
        QCOMPARE(flow.producerGaps, quint64(2));
        QCOMPARE(flow.maxGapMs, quint32(1000));
    }
    void audioLateDeliveryNeedsMoreThanSixtyMillisecondsWaiting()
    {
        Stats::AudioFlowCounters c;
        c.noteDelivered(PacketBytes);       // 20 ms backlog: on time
        c.noteDelivered(3 * PacketBytes);   // 60 ms: on time
        QCOMPARE(c.flow().late, quint64(0));
        c.noteDelivered(4 * PacketBytes);   // 80 ms: late
        c.noteDelivered(12 * PacketBytes);  // 240 ms: late
        const auto flow = c.flow();
        QCOMPARE(flow.packets, quint64(4));
        QCOMPARE(flow.late, quint64(2));
        QCOMPARE(flow.maxBacklogMs, quint32(240));
    }
    void audioOverflowCounts()
    {
        Stats::AudioFlowCounters c;
        c.noteOverflow(4000);
        c.noteOverflow(100);
        QCOMPARE(c.flow().overflowEvents, quint64(2));
        QCOMPARE(c.flow().overflowBytes, quint64(4100));
    }
    void audioFlowsAdd()
    {
        Stats::AudioFlow a;
        a.packets = 1;
        a.maxGapMs = 5;
        Stats::AudioFlow b;
        b.packets = 2;
        b.maxGapMs = 9;
        b.late = 1;
        a += b;
        QCOMPARE(a.packets, quint64(3));
        QCOMPARE(a.maxGapMs, quint32(9));
        QCOMPARE(a.late, quint64(1));
    }

    void externalQueueCountsLateTakesAndOverflow()
    {
        ExternalAudioQueue queue;
        queue.setEnabled(true);
        int delivered = 0;
        const auto sink = [&delivered](const QByteArray &packet) {
            QCOMPARE(packet.size(), PacketBytes);
            ++delivered;
        };
        // steady: one packet in, one packet out
        for (int i = 0; i < 10; ++i) {
            queue.submit(pcm(1));
            QVERIFY(queue.deliver(sink));
        }
        QCOMPARE(queue.flow().packets, quint64(10));
        QCOMPARE(queue.flow().late, quint64(0));
        // the session loop falls behind: 8 packets (160 ms) pile up, then are taken one by one
        queue.submit(pcm(8));
        QVERIFY(queue.deliver(sink)); // backlog 160 ms: late
        QVERIFY(queue.deliver(sink)); // 140 ms: late
        QVERIFY(queue.deliver(sink)); // 120 ms: late
        QCOMPARE(queue.flow().late, quint64(3));
        QCOMPARE(queue.flow().maxBacklogMs, quint32(160));
        // and past half a second the oldest audio is dropped
        queue.submit(pcm(30)); // 600 ms on top of what is left: capped at 500 ms
        const auto flow = queue.flow();
        QCOMPARE(flow.overflowEvents, quint64(1));
        QVERIFY(flow.overflowBytes > 0);
        QCOMPARE(delivered, 13);
    }
    void externalQueueIgnoresWhatItDoesNotAccept()
    {
        ExternalAudioQueue queue;
        queue.submit(pcm(1)); // disabled
        QVERIFY(!queue.deliver([](const QByteArray &) {}));
        queue.setEnabled(true);
        queue.submit(QByteArray(3, 'x')); // not whole frames
        QVERIFY(!queue.deliver([](const QByteArray &) {}));
        QCOMPARE(queue.flow().packets, quint64(0));
    }

    void sampleRecordCarriesTheNewMeasurementsAsIntervalDeltas()
    {
        Stats::Snapshot previous;
        Stats::Snapshot current;
        Stats::FrameDelay delay;
        Stats::SendQueue queue;
        delay.record(2);
        queue.record(1000);
        previous.frameDelay = delay.counts();
        previous.sendQueueAtSend = queue.counts();
        for (int i = 0; i < 98; ++i) delay.record(3);
        delay.record(35);
        for (int i = 0; i < 99; ++i) queue.record(2000);
        queue.record(300 * 1024);
        current.frameDelay = delay.counts();
        current.sendQueueAtSend = queue.counts();
        previous.audio = Stats::AudioFlow{.packets = 100, .late = 1, .overflowEvents = 0, .overflowBytes = 0, .producerGaps = 0, .maxGapMs = 20, .maxBacklogMs = 40};
        current.audio = Stats::AudioFlow{.packets = 150, .late = 3, .overflowEvents = 1, .overflowBytes = 8192, .producerGaps = 2, .maxGapMs = 130, .maxBacklogMs = 90};
        const QJsonObject record = Stats::sampleRecord(current, previous, 1000, 7);
        const QJsonObject delayJson = record[QLatin1String("video")][QLatin1String("sendDelayMs")].toObject();
        QCOMPARE(delayJson[QLatin1String("n")].toInt(), 99);
        QCOMPARE(delayJson[QLatin1String("p50")].toInt(), 5);   // the 3 ms samples: bucket bound 5
        QCOMPARE(delayJson[QLatin1String("p99")].toInt(), 35);  // the 35 ms sample: bound 40 capped at the maximum
        QCOMPARE(delayJson[QLatin1String("max")].toInt(), 35);
        const QJsonObject queueJson = record[QLatin1String("link")][QLatin1String("sendQueueAtSendKiB")].toObject();
        QCOMPARE(queueJson[QLatin1String("n")].toInt(), 100);
        QCOMPARE(queueJson[QLatin1String("max")].toInt(), 300);
        QVERIFY(queueJson[QLatin1String("p99")].toInt() <= 300);
        const QJsonObject audio = record[QLatin1String("audio")].toObject();
        QCOMPARE(audio[QLatin1String("packets")].toInt(), 50);
        QCOMPARE(audio[QLatin1String("late")].toInt(), 2);
        QCOMPARE(audio[QLatin1String("overflows")].toInt(), 1);
        QCOMPARE(audio[QLatin1String("overflowKiB")].toInt(), 8);
        QCOMPARE(audio[QLatin1String("producerGaps")].toInt(), 2);
        QCOMPARE(audio[QLatin1String("maxGapMs")].toInt(), 130);
        QCOMPARE(audio[QLatin1String("maxBacklogMs")].toInt(), 90);
    }
    void sampleRecordIsUnchangedWithoutTheNewMeasurements()
    {
        const QJsonObject record = Stats::sampleRecord(Stats::Snapshot{}, Stats::Snapshot{}, 1000, 1);
        QVERIFY(!record.contains(QStringLiteral("audio")));
        QVERIFY(!record[QLatin1String("video")].toObject().contains(QStringLiteral("sendDelayMs")));
        QVERIFY(!record[QLatin1String("link")].toObject().contains(QStringLiteral("sendQueueAtSendKiB")));
    }
    void audioAppearsMidStreamAgainstAnAbsentPrevious()
    {
        Stats::Snapshot current;
        current.audio = Stats::AudioFlow{.packets = 5};
        const QJsonObject audio = Stats::sampleRecord(current, Stats::Snapshot{}, 1000, 1)[QLatin1String("audio")].toObject();
        QCOMPARE(audio[QLatin1String("packets")].toInt(), 5);
    }
};

QTEST_GUILESS_MAIN(DeliveryStatsTest)

#include "DeliveryStatsTest.moc"
