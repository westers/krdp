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
};

QTEST_GUILESS_MAIN(VideoStreamCodecTest)
#include "VideoStreamCodecTest.moc"
