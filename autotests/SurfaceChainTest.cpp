// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX11 R6: VideoStream's per-surface rule. Nothing of a codec goes out on a surface before
// a keyframe of that codec with its headers in band has gone out on it; a broken chain waits
// for the next such keyframe. Real VCN packets (data/motion-chain) and header-stripped copies.

#include <QFile>
#include <QTest>

#include "ClientStyleDecoder.h"
#include "SurfaceChain.h"

using namespace KRdp;

namespace
{
QByteArray motion(VideoCodec codec, int number)
{
    const QString extension = codec == VideoCodec::Av1 ? QStringLiteral("av1") : codec == VideoCodec::Hevc ? QStringLiteral("hevc") : QStringLiteral("h264");
    QFile file(QFINDTESTDATA(QStringLiteral("data/motion-chain/1280x720-%1.%2").arg(number).arg(extension)));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

/// The keyframe without its parameter sets: what a stream whose headers went to extradata
/// (AV_CODEC_FLAG_GLOBAL_HEADER) would send.
QByteArray withoutHeaders(VideoCodec codec, const QByteArray &keyframe)
{
    const auto *data = reinterpret_cast<const unsigned char *>(keyframe.constData());
    if (codec == VideoCodec::Av1) {
        // The first OBU is the sequence header: header byte, one-byte leb128 size, payload.
        if ((data[0] >> 3 & 0xf) != 1 || (data[1] & 0x80)) return {};
        return keyframe.mid(2 + data[1]);
    }
    // From the IDR/IRAP slice's start code on.
    for (qsizetype i = 0; i + 4 < keyframe.size(); ++i) {
        if (data[i] || data[i + 1] || data[i + 2] != 1) continue;
        const int type = codec == VideoCodec::Hevc ? (data[i + 3] >> 1) & 0x3f : data[i + 3] & 0x1f;
        if (codec == VideoCodec::Hevc ? (type >= 16 && type <= 21) : type == 5) return keyframe.mid(i);
    }
    return {};
}
}

class SurfaceChainTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void keyframesCarryTheirHeaders_data()
    {
        QTest::addColumn<int>("codecId");
        QTest::newRow("av1") << int(VideoCodec::Av1);
        QTest::newRow("hevc") << int(VideoCodec::Hevc);
        QTest::newRow("h264") << int(VideoCodec::Avc420);
    }
    void keyframesCarryTheirHeaders()
    {
        QFETCH(int, codecId);
        const auto codec = VideoCodec(codecId);
        const auto keyframe = motion(codec, 0);
        QVERIFY(!keyframe.isEmpty());
        QVERIFY(keyframeCarriesHeaders(codec, keyframe));
        QVERIFY(!keyframeCarriesHeaders(codec, motion(codec, 1)));
        const auto stripped = withoutHeaders(codec, keyframe);
        QVERIFY(!stripped.isEmpty());
        QVERIFY(!keyframeCarriesHeaders(codec, stripped));
        // A fresh client decoder cannot start from it either (the rule is not stricter than FFmpeg).
        ClientStyle::Decoder client;
        QVERIFY(!client.feed(1, codec, stripped) || !client.missingPictures().isEmpty());
        // Another codec's keyframe is not this codec's.
        const auto other = codec == VideoCodec::Av1 ? VideoCodec::Hevc : VideoCodec::Av1;
        QVERIFY(!keyframeCarriesHeaders(other, keyframe));
        // Truncated or empty packets are no keyframes.
        QVERIFY(!keyframeCarriesHeaders(codec, {}));
        QVERIFY(!keyframeCarriesHeaders(codec, keyframe.left(3)));
    }

