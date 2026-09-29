// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// STATS-S5: the KRDPCTL `stats` request (parse, clamp, invalid input), the `stats-sample` and
// `stats-event` records (shape; a 16-surface sample under 2 KiB), and StatsReporter's timer and
// event rules (STATS-PANEL-DESIGN.md §5, KRDPCTL-V2-CONTRACT.md (g)).

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTest>

#include "LayoutControl.h"
#include "RdpConnection.h"
#include "Server.h"
#include "StatsReporter.h"
#include "StatsRequest.h"
#include "VideoStream.h"

using namespace KRdp;
using namespace Qt::StringLiterals;

namespace
{
QJsonObject request(const QJsonObject &fields)
{
    QJsonObject record{{u"type"_s, u"stats"_s}, {u"v"_s, 1}};
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        record.insert(it.key(), it.value());
    }
    return record;
}

QSet<QString> keys(const QJsonObject &object)
{
    const auto list = object.keys();
    return QSet<QString>(list.cbegin(), list.cend());
}

/// The busiest sample a host can send: every optional field present, 16 surfaces, large values.
Stats::Snapshot busy(quint64 scale)
{
    Stats::Snapshot s;
    s.codec = u"hevc"_s;
    s.hardware = false;
    s.preset = u"superfast"_s;
    s.chroma = u"420"_s;
    s.size = QSize(16384, 2160);
    s.frameRate = 60;
    s.frameRateCap = 60;
    s.quality = 100;
    s.qualityCap = 100;
    s.targetKbps = 999999;
    s.bytesSent = 125000000ULL * scale;
    s.framesSent = 960 * scale;
    s.framesAcked = 960 * scale;
    s.coalesced = 960 * scale;
    s.keyframes = 960 * scale;
    s.keyframeRequests = 960 * scale;
    s.encoderRestarts = 99 * scale;
    s.framesEncoded = 960 * scale;
    s.framesSkipped = 960 * scale;
    s.encodeLoadP95 = 0.8765;
    s.encodeMs = 123.456;
    s.inFlight = 256;
    s.window = 256;
    s.windowBytes = 99999999;
    s.ackLatencyMs = 1234.56;
    s.acksSuspended = true;
    s.throttled = true;
    s.qoeFrames = 960 * scale;
    s.qoeDecodeMs = 9600 * scale;
    s.qoeRenderMs = 9600 * scale;
    s.rttMs = 1234.56;
    s.rttMinMs = 1234.56;
    s.rttVarMs = 1234.56;
    s.sentKbps = 9999999;
    s.sentSamples = 99999;
    s.capacityKbps = 9999999;
    s.capacitySource = u"probe"_s;
    s.appLimited = false;
    s.retransmits = 99999 * scale;
    s.sendQueueBytes = 99999999;
    s.congested = true;
    s.slow = true;
    s.mode = u"prefer"_s;
    s.adaptive = true;
    s.guardState = u"holding"_s;
    s.limit = u"encoder"_s; // AUD-FIX13: the longest value
    s.heldBack = {u"hevc"_s, u"av1"_s};
    s.retryInS = 3599;
    for (int i = 0; i < 16; ++i) {
        s.surfaces.append({960 * scale, 960 * scale, 960 * scale});
    }
    return s;
}
}

class StatsRequestTest : public QObject
{
    Q_OBJECT

