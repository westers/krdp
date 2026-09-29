// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// AV1-Q: the quality -> quantiser mapping per codec and the AV1 tile layouts of the KPipeWire
// that KRdp links (PipeWireBaseEncodedStream's static queries; the table is in
// KRDPCTL-V2-CONTRACT.md (e) and research.md AV1-Q). Until AV1-Q, av1_vaapi reused the HEVC QP
// formula as its qindex (quality 80 -> 18 of 255, near-lossless).

#include <PipeWireBaseEncodedStream>

#include <QTest>

using Stream = PipeWireBaseEncodedStream;
using Backend = PipeWireBaseEncodedStream::EncoderBackend;

class EncoderTuningTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void quantizerPerCodec_data()
    {
        QTest::addColumn<int>("quality");
        QTest::addColumn<int>("hevcQp"); // hevc_vaapi QP, libx265 CRF
        QTest::addColumn<int>("av1QIndex"); // av1_vaapi base_q_idx (0..255)
        QTest::addColumn<int>("av1Crf"); // SVT-AV1 / libaom CRF (0..63)
        QTest::newRow("100") << 100 << 12 << 64 << 16;
        QTest::newRow("80 (default)") << 80 << 18 << 88 << 22;
        QTest::newRow("60") << 60 << 23 << 111 << 28;
        QTest::newRow("50") << 50 << 26 << 123 << 31;
        QTest::newRow("30") << 30 << 32 << 147 << 37;
        QTest::newRow("10 (adaptive floor)") << 10 << 37 << 170 << 43;
    }

    void quantizerPerCodec()
    {
        QFETCH(int, quality);
        QFETCH(int, hevcQp);
        QFETCH(int, av1QIndex);
        QFETCH(int, av1Crf);
        QCOMPARE(Stream::quantizerForQuality(Stream::HEVCMain, Backend::Hardware, quint8(quality)), hevcQp);
        QCOMPARE(Stream::quantizerForQuality(Stream::HEVCMain, Backend::Software, quint8(quality)), hevcQp);
        QCOMPARE(Stream::quantizerForQuality(Stream::AV1Main, Backend::Hardware, quint8(quality)), av1QIndex);
        QCOMPARE(Stream::quantizerForQuality(Stream::AV1Main, Backend::Software, quint8(quality)), av1Crf);
    }

    void av1NormalRangeIsNotNearLossless()
    {
        // Adaptive quality runs 10..100; the AV1 qindex must stay in a sane range across it.
        for (int quality = 10; quality <= 100; ++quality) {
            const int qindex = Stream::quantizerForQuality(Stream::AV1Main, Backend::Hardware, quint8(quality));
            QVERIFY2(qindex >= 60 && qindex <= 200, qPrintable(QStringLiteral("quality %1 -> qindex %2").arg(quality).arg(qindex)));
        }
    }

    void av1TileLayouts()
    {
        // Automatic: rows on the hardware encoder, columns first in software.
        QCOMPARE(Stream::av1TileLayout(Backend::Hardware, QSize(1920, 1080), 0), QSize(1, 4));
        QCOMPARE(Stream::av1TileLayout(Backend::Hardware, QSize(2560, 1440), 0), QSize(1, 8));
        QCOMPARE(Stream::av1TileLayout(Backend::Hardware, QSize(3840, 2160), 0), QSize(1, 16));
        QCOMPARE(Stream::av1TileLayout(Backend::Software, QSize(1920, 1080), 0), QSize(2, 1));
        QCOMPARE(Stream::av1TileLayout(Backend::Software, QSize(2560, 1440), 0), QSize(2, 2));
        // Manual counts.
        QCOMPARE(Stream::av1TileLayout(Backend::Hardware, QSize(1920, 1080), 1), QSize(1, 1));
        QCOMPARE(Stream::av1TileLayout(Backend::Hardware, QSize(1920, 1080), 8), QSize(1, 8));
        QCOMPARE(Stream::av1TileLayout(Backend::Software, QSize(1920, 1080), 8), QSize(4, 2));
    }
};

QTEST_GUILESS_MAIN(EncoderTuningTest)
#include "EncoderTuningTest.moc"
