// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX2 F1 on a real VideoStream (no socket): the `codec` request is answered from the
// encoders the host really has, the codec sessions are built for follows, and an encoder that
// turns out not to produce the private codec moves the connection off it at once.

#include "CodecRequest.h"
#include "RdpConnection.h"
#include "Server.h"
#include "StatsReporter.h"
#include "VideoStream.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace KRdp;
using CodecPolicy::Family;

namespace
{
CodecPolicy::Encoders hal()
{
    CodecPolicy::Encoders e;
    e.avc = {true, true};
    e.hevc = {true, false};
    e.av1 = {true, false};
    return e;
}
CodecPolicy::Encoders sol()
{
    CodecPolicy::Encoders e;
    e.avc = {false, true};
    return e;
}
CodecPolicy::Encoders softwareOnly() // WS-E: libx264, libx265, SVT-AV1, no GPU
{
    CodecPolicy::Encoders e;
    e.avc = {false, true};
    e.hevc = {false, true};
    e.av1 = {false, true};
    return e;
}
}

class VideoStreamCodecTest : public QObject
{
    Q_OBJECT

    struct Fixture {
        Server server;
        RdpConnection connection{&server, -1};
        Fixture()
        {
            QCoreApplication::removePostedEvents(&connection, QEvent::MetaCall); // no socket to initialise
        }
        VideoStream *stream() { return connection.videoStream(); }
    };

private Q_SLOTS:
    // AUD-FIX10: a Refresh Rect asks every surface for a keyframe, at most once per second per
    // client, and nothing while the client has suppressed output.
    void refreshRequestsKeyFramesRateLimited()
    {
        Fixture f;
        QSignalSpy requested(f.stream(), &VideoStream::keyFrameRequested);
        f.stream()->setEnabled(false);
        QVERIFY(!f.stream()->requestRefresh());
        QCOMPARE(requested.count(), 0);
        f.stream()->setEnabled(true);
        QVERIFY(f.stream()->requestRefresh());
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().first().toInt(), 0);
        QVERIFY(!f.stream()->requestRefresh());
        QCOMPARE(requested.count(), 1);
        QTest::qWait(int(VideoStream::RefreshMinInterval.count()) + 50);
        QVERIFY(f.stream()->requestRefresh());
        QCOMPARE(requested.count(), 2);
    }

    // Sol: the client asks for HEVC+AV1, the host has neither: AVC, and nothing says 0x8001.
    void noEncoderMeansAvc()
    {
        Fixture f;
        f.stream()->setEncoderPolicy(sol(), CodecPolicy::SoftwareEncoding::Auto);
        const auto d = f.stream()->setPrivateCodecPolicy({VideoCodec::Av1, VideoCodec::Hevc}, true);
        QCOMPARE(d.choice.family, Family::Avc);
        QVERIFY(!d.choice.hardware);
        QVERIFY(!f.stream()->negotiatedCodec()); // caps decide the AVC flavour
        QCOMPARE(f.stream()->codecForSessions(), VideoCodec::Avc444v2);
    }

    void hardwareCodecIsSelected()
    {
        Fixture f;
        f.stream()->setEncoderPolicy(hal(), CodecPolicy::SoftwareEncoding::Auto);
        auto d = f.stream()->setPrivateCodecPolicy({VideoCodec::Hevc}, true);
        QCOMPARE(d.choice, (CodecPolicy::Choice{Family::Hevc, true}));
        QCOMPARE(f.stream()->negotiatedCodec(), VideoCodec::Hevc);
        QCOMPARE(f.stream()->codecForSessions(), VideoCodec::Hevc);
        // An empty list is plain AVC again.
        d = f.stream()->setPrivateCodecPolicy({}, true);
        QCOMPARE(d.choice.family, Family::Avc);
        QVERIFY(!f.stream()->negotiatedCodec());
    }

    // The session found its encoder cannot do HEVC after all: off it before the stream starts.
    void unavailableEncoderFallsBackAtOnce()
    {
        Fixture f;
        f.stream()->setEncoderPolicy(hal(), CodecPolicy::SoftwareEncoding::Auto);
        QCOMPARE(f.stream()->setPrivateCodecPolicy({VideoCodec::Av1, VideoCodec::Hevc}, true).choice.family, Family::Av1);
        QSignalSpy changed(f.stream(), &VideoStream::negotiatedCodecChanged);
        f.stream()->privateCodecUnavailable(VideoCodec::Av1); // no switch interval for this
        QCOMPARE(f.stream()->negotiatedCodec(), VideoCodec::Hevc);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().first().value<VideoCodec>(), VideoCodec::Hevc);
        f.stream()->privateCodecUnavailable(VideoCodec::Hevc);
        QVERIFY(!f.stream()->negotiatedCodec());
        QCOMPARE(changed.size(), 2);
        QCOMPARE(changed.last().first().value<VideoCodec>(), VideoCodec::Avc444v2); // what sessions rebuild with
        // AVC is never "unavailable": nothing happens.
        f.stream()->privateCodecUnavailable(VideoCodec::Avc420);
        QCOMPARE(changed.size(), 2);
    }

    void neverKeepsSoftwareOnlyCodecsOut()
    {
        Fixture f;
        auto e = sol();
        e.av1 = {false, true}; // a future software AV1
        f.stream()->setEncoderPolicy(e, CodecPolicy::SoftwareEncoding::Never);
        QCOMPARE(f.stream()->setPrivateCodecPolicy({VideoCodec::Av1}, true).choice.family, Family::Avc);
        f.stream()->setEncoderPolicy(e, CodecPolicy::SoftwareEncoding::Prefer);
        QCOMPARE(f.stream()->setPrivateCodecPolicy({VideoCodec::Av1}, true).choice, (CodecPolicy::Choice{Family::Av1, false}));
        QCOMPARE(f.stream()->negotiatedCodec(), VideoCodec::Av1);
    }

    // WS-E: software AV1 chosen: the sessions hear the backend, preset and 30 fps cap before
    // the codec change that restarts their encoders.
    void softwareCodecSettingsReachSessionsFirst()
    {
        Fixture f;
        QStringList order;
        connect(f.stream(), &VideoStream::encoderSettingsChanged, f.stream(), [&order](const CodecPolicy::EncoderSettings &) {
            order << QStringLiteral("settings");
        });
        connect(f.stream(), &VideoStream::negotiatedCodecChanged, f.stream(), [&order](VideoCodec) {
            order << QStringLiteral("codec");
        });
        QSignalSpy rate(f.stream(), &VideoStream::requestedFrameRateChanged);
        f.stream()->setEncoderPolicy(softwareOnly(), CodecPolicy::SoftwareEncoding::Prefer);
        f.stream()->setQualityCap(70);
        QCOMPARE(f.stream()->requestedFrameRate(), 60u);
        const auto d = f.stream()->setPrivateCodecPolicy({VideoCodec::Av1, VideoCodec::Hevc}, true);
        QCOMPARE(d.choice, (CodecPolicy::Choice{Family::Av1, false}));
        QCOMPARE(order, (QStringList{QStringLiteral("settings"), QStringLiteral("codec")}));
        // OPT-063: SVT-AV1 reopens for every rate change, so it opens in quality mode (the client's Quality as a constant QP,
        // no target bitrate); only a slow link gives it a bitrate to hold.
        QCOMPARE(f.stream()->encoderSettings(), (CodecPolicy::EncoderSettings{false, CodecPolicy::Preset::Efficient, 0, 30}));
        QCOMPARE(f.stream()->requestedFrameRate(), 30u);
        QCOMPARE(rate.size(), 1);
        // Back to AVC only: full rate again.
        f.stream()->setPrivateCodecPolicy({}, true);
        QCOMPARE(f.stream()->requestedFrameRate(), 60u);
        QCOMPARE(f.stream()->encoderSettings()->maxFrameRate, 0);
    }

    // The backend the encoder really opened on wins over the one chosen: KPipeWire's H.264
    // fallback from h264_vaapi to libx264 is followed and pushed to the own client.
    void reportedBackendIsFollowed()
    {
        Fixture f;
        auto e = hal();
        e.hevc = {};
        e.av1 = {};
        f.stream()->setEncoderPolicy(e, CodecPolicy::SoftwareEncoding::Auto);
        QCOMPARE(f.stream()->setPrivateCodecPolicy({VideoCodec::Hevc}, true).choice, (CodecPolicy::Choice{Family::Avc, true}));
        QVERIFY(f.stream()->encoderSettings()->hardware);
        QSignalSpy settings(f.stream(), &VideoStream::encoderSettingsChanged);
        f.stream()->encoderBackendReported(VideoCodec::Avc444v2, true); // as chosen: nothing to do
        QCOMPARE(settings.size(), 0);
        f.stream()->encoderBackendReported(VideoCodec::Hevc, false); // not the current codec: stale
        QCOMPARE(settings.size(), 0);
        // The `codec` push with backend "software" (no channel here, so it is dropped with a warning).
        QTest::ignoreMessage(QtWarningMsg, "KRDPCTL: dropping a \"codec\" record, the channel is not open");
        f.stream()->encoderBackendReported(VideoCodec::Avc444v2, false);
        QCOMPARE(settings.size(), 1);
        QVERIFY(!f.stream()->encoderSettings()->hardware);
        f.stream()->encoderBackendReported(VideoCodec::Avc444v2, false); // reported once
        QCOMPARE(settings.size(), 1);
    }

    // STATS-S5: nothing runs for stats until a client subscribes: no timer, and the events the
    // codec policy raises never reach the sink (their text is not even built).
    void noStatsTimerWhileUnsubscribed()
    {
        Fixture f;
        QList<QJsonObject> sent;
        f.stream()->statsReporter()->setSink([&sent](const QJsonObject &record) {
            sent << record;
            return true;
        });
        QVERIFY(!f.stream()->statsSubscribed());
        QVERIFY(!f.stream()->statsReporter()->timerActive());
        f.stream()->setEncoderPolicy(softwareOnly(), CodecPolicy::SoftwareEncoding::Prefer);
        f.stream()->setPrivateCodecPolicy({VideoCodec::Av1, VideoCodec::Hevc}, true); // codec, settings, frame rate
        f.stream()->privateCodecUnavailable(VideoCodec::Av1);
        QTest::qWait(300);
        QVERIFY(sent.isEmpty());
        QVERIFY(!f.stream()->statsReporter()->timerActive());

        QSignalSpy changed(f.stream(), &VideoStream::statsSubscriptionChanged);
        QCOMPARE(f.stream()->setStatsSubscription(2), 2);
        QVERIFY(f.stream()->statsReporter()->timerActive());
        QCOMPARE(changed.size(), 1);
        QCOMPARE(f.stream()->setStatsSubscription(0), 0);
        QVERIFY(!f.stream()->statsReporter()->timerActive());
        QCOMPARE(changed.size(), 2);
        QCOMPARE(changed.last().first().toBool(), false);
    }

    // Without a channel (the client hung up) the first sample ends the subscription.
    void subscriptionEndsWithTheChannel()
    {
        Fixture f;
        QVERIFY(!f.connection.hasControlChannel());
        QCOMPARE(f.stream()->setStatsSubscription(4), 4);
        QTRY_VERIFY_WITH_TIMEOUT(!f.stream()->statsSubscribed(), 1000);
        QVERIFY(!f.stream()->statsReporter()->timerActive());
    }

    // Codec switches, backend reports, frame-rate changes and CPU-guard steps become events.
    void statsEventsOnCodecSwitchesAndGuardSteps()
    {
        Fixture f;
        QList<QJsonObject> sent;
        f.stream()->statsReporter()->setSink([&sent](const QJsonObject &record) {
            sent << record;
            return true;
        });
        QCOMPARE(f.stream()->setStatsSubscription(1), 1);
        const auto events = [&sent](const QString &kind) {
            QList<QJsonObject> matching;
            for (const auto &record : std::as_const(sent)) {
                if (record.value(QLatin1String("type")).toString() == QLatin1String("stats-event") && record.value(QLatin1String("kind")).toString() == kind) {
                    matching << record;
                }
            }
            return matching;
        };

        // The client's `codec` request: software AV1 (prefer) at 30 fps.
        f.stream()->setEncoderPolicy(softwareOnly(), CodecPolicy::SoftwareEncoding::Prefer);
        f.stream()->setPrivateCodecPolicy({VideoCodec::Av1, VideoCodec::Hevc}, true);
        QCOMPARE(events(QStringLiteral("codec")).size(), 1);
        QCOMPARE(events(QStringLiteral("codec")).last().value(QLatin1String("codec")).toString(), QStringLiteral("av1"));
        QCOMPARE(events(QStringLiteral("codec")).last().value(QLatin1String("backend")).toString(), QStringLiteral("software"));
        QCOMPARE(events(QStringLiteral("throttle")).size(), 1);
        QVERIFY(events(QStringLiteral("throttle")).last().value(QLatin1String("reason")).toString().contains(QLatin1String("30 fps")));

        // The encoder turns out unable: an immediate switch, with its reason.
        f.stream()->privateCodecUnavailable(VideoCodec::Av1);
        auto codec = events(QStringLiteral("codec"));
        QCOMPARE(codec.size(), 2);
        QCOMPARE(codec.last().value(QLatin1String("reason")).toString(), QStringLiteral("encoder unavailable"));
        QCOMPARE(codec.last().value(QLatin1String("codec")).toString(), QStringLiteral("hevc"));

        // The CPU guard: a preset step (no codec change) and a step away from the codec.
        CodecPolicy::Decision preset;
        preset.choice = {CodecPolicy::Family::Hevc, false};
        preset.settings = {false, CodecPolicy::Preset::Balanced, f.stream()->encoderSettings()->targetKbps, 30};
        preset.settingsChanged = true;
        preset.restartsEncoder = true;
        preset.settingsReason = QStringLiteral("CPU guard: software hevc at 82% of the frame budget (p95); preset balanced");
        f.stream()->applyCodecDecision(preset);
        QCOMPARE(events(QStringLiteral("cpu-guard")).size(), 1);
        QCOMPARE(events(QStringLiteral("cpu-guard")).last().value(QLatin1String("reason")).toString(), preset.settingsReason);
        QCOMPARE(events(QStringLiteral("codec")).size(), 2); // not a codec switch

        CodecPolicy::Decision away = CodecPolicy::makeDecision({CodecPolicy::Family::Avc, false}, true,
                                                               QStringLiteral("CPU guard: software hevc at 91% of the frame budget (p95); not retried for 5 min"));
        away.settings = {false, CodecPolicy::Preset::Efficient, 0, 0};
        away.settingsChanged = true;
        f.stream()->applyCodecDecision(away);
        codec = events(QStringLiteral("codec"));
        QCOMPARE(codec.size(), 3);
        QVERIFY(codec.last().value(QLatin1String("reason")).toString().startsWith(QLatin1String("CPU guard")));
        QCOMPARE(events(QStringLiteral("cpu-guard")).size(), 2);
        QCOMPARE(events(QStringLiteral("throttle")).size(), 2); // back to 60 fps

        // A bitrate reopen that is not the guard: `settings`.
        CodecPolicy::Decision bitrate;
        bitrate.choice = {CodecPolicy::Family::Avc, false};
        bitrate.settings = {false, CodecPolicy::Preset::Efficient, 0, 0};
        bitrate.settings.targetKbps = 3000;
        bitrate.settingsChanged = true;
        bitrate.restartsEncoder = true;
        bitrate.settingsReason = QStringLiteral("target bitrate 3000 kbit/s");
        f.stream()->applyCodecDecision(bitrate);
        QCOMPARE(events(QStringLiteral("settings")).size(), 1);

        // The encoder that really runs: a backend report is an event once per change.
        f.stream()->encoderBackendReported(VideoCodec::Avc420, false);
        f.stream()->encoderBackendReported(VideoCodec::Avc420, false);
        codec = events(QStringLiteral("codec"));
        QCOMPARE(codec.size(), 4);
        QCOMPARE(codec.last().value(QLatin1String("reason")).toString(), QStringLiteral("encoder backend: software"));

        // Events and samples share one sequence.
        for (int i = 1; i < sent.size(); ++i) {
            QCOMPARE(sent.at(i).value(QLatin1String("seq")).toInteger(), sent.at(i - 1).value(QLatin1String("seq")).toInteger() + 1);
        }
        f.stream()->setStatsSubscription(0);
    }

    // What a sample reads from a real stream.
    // AV1-Q: the `codec` request's `decode` map is optional; only "hw"/"sw" count.
    void codecRequestReadsTheDecodePaths()
    {
        const auto parse = [](const char *json) {
            return CodecRequest::parse(QJsonDocument::fromJson(json).object());
        };
        auto r = parse(R"({"codecs":["hevc","av1"],"decode":{"avc":"hw","hevc":"hw","av1":"sw"}})");
        QVERIFY(r);
        QCOMPARE(r->decode, (CodecPolicy::ClientDecode{CodecPolicy::DecodePath::Hardware, CodecPolicy::DecodePath::Hardware, CodecPolicy::DecodePath::Software}));
        r = parse(R"({"codecs":["av1"]})"); // an older client
        QVERIFY(r);
        QCOMPARE(r->decode, CodecPolicy::ClientDecode{});
        r = parse(R"({"codecs":["av1"],"decode":{"av1":"gpu","hevc":1,"vp9":"hw"}})"); // unknown values and codecs
        QVERIFY(r);
        QCOMPARE(r->decode, CodecPolicy::ClientDecode{});
        r = parse(R"({"codecs":["av1"],"decode":"hw"})"); // not an object: ignored, not an error
        QVERIFY(r);
        QCOMPARE(r->decode, CodecPolicy::ClientDecode{});
        r = parse(R"({"codecs":[],"decode":{"avc":"SW"}})");
        QVERIFY(r);
        QCOMPARE(r->decode.avc, CodecPolicy::DecodePath::Software);
    }

    // AV1-Q: the AV1 tile count the encoders get: a manual setting always; Automatic is one tile
    // for a client decoding AV1 in hardware, else KPipeWire's per-resolution rows (0).
    void av1TilesFollowTheSettingAndTheDecoder_data()
    {
        QTest::addColumn<QString>("setting");
        QTest::addColumn<QString>("decode"); // the client's decode.av1; empty = not sent
        QTest::addColumn<int>("tiles");
        QTest::newRow("auto, software decoder") << QStringLiteral("auto") << QStringLiteral("sw") << 0;
        QTest::newRow("auto, hardware decoder") << QStringLiteral("auto") << QStringLiteral("hw") << 1;
        QTest::newRow("auto, decoder unknown") << QStringLiteral("auto") << QString() << 0;
        QTest::newRow("8, hardware decoder") << QStringLiteral("8") << QStringLiteral("hw") << 8;
        QTest::newRow("1, software decoder") << QStringLiteral("1") << QStringLiteral("sw") << 1;
        QTest::newRow("16, decoder unknown") << QStringLiteral("16") << QString() << 16;
    }

    void av1TilesFollowTheSettingAndTheDecoder()
    {
        QFETCH(QString, setting);
        QFETCH(QString, decode);
        QFETCH(int, tiles);
        Fixture f;
        f.stream()->setEncoderPolicy(hal(), CodecPolicy::SoftwareEncoding::Auto);
        f.stream()->setAv1TilesSetting(*CodecPolicy::parseAv1Tiles(setting));
        QSignalSpy settings(f.stream(), &VideoStream::encoderSettingsChanged);
        QJsonObject record{{QStringLiteral("codecs"), QJsonArray{QStringLiteral("av1"), QStringLiteral("hevc")}}}; // the client's order is honoured (OPT-063)
        if (!decode.isEmpty()) {
            record.insert(QStringLiteral("decode"), QJsonObject{{QStringLiteral("avc"), QStringLiteral("hw")}, {QStringLiteral("av1"), decode}});
        }
        const auto request = CodecRequest::parse(record);
        QVERIFY(request);
        QString log;
        const auto reply = CodecRequest::apply(*f.stream(), *request, &log);
        QCOMPARE(reply.value(QStringLiteral("selected")).toString(), QStringLiteral("av1"));
        QVERIFY2(log.contains(QStringLiteral("AV1 tiles")), qPrintable(log));
        QCOMPARE(f.stream()->av1Tiles(), tiles);
        QVERIFY(f.stream()->encoderSettings());
        QCOMPARE(f.stream()->encoderSettings()->av1Tiles, tiles);
        // The sessions hear it with the first settings, before the codec switch.
        QVERIFY(!settings.isEmpty());
        QCOMPARE(settings.last().at(0).value<CodecPolicy::EncoderSettings>().av1Tiles, tiles);

        // Stats: the client's decode paths and the tile count.
        const auto snapshot = f.stream()->statsSnapshot();
        QCOMPARE(snapshot.av1Tiles, std::optional<int>(tiles));
        const auto sample = Stats::sampleRecord(snapshot, Stats::Snapshot{}, 1000, 1);
        QCOMPARE(sample.value(QStringLiteral("video")).toObject().value(QStringLiteral("av1Tiles")).toInt(-1), tiles);
        const auto decodeRecord = sample.value(QStringLiteral("policy")).toObject().value(QStringLiteral("decode")).toObject();
        if (decode.isEmpty()) {
            QVERIFY(decodeRecord.isEmpty());
        } else {
            QCOMPARE(decodeRecord.value(QStringLiteral("av1")).toString(), decode);
            QCOMPARE(decodeRecord.value(QStringLiteral("avc")).toString(), QStringLiteral("hw"));
            QVERIFY(!decodeRecord.contains(QStringLiteral("hevc")));
        }

        // A later setting change (a reload between connections) re-resolves at once.
        f.stream()->setAv1TilesSetting(4);
        QCOMPARE(f.stream()->encoderSettings()->av1Tiles, 4);
    }

    void statsSnapshotFollowsThePolicy()
    {
        Fixture f;
        f.stream()->setEncoderPolicy(hal(), CodecPolicy::SoftwareEncoding::Auto);
        f.stream()->setQualityCap(70);
        auto s = f.stream()->statsSnapshot();
        QVERIFY(s.codec.isEmpty()); // no caps, no codec request yet
        QVERIFY(!s.hardware);
        QVERIFY(!s.adaptive);
        QCOMPARE(s.mode, QStringLiteral("auto"));
        QCOMPARE(s.guardState, QStringLiteral("ok"));
        QVERIFY(s.surfaces.isEmpty());
        f.stream()->setPrivateCodecPolicy({VideoCodec::Hevc}, false);
        s = f.stream()->statsSnapshot();
        QCOMPARE(s.codec, QStringLiteral("hevc"));
        QCOMPARE(s.chroma, QStringLiteral("420"));
        QCOMPARE(s.hardware, std::optional<bool>(true));
        QVERIFY(s.preset.isEmpty()); // hardware
        QCOMPARE(s.adaptive, std::optional<bool>(false));
        QCOMPARE(s.qualityCap, 70);
        QCOMPARE(s.frameRateCap, 60);
        QCOMPARE(s.encoderRestarts, std::optional<quint64>(0));
        QVERIFY(!s.encodeLoadP95);
        QVERIFY(!s.framesSkipped);
        // A worker's EncoderStats (stage 6) and a measured encode time.
        f.stream()->addWorkerEncoderStats(30, 2, 4.2);
        s = f.stream()->statsSnapshot();
        QCOMPARE(s.framesSkipped, std::optional<quint64>(2));
        QCOMPARE(s.framesEncoded, quint64(30));
        QCOMPARE(s.encodeMs, std::optional<double>(4.2));
        // Stage 7: no QoE acknowledgement yet.
        QVERIFY(!f.stream()->clientQoe());
        QCOMPARE(s.qoeFrames, quint64(0));
    }
};

QTEST_GUILESS_MAIN(VideoStreamCodecTest)
#include "VideoStreamCodecTest.moc"