    struct Fixture {
        Server server;
        RdpConnection connection{&server, -1};
        Fixture()
        {
            QCoreApplication::removePostedEvents(&connection, QEvent::MetaCall); // no socket to initialise
        }
        VideoStream *stream()
        {
            return connection.videoStream();
        }
    };

private Q_SLOTS:
    void parsesSubscribeAndUnsubscribe()
    {
        using StatsRequest::Action;
        QCOMPARE(StatsRequest::parse(request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, 2}})), (StatsRequest::Request{Action::Subscribe, 2}));
        // No rate: 1 Hz. A fraction is rounded; the reporter clamps what is left.
        QCOMPARE(StatsRequest::parse(request({{u"action"_s, u"subscribe"_s}})), (StatsRequest::Request{Action::Subscribe, 1}));
        QCOMPARE(StatsRequest::parse(request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, 2.6}}))->rateHz, 3);
        QCOMPARE(StatsRequest::parse(request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, 0.2}}))->rateHz, 0);
        QCOMPARE(StatsRequest::parse(request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, 60}}))->rateHz, 60);
        QCOMPARE(StatsRequest::parse(request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, 1e300}}))->rateHz, std::numeric_limits<int>::max());
        // Unsubscribe ignores a rate, and unknown fields are ignored everywhere.
        QCOMPARE(StatsRequest::parse(request({{u"action"_s, u"unsubscribe"_s}, {u"rateHz"_s, u"x"_s}})), (StatsRequest::Request{Action::Unsubscribe, 1}));
        QVERIFY(StatsRequest::parse(request({{u"action"_s, u"subscribe"_s}, {u"future"_s, true}})));
    }

    void rejectsInvalidInput_data()
    {
        QTest::addColumn<QJsonObject>("record");
        QTest::newRow("no action") << request({{u"rateHz"_s, 2}});
        QTest::newRow("unknown action") << request({{u"action"_s, u"pause"_s}});
        QTest::newRow("action not a string") << request({{u"action"_s, 1}});
        QTest::newRow("action wrong case") << request({{u"action"_s, u"Subscribe"_s}});
        QTest::newRow("rate a string") << request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, u"2"_s}});
        QTest::newRow("rate a bool") << request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, true}});
        QTest::newRow("rate null") << request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, QJsonValue::Null}});
        QTest::newRow("rate zero") << request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, 0}});
        QTest::newRow("rate negative") << request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, -2}});
        QTest::newRow("rate an array") << request({{u"action"_s, u"subscribe"_s}, {u"rateHz"_s, QJsonArray{2}}});
    }
    void rejectsInvalidInput()
    {
        QFETCH(QJsonObject, record);
        QVERIFY(!StatsRequest::parse(record));
        const auto error = StatsRequest::invalidRecord();
        QCOMPARE(error.value(u"type"_s).toString(), u"error"_s);
        QCOMPARE(error.value(u"code"_s).toString(), u"invalid"_s);
    }

    void clampsTheRate()
    {
        QCOMPARE(StatsReporter::clampRate(0, false), 1);
        QCOMPARE(StatsReporter::clampRate(1, false), 1);
        QCOMPARE(StatsReporter::clampRate(3, false), 3);
        QCOMPARE(StatsReporter::clampRate(4, false), 4);
        QCOMPARE(StatsReporter::clampRate(60, false), 4);
        // A slow link: 1 Hz whatever was asked.
        QCOMPARE(StatsReporter::clampRate(4, true), 1);
        QCOMPARE(StatsReporter::clampRate(1, true), 1);
    }

    // The request on a real VideoStream: the reply carries the rate really used.
    void applyAnswersWithTheClampedRate()
    {
        Fixture f;
        QString log;
        auto reply = StatsRequest::apply(*f.stream(), {StatsRequest::Action::Subscribe, 60}, &log);
        QCOMPARE(reply, (QJsonObject{{u"type"_s, u"stats"_s}, {u"v"_s, 1}, {u"state"_s, u"subscribed"_s}, {u"rateHz"_s, 4}}));
        QVERIFY(f.stream()->statsSubscribed());
        QVERIFY(log.contains(u"4 Hz"_s));
        reply = StatsRequest::apply(*f.stream(), {StatsRequest::Action::Subscribe, 0});
        QCOMPARE(reply.value(u"rateHz"_s).toInt(), 1); // re-rated, still subscribed
        reply = StatsRequest::apply(*f.stream(), {StatsRequest::Action::Unsubscribe, 1}, &log);
        QCOMPARE(reply, (QJsonObject{{u"type"_s, u"stats"_s}, {u"v"_s, 1}, {u"state"_s, u"unsubscribed"_s}}));
        QVERIFY(!f.stream()->statsSubscribed());
        // Unsubscribing twice is harmless and answers the same.
        QCOMPARE(StatsRequest::apply(*f.stream(), {StatsRequest::Action::Unsubscribe, 1}).value(u"state"_s).toString(), u"unsubscribed"_s);
    }

    // The design's shape (STATS-PANEL-DESIGN.md §5), counters as deltas, 16 surfaces < 2 KiB.
    void sampleShapeAndSize()
    {
        const auto previous = busy(1);
        const auto current = busy(2);
        const auto record = Stats::sampleRecord(current, previous, 1000, 42);
        QCOMPARE(keys(record), (QSet<QString>{u"type"_s, u"v"_s, u"seq"_s, u"intervalMs"_s, u"video"_s, u"flow"_s, u"link"_s, u"policy"_s, u"surfaces"_s}));
        QCOMPARE(record.value(u"type"_s).toString(), u"stats-sample"_s);
        QCOMPARE(record.value(u"v"_s).toInt(), 1);
        QCOMPARE(record.value(u"seq"_s).toInteger(), 42);
        QCOMPARE(record.value(u"intervalMs"_s).toInteger(), 1000);
        QVERIFY(!record.contains(u"requestId"_s)); // unsolicited
        const auto video = record.value(u"video"_s).toObject();
        const QSet<QString> designVideo{u"codec"_s, u"backend"_s, u"preset"_s, u"chroma"_s, u"width"_s, u"height"_s, u"frameRate"_s,
                                        u"frameRateCap"_s, u"quality"_s, u"qualityCap"_s, u"targetKbps"_s, u"sentKbps"_s, u"framesSent"_s,
                                        u"framesAcked"_s, u"coalesced"_s, u"keyframes"_s, u"keyframeRequests"_s, u"encoderRestarts"_s,
                                        u"encodeLoadP95"_s, u"encodeMs"_s};
        // Additions to the design (contract (g)): the encoder's own frame counts.
        QCOMPARE(keys(video), designVideo + QSet<QString>({u"framesEncoded"_s, u"framesSkipped"_s}));
        QCOMPARE(video.value(u"codec"_s).toString(), u"hevc"_s);
        QCOMPARE(video.value(u"backend"_s).toString(), u"software"_s);
        QCOMPARE(video.value(u"framesSent"_s).toInt(), 960); // a delta, not the total
        QCOMPARE(video.value(u"encoderRestarts"_s).toInt(), 99);
        QCOMPARE(video.value(u"sentKbps"_s).toInteger(), 1000000); // 125 MB in 1 s
        QCOMPARE(video.value(u"encodeLoadP95"_s).toDouble(), 0.88);
        QCOMPARE(video.value(u"encodeMs"_s).toDouble(), 123.5);
        const auto flow = record.value(u"flow"_s).toObject();
        QCOMPARE(keys(flow),
                 (QSet<QString>{u"inFlight"_s, u"window"_s, u"windowKiB"_s, u"ackLatencyMs"_s, u"acksSuspended"_s, u"throttled"_s,
                                u"clientDecodeMs"_s, u"clientRenderMs"_s}));
        QCOMPARE(flow.value(u"clientDecodeMs"_s).toDouble(), 10.0);
        const auto link = record.value(u"link"_s).toObject();
        QCOMPARE(keys(link),
                 (QSet<QString>{u"rttMs"_s, u"rttMinMs"_s, u"rttVarMs"_s, u"sentKbps"_s, u"sentSamples"_s, u"capacityKbps"_s,
                                u"capacitySource"_s, u"appLimited"_s, u"retransmits"_s, u"sendQueueKiB"_s, u"congested"_s, u"slow"_s}));
        QCOMPARE(link.value(u"capacitySource"_s).toString(), u"probe"_s);
        QCOMPARE(link.value(u"retransmits"_s).toInteger(), 99999);
        const auto policy = record.value(u"policy"_s).toObject();
        QCOMPARE(keys(policy), (QSet<QString>{u"mode"_s, u"adaptive"_s, u"cpuGuard"_s, u"limit"_s}));
        QCOMPARE(policy.value(u"limit"_s).toString(), u"encoder"_s);
        QCOMPARE(policy.value(u"cpuGuard"_s).toObject(),
                 (QJsonObject{{u"state"_s, u"holding"_s}, {u"heldBack"_s, QJsonArray{u"hevc"_s, u"av1"_s}}, {u"retryInS"_s, 3599}}));
        const auto surfaces = record.value(u"surfaces"_s).toArray();
        QCOMPARE(surfaces.size(), 16);
        QCOMPARE(surfaces.at(15).toObject(),
                 (QJsonObject{{u"index"_s, 15}, {u"framesSent"_s, 960}, {u"coalesced"_s, 960}, {u"keyframes"_s, 960}}));

        const QByteArray json = QJsonDocument(record).toJson(QJsonDocument::Compact);
        qInfo() << "busiest 16-surface sample:" << json.size() << "bytes";
        QVERIFY2(json.size() < 2048, json.constData());
        QVERIFY(LayoutControl::frame(record).size() < 2048 + 16);
    }

    void unknownFieldsAreLeftOut()
    {
        // A fresh stream: no codec yet, no backend, no RTT, no policy - those keys are absent.
        Stats::Snapshot empty;
        empty.mode = u"auto"_s;
        const auto record = Stats::sampleRecord(empty, empty, 500, 0);
        const auto video = record.value(u"video"_s).toObject();
        for (const auto &key : {u"codec"_s, u"backend"_s, u"preset"_s, u"chroma"_s, u"width"_s, u"targetKbps"_s, u"encoderRestarts"_s,
                               u"framesSkipped"_s, u"encodeLoadP95"_s, u"encodeMs"_s}) {
            QVERIFY2(!video.contains(key), qPrintable(key));
        }
        QCOMPARE(video.value(u"sentKbps"_s).toInt(-1), 0);
        const auto link = record.value(u"link"_s).toObject();
        // AUD-FIX12: no capacity without evidence (and never the send rate standing in for it).
        for (const auto &key : {u"rttMs"_s, u"rttMinMs"_s, u"rttVarMs"_s, u"sentKbps"_s, u"capacityKbps"_s, u"capacitySource"_s, u"appLimited"_s,
                                u"retransmits"_s, u"sendQueueKiB"_s, u"goodputKbps"_s}) {
            QVERIFY2(!link.contains(key), qPrintable(key));
        }
        QVERIFY(!record.value(u"flow"_s).toObject().contains(u"clientDecodeMs"_s));
        QVERIFY(!record.value(u"policy"_s).toObject().contains(u"adaptive"_s));
        QCOMPARE(record.value(u"policy"_s).toObject().value(u"cpuGuard"_s).toObject(), (QJsonObject{{u"state"_s, u"ok"_s}}));
        QCOMPARE(record.value(u"policy"_s).toObject().value(u"limit"_s).toString(), u"none"_s); // AUD-FIX13: always present
        QCOMPARE(record.value(u"surfaces"_s).toArray().size(), 0);
        // A counter that went down (the policy restarted its count) counts from zero.
        Stats::Snapshot before = empty, after = empty;
        before.encoderRestarts = 5;
        after.encoderRestarts = 1;
        QCOMPARE(Stats::sampleRecord(after, before, 500, 1).value(u"video"_s).toObject().value(u"encoderRestarts"_s).toInt(), 1);
    }

    // AUD-FIX12: the capacity estimate takes only network-limited TCP delivery-rate samples, the
    // highest of the last 10 s; application-limited ones (a LAN with room to spare) give none.
    void capacityNeedsNetworkLimitedSamples()
    {
        using namespace std::chrono_literals;
        Stats::CapacityEstimate estimate;
        const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
        for (int i = 0; i < 20; ++i) estimate.sample(t0 + i * 500ms, 125'000'000, true); // 1 Gbit/s, app-limited
        QVERIFY(!estimate.kbps(t0 + 10s));
        estimate.sample(t0 + 10s, 750'000, false); // 6 Mbit/s, network-limited (a tbf queue)
        estimate.sample(t0 + 11s, 700'000, false);
        QCOMPARE(estimate.kbps(t0 + 11s), std::optional<quint32>(6000));
        estimate.sample(t0 + 12s, 125'000'000, true);
        QCOMPARE(estimate.kbps(t0 + 12s), std::optional<quint32>(6000));
        // Ten seconds later the evidence is gone.
        QCOMPARE(estimate.kbps(t0 + 20s + 500ms), std::optional<quint32>(5600));
        QVERIFY(!estimate.kbps(t0 + 22s));
        estimate.sample(t0 + 23s, 0, false); // no sample yet
        QVERIFY(!estimate.kbps(t0 + 23s));
    }

    void eventShape()
    {
        const auto record = Stats::eventRecord(Stats::EventKind::CpuGuard, 43, {u"CPU guard: software av1 at 82% ..."_s, u"hevc"_s, false});
        QCOMPARE(record,
                 (QJsonObject{{u"type"_s, u"stats-event"_s}, {u"v"_s, 1}, {u"seq"_s, 43}, {u"kind"_s, u"cpu-guard"_s},
                              {u"reason"_s, u"CPU guard: software av1 at 82% ..."_s}, {u"codec"_s, u"hevc"_s}, {u"backend"_s, u"software"_s}}));
        const QStringList kinds{u"codec"_s, u"settings"_s, u"keyframe"_s, u"coalesce"_s, u"slow-link"_s, u"cpu-guard"_s, u"throttle"_s, u"quality"_s,
                                u"client-limited"_s};
        for (int i = 0; i < Stats::EventKindCount; ++i) {
            QCOMPARE(QString::fromLatin1(Stats::eventKindName(Stats::EventKind(i))), kinds.at(i));
        }
        const auto bare = Stats::eventRecord(Stats::EventKind::Throttle, 1, {u"frame rate 30 fps"_s, {}, std::nullopt});
        QVERIFY(!bare.contains(u"codec"_s) && !bare.contains(u"backend"_s));
    }

    void presetNames()
    {
        QCOMPARE(Stats::encoderPresetName(u"hevc"_s, 0), u"veryfast"_s);
        QCOMPARE(Stats::encoderPresetName(u"hevc"_s, 2), u"ultrafast"_s);
        QCOMPARE(Stats::encoderPresetName(u"av1"_s, 0), u"M10"_s);
        QCOMPARE(Stats::encoderPresetName(u"av1"_s, 2), u"M11"_s); // SVT-AV1 low delay: 12 is 11
        QCOMPARE(Stats::encoderPresetName(u"avc"_s, 0), u"ultrafast"_s);
    }

    // The reporter: no timer and no work while unsubscribed; samples at the rate; slow link 1 Hz.
    void reporterTimerAndRate()
    {
        int built = 0;
        QList<QJsonObject> sent;
        StatsReporter reporter(
            [&built] {
                ++built;
                return Stats::Snapshot{};
            },
            [&sent](const QJsonObject &record) {
                sent << record;
                return true;
            });
        QVERIFY(!reporter.subscribed());
        QVERIFY(!reporter.timerActive());
        QCOMPARE(reporter.rateHz(), 0);
        bool called = false;
        reporter.event(Stats::EventKind::Codec, [&called] {
            called = true;
            return Stats::EventDetail{};
        });
        QVERIFY(!called); // unsubscribed: the detail is never even built
        QTest::qWait(300);
        QCOMPARE(built, 0);
        QVERIFY(sent.isEmpty());

        QCOMPARE(reporter.subscribe(4), 4);
        QVERIFY(reporter.timerActive());
        QCOMPARE(built, 1); // the baseline for the first sample's deltas
        QTest::qWait(1120);
        const auto samples = sent.size();
        QVERIFY2(samples >= 3 && samples <= 5, qPrintable(QString::number(samples)));
        QCOMPARE(sent.first().value(u"type"_s).toString(), u"stats-sample"_s);
        QVERIFY(std::abs(sent.last().value(u"intervalMs"_s).toInt() - 250) <= 60);
        for (int i = 1; i < sent.size(); ++i) {
            QCOMPARE(sent.at(i).value(u"seq"_s).toInteger(), sent.at(i - 1).value(u"seq"_s).toInteger() + 1);
        }

        // A slow link: 1 Hz.
        reporter.setSlowLink(true);
        QCOMPARE(reporter.rateHz(), 1);
        sent.clear();
        QTest::qWait(1150);
        QVERIFY2(sent.size() == 1, qPrintable(QString::number(sent.size())));
        reporter.setSlowLink(false);
        QCOMPARE(reporter.rateHz(), 4);

        reporter.event(Stats::EventKind::Codec, [&called] {
            called = true;
            return Stats::EventDetail{u"initial choice"_s, u"avc"_s, true};
        });
        QVERIFY(called);
        QCOMPARE(sent.last().value(u"type"_s).toString(), u"stats-event"_s);
        QCOMPARE(sent.last().value(u"seq"_s).toInteger(), sent.at(sent.size() - 2).value(u"seq"_s).toInteger() + 1); // one sequence

        reporter.unsubscribe();
        QVERIFY(!reporter.timerActive());
        sent.clear();
        const int before = built;
        QTest::qWait(600);
        QVERIFY(sent.isEmpty());
        QCOMPARE(built, before);
    }

    void reporterRateLimitsNoisyEventsAndEndsWithTheChannel()
    {
        bool open = true;
        QList<QJsonObject> sent;
        StatsReporter reporter([] { return Stats::Snapshot{}; },
                               [&](const QJsonObject &record) {
                                   if (!open) return false;
                                   sent << record;
                                   return true;
                               });
        reporter.subscribe(1);
        for (int i = 0; i < 50; ++i) {
            reporter.event(Stats::EventKind::Coalesce, [] { return Stats::EventDetail{u"dropped"_s, {}, std::nullopt}; });
            reporter.event(Stats::EventKind::KeyFrame, [] { return Stats::EventDetail{u"keyframe"_s, {}, std::nullopt}; });
            reporter.event(Stats::EventKind::Quality, [] { return Stats::EventDetail{u"quality"_s, {}, std::nullopt}; });
        }
        const auto count = [&sent](const QString &kind) {
            return std::count_if(sent.cbegin(), sent.cend(), [&kind](const QJsonObject &r) { return r.value(u"kind"_s).toString() == kind; });
        };
        QCOMPARE(count(u"coalesce"_s), 1);
        QCOMPARE(count(u"keyframe"_s), 1);
        QCOMPARE(count(u"quality"_s), 50);
        QTest::qWait(int(StatsReporter::EventMinIntervalMs) + 50);
        reporter.event(Stats::EventKind::Coalesce, [] { return Stats::EventDetail{u"dropped"_s, {}, std::nullopt}; });
        QCOMPARE(count(u"coalesce"_s), 2);
        // The channel went away: the next sample ends the subscription and its timer.
        open = false;
        reporter.sendSample();
        QVERIFY(!reporter.subscribed());
        QVERIFY(!reporter.timerActive());
    }
};

QTEST_GUILESS_MAIN(StatsRequestTest)
#include "StatsRequestTest.moc"
