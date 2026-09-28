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
// The WS-E KPipeWire: backend policies (suggestedEncoders() follows them) and a colour range.
struct BackendStream {
    enum Encoder { NoEncoder, VP8, VP9, H264Main, H264Baseline, HEVCMain, AV1Main, WebP, Gif };
    enum class EncoderBackendPolicy { HardwareFirst, HardwareOnly, SoftwareFirst, SoftwareOnly };
    enum class ColorRange { Limited, Full };
    BackendStream(QList<Encoder> hw, QList<Encoder> sw)
        : hardware(std::move(hw))
        , software(std::move(sw))
    {
    }
    QList<Encoder> hardware; ///< encoders with a hardware backend
    QList<Encoder> software;
    EncoderBackendPolicy policy = EncoderBackendPolicy::HardwareFirst;
    ColorRange range = ColorRange::Limited;
    Encoder current = NoEncoder;
    QList<EncoderBackendPolicy> policyWhenEncoderSet;
    void setEncoderBackendPolicy(EncoderBackendPolicy p) { policy = p; }
    void setColorRange(ColorRange r) { range = r; }
    QList<Encoder> suggestedEncoders() const
    {
        QList<Encoder> result;
        for (Encoder e : {H264Main, H264Baseline, HEVCMain, AV1Main}) {
            const bool hw = hardware.contains(e) && policy != EncoderBackendPolicy::SoftwareOnly;
            const bool sw = software.contains(e) && policy != EncoderBackendPolicy::HardwareOnly;
            if (hw || sw) result.append(e);
        }
        return result;
    }
    void setEncoder(Encoder e)
    {
        policyWhenEncoderSet.append(policy);
        if (suggestedEncoders().contains(e)) current = e;
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

    // WS-E: the codec policy's backend reaches KPipeWire before setEncoder() (suggestedEncoders()
    // depends on it): HEVC/AV1 on exactly that backend, H.264 in hardware keeps its fallback.
    void backendPolicyIsSetBeforeTheEncoder()
    {
        using Policy = BackendStream::EncoderBackendPolicy;
        BackendStream s{{BackendStream::H264Main, BackendStream::HEVCMain}, {BackendStream::H264Main, BackendStream::HEVCMain, BackendStream::AV1Main}};
        QVERIFY(EncoderSelection::apply(&s, VideoCodec::Av1, false));
        QCOMPARE(s.policyWhenEncoderSet.last(), Policy::SoftwareOnly);
        QCOMPARE(s.encoder(), BackendStream::AV1Main);
        QVERIFY(EncoderSelection::apply(&s, VideoCodec::Hevc, true));
        QCOMPARE(s.policyWhenEncoderSet.last(), Policy::HardwareOnly);
        QVERIFY(EncoderSelection::apply(&s, VideoCodec::Hevc, false));
        QCOMPARE(s.policyWhenEncoderSet.last(), Policy::SoftwareOnly);
        QVERIFY(EncoderSelection::apply(&s, VideoCodec::Avc444v2, true));
        QCOMPARE(s.policyWhenEncoderSet.last(), Policy::HardwareFirst);
        QVERIFY(EncoderSelection::apply(&s, VideoCodec::Avc420, false));
        QCOMPARE(s.policyWhenEncoderSet.last(), Policy::SoftwareOnly);
        // AV1 only in software, but the policy says hardware: not available, the caller is told.
        QVERIFY(!EncoderSelection::apply(&s, VideoCodec::Av1, true));
        // No choice (a stock client): the policy is left alone.
        BackendStream stock{{BackendStream::H264Main}, {BackendStream::H264Main}};
        QVERIFY(EncoderSelection::apply(&stock, VideoCodec::Avc420));
        QCOMPARE(stock.policyWhenEncoderSet.last(), Policy::HardwareFirst);
    }

    // krdp-client converts HEVC/AV1 with swscale defaults (limited range, whatever the stream
    // signals): both backends of the private codecs encode limited range; AVC stays full.
    void privateCodecsUseLimitedRange()
    {
        QCOMPARE(EncoderSelection::colorRangeFor(VideoCodec::Hevc), EncoderSelection::Range::Limited);
        QCOMPARE(EncoderSelection::colorRangeFor(VideoCodec::Av1), EncoderSelection::Range::Limited);
        QCOMPARE(EncoderSelection::colorRangeFor(VideoCodec::Avc420), EncoderSelection::Range::Full);
        QCOMPARE(EncoderSelection::colorRangeFor(VideoCodec::Avc444v2), EncoderSelection::Range::Full);
        BackendStream s{{BackendStream::H264Main, BackendStream::HEVCMain}, {BackendStream::H264Main, BackendStream::HEVCMain, BackendStream::AV1Main}};
        EncoderSelection::apply(&s, VideoCodec::Avc420, true);
        QCOMPARE(s.range, BackendStream::ColorRange::Full);
        for (const bool hardware : {false, true}) {
            s.range = BackendStream::ColorRange::Full;
            QVERIFY(EncoderSelection::apply(&s, VideoCodec::Hevc, hardware));
            QCOMPARE(s.range, BackendStream::ColorRange::Limited);
        }
        s.range = BackendStream::ColorRange::Full;
        QVERIFY(EncoderSelection::apply(&s, VideoCodec::Av1, false));
        QCOMPARE(s.range, BackendStream::ColorRange::Limited);
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
