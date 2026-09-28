// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX2 F1 on a real VideoStream (no socket): the `codec` request is answered from the
// encoders the host really has, the codec sessions are built for follows, and an encoder that
// turns out not to produce the private codec moves the connection off it at once.

#include "RdpConnection.h"
#include "Server.h"
#include "VideoStream.h"

#include <QCoreApplication>
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
    // Sol: the client asks for HEVC+AV1, the host has neither: AVC, and nothing says 0x8001.
    void noEncoderMeansAvc()
    {
        Fixture f;
        f.stream()->setEncoderPolicy(sol(), CodecPolicy::SoftwareEncoding::Auto);
        const auto d = f.stream()->setPrivateCodecPolicy({VideoCodec::Hevc, VideoCodec::Av1}, true);
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
        QCOMPARE(f.stream()->setPrivateCodecPolicy({VideoCodec::Hevc, VideoCodec::Av1}, true).choice.family, Family::Av1);
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
        QCOMPARE(f.stream()->requestedFrameRate(), 60u);
        const auto d = f.stream()->setPrivateCodecPolicy({VideoCodec::Hevc, VideoCodec::Av1}, true);
        QCOMPARE(d.choice, (CodecPolicy::Choice{Family::Av1, false}));
        QCOMPARE(order, (QStringList{QStringLiteral("settings"), QStringLiteral("codec")}));
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
};

QTEST_GUILESS_MAIN(VideoStreamCodecTest)
#include "VideoStreamCodecTest.moc"