    void nothingBeforeAKeyframeWithHeaders_data()
    {
        keyframesCarryTheirHeaders_data();
    }
    void nothingBeforeAKeyframeWithHeaders()
    {
        QFETCH(int, codecId);
        const auto codec = VideoCodec(codecId);
        SurfaceChain chain;
        QVERIFY(!chain.family());
        // A fresh surface: a delta (a pre-keyframe or leftover packet) waits for a keyframe.
        QCOMPARE(chain.admit(codec, true, false, motion(codec, 1)), SurfaceChain::Verdict::WaitForKeyFrame);
        // A keyframe no fresh decoder can start from does not start the chain.
        QCOMPARE(chain.admit(codec, true, true, withoutHeaders(codec, motion(codec, 0))), SurfaceChain::Verdict::KeyFrameWithoutHeaders);
        QCOMPARE(chain.admit(codec, true, false, motion(codec, 1)), SurfaceChain::Verdict::WaitForKeyFrame);
        // The keyframe with its headers does, and its deltas follow.
        QCOMPARE(chain.admit(codec, true, true, motion(codec, 0)), SurfaceChain::Verdict::Send);
        QCOMPARE(chain.family(), std::optional(codecFamily(codec)));
        QCOMPARE(chain.admit(codec, true, false, motion(codec, 1)), SurfaceChain::Verdict::Send);
        QCOMPARE(chain.admit(codec, true, false, motion(codec, 2)), SurfaceChain::Verdict::Send);
        // A frame of the chain that did not go out breaks it until the next keyframe.
        chain.broken();
        QCOMPARE(chain.admit(codec, true, false, motion(codec, 3)), SurfaceChain::Verdict::WaitForKeyFrame);
        QCOMPARE(chain.admit(codec, true, true, motion(codec, 0)), SurfaceChain::Verdict::Send);
        QCOMPARE(chain.admit(codec, true, false, motion(codec, 1)), SurfaceChain::Verdict::Send);
    }

    // A codec switch on a live surface: the new codec's deltas (and the old codec's leftovers)
    // wait for the new codec's keyframe, including encoder changes420/444/v2.
    void aCodecSwitchNeedsTheNewCodecsKeyframe()
    {
        SurfaceChain chain;
        QCOMPARE(chain.admit(VideoCodec::Avc420, true, true, motion(VideoCodec::Avc420, 0)), SurfaceChain::Verdict::Send);
        QCOMPARE(chain.admit(VideoCodec::Avc444v2, true, false, motion(VideoCodec::Avc420, 1)), SurfaceChain::Verdict::WaitForKeyFrame);
        QCOMPARE(chain.admit(VideoCodec::Avc444v2, true, true, motion(VideoCodec::Avc420, 0)), SurfaceChain::Verdict::Send);
        QCOMPARE(chain.admit(VideoCodec::Avc444v2, true, false, motion(VideoCodec::Avc420, 1)), SurfaceChain::Verdict::Send);
        QCOMPARE(chain.admit(VideoCodec::Avc420, true, false, motion(VideoCodec::Avc420, 1)), SurfaceChain::Verdict::WaitForKeyFrame);
        QCOMPARE(chain.admit(VideoCodec::Avc444, true, false, motion(VideoCodec::Avc420, 1)), SurfaceChain::Verdict::WaitForKeyFrame);
        QCOMPARE(chain.admit(VideoCodec::Av1, true, false, motion(VideoCodec::Av1, 1)), SurfaceChain::Verdict::WaitForKeyFrame);
        QCOMPARE(chain.admit(VideoCodec::Av1, true, true, motion(VideoCodec::Av1, 0)), SurfaceChain::Verdict::Send);
        QCOMPARE(chain.admit(VideoCodec::Avc420, true, false, motion(VideoCodec::Avc420, 2)), SurfaceChain::Verdict::WaitForKeyFrame);
        QCOMPARE(chain.admit(VideoCodec::Av1, true, false, motion(VideoCodec::Av1, 1)), SurfaceChain::Verdict::Send);
        QCOMPARE(chain.admit(VideoCodec::Hevc, true, true, motion(VideoCodec::Hevc, 0)), SurfaceChain::Verdict::Send);
        QCOMPARE(chain.admit(VideoCodec::Av1, true, false, motion(VideoCodec::Av1, 2)), SurfaceChain::Verdict::WaitForKeyFrame);
    }

    // A frame that does not say which codec produced it (a synthetic payload) is the
    // connection's codec; its keyframe is not inspected, but the chain rule still holds.
    void unlabelledFramesFollowTheChainRule()
    {
        SurfaceChain chain;
        const QByteArray synthetic("KRDPSEQ:\x01\x00\x00\x00\x00\x00\x00\x00", 16);
        QCOMPARE(chain.admit(VideoCodec::Avc420, false, false, synthetic), SurfaceChain::Verdict::WaitForKeyFrame);
        QCOMPARE(chain.admit(VideoCodec::Avc420, false, true, synthetic), SurfaceChain::Verdict::Send);
        QCOMPARE(chain.admit(VideoCodec::Avc420, false, false, synthetic), SurfaceChain::Verdict::Send);
    }
};

QTEST_GUILESS_MAIN(SurfaceChainTest)
#include "SurfaceChainTest.moc"
