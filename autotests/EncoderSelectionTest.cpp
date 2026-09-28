// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AUD-FIX2 F1: the RDPGFX codec id must always match the encoder that runs. The session asks
// KPipeWire for the codec's encoder (EncoderSelection::apply()) and learns whether it got it;
// Sol's KPipeWire (HEVCMain in the enum, but no VAAPI HEVC) silently ran H.264 under 0x8001.
// Test doubles with the shape of a stock and of the private KPipeWire stream.

#include "EncoderSelection.h"

#include <QList>
#include <QTest>

using namespace KRdp;

namespace
{
struct PrivateStream {
    enum Encoder { NoEncoder, VP8, VP9, H264Main, H264Baseline, HEVCMain, AV1Main, WebP, Gif };
    QList<Encoder> offered;
    Encoder current = NoEncoder;
    QList<Encoder> suggestedEncoders() const { return offered; }
    void setEncoder(Encoder e)
    {
        if (offered.contains(e)) current = e;
    }
    Encoder encoder() const { return current; }
};
struct StockLike {
    enum Encoder { NoEncoder, VP8, VP9, H264Main, H264Baseline, WebP, Gif };
    QList<Encoder> offered;
    Encoder current = NoEncoder;
    QList<Encoder> suggestedEncoders() const { return offered; }
    void setEncoder(Encoder e)
    {
        if (offered.contains(e)) current = e;
    }
    Encoder encoder() const { return current; }
};
}

class EncoderSelectionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void hardwareHevcIsUsed()
    {
        PrivateStream hal{{PrivateStream::H264Main, PrivateStream::H264Baseline, PrivateStream::HEVCMain, PrivateStream::AV1Main}};
        QVERIFY(EncoderSelection::apply(&hal, VideoCodec::Hevc));
        QCOMPARE(hal.encoder(), PrivateStream::HEVCMain);
        QVERIFY(EncoderSelection::apply(&hal, VideoCodec::Av1));
        QCOMPARE(hal.encoder(), PrivateStream::AV1Main);
        QVERIFY(EncoderSelection::apply(&hal, VideoCodec::Avc444v2));
        QCOMPARE(hal.encoder(), PrivateStream::H264Main);
    }

    // Sol: the private enum, but KPipeWire does not suggest HEVC/AV1 (no VAAPI encoder).
    void missingHevcIsReported()
    {
        PrivateStream sol{{PrivateStream::VP8, PrivateStream::H264Main, PrivateStream::H264Baseline}};
        QVERIFY(!EncoderSelection::apply(&sol, VideoCodec::Hevc));
        QCOMPARE(sol.encoder(), PrivateStream::H264Main); // what actually runs
        QVERIFY(!EncoderSelection::apply(&sol, VideoCodec::Av1));
        QVERIFY(EncoderSelection::apply(&sol, VideoCodec::Avc420));
    }

    void stockKPipeWireHasNoPrivateCodecs()
    {
        StockLike stock{{StockLike::H264Main, StockLike::H264Baseline}};
        QVERIFY(!EncoderSelection::apply(&stock, VideoCodec::Hevc));
        QVERIFY(!EncoderSelection::apply(&stock, VideoCodec::Av1));
        QVERIFY(EncoderSelection::apply(&stock, VideoCodec::Avc444));
        QCOMPARE(stock.encoder(), StockLike::H264Main);
    }

    void baselineOnlyStillCountsAsAvc()
    {
        StockLike openh264{{StockLike::H264Baseline}};
        QVERIFY(EncoderSelection::apply(&openh264, VideoCodec::Avc420));
        QCOMPARE(openh264.encoder(), StockLike::H264Baseline);
        // No H.264 at all: nothing matches, the caller is told.
        StockLike none{{StockLike::VP8}};
        QVERIFY(!EncoderSelection::apply(&none, VideoCodec::Avc420));
    }
};

QTEST_GUILESS_MAIN(EncoderSelectionTest)
#include "EncoderSelectionTest.moc"
