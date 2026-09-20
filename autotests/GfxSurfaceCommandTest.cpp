// SPDX-FileCopyrightText: 2026 Steve Westers
//
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <QTest>

#include "GfxSurfaceCommand.h"

using namespace KRdp;
using namespace KRdp::GfxSurfaceCommand;

class GfxSurfaceCommandTest : public QObject
{
    Q_OBJECT
private:
    static void checkCommon(const Storage &s, uint16_t surfaceId, const QSize &size, quint8 quality)
    {
        QCOMPARE(s.command.surfaceId, uint32_t(surfaceId));
        QCOMPARE(s.command.format, uint32_t(PIXEL_FORMAT_BGRX32));
        QCOMPARE(s.command.left, 0u);
        QCOMPARE(s.command.top, 0u);
        QCOMPARE(s.command.right, uint32_t(size.width()));
        QCOMPARE(s.command.bottom, uint32_t(size.height()));
        QCOMPARE(s.command.length, 0u);
        QVERIFY(s.command.data == nullptr);
        QCOMPARE(s.rect.left, 0);
        QCOMPARE(s.rect.top, 0);
        QCOMPARE(s.rect.right, UINT16(size.width()));
        QCOMPARE(s.rect.bottom, UINT16(size.height()));
        QCOMPARE(s.quant.qp, qpForQuality(quality));
        QCOMPARE(s.quant.qualityVal, quality);
        QCOMPARE(s.quant.qpVal, BYTE(0));
        QCOMPARE(s.quant.r, BYTE(0));
        QCOMPARE(s.quant.p, BYTE(0));
    }
    static void checkMeta(const RDPGFX_H264_METABLOCK &m, const Storage &s)
    {
        QCOMPARE(m.numRegionRects, 1u);
        QVERIFY(m.regionRects == &s.rect);
        QVERIFY(m.quantQualityVals == &s.quant);
    }

private Q_SLOTS:
    void avc420IsTodaysCommand()
    {
        // Field for field what VideoStream::sendFrame built inline before this header existed (R7).
        const QByteArray data(100, 'm');
        Storage s;
        build(s, VideoCodec::Avc420, 7, QSize(2560, 1440), data, QByteArray(), 80);
        checkCommon(s, 7, QSize(2560, 1440), 80);
        QCOMPARE(s.command.codecId, uint32_t(RDPGFX_CODECID_AVC420));
        QVERIFY(s.command.extra == &s.avc420);
        QVERIFY(s.avc420.data == reinterpret_cast<const BYTE *>(data.constData()));
        QCOMPARE(s.avc420.length, 100u);
        checkMeta(s.avc420.meta, s);
        QCOMPARE(qpForQuality(100), quint8(12));
        QCOMPARE(qpForQuality(80), quint8(18));
        QCOMPARE(qpForQuality(50), quint8(26));
    }
    void avc420IgnoresAnAux()
    {
        Storage s;
        build(s, VideoCodec::Avc420, 1, QSize(64, 32), QByteArray(10, 'm'), QByteArray(5, 'a'), 70);
        QCOMPARE(s.command.codecId, uint32_t(RDPGFX_CODECID_AVC420));
        QVERIFY(s.command.extra == &s.avc420);
    }
    void privateCodecUsesRawWirePayload()
    {
        const QByteArray data(123, 'h');
        Storage s;
        build(s, VideoCodec::Hevc, 4, QSize(1280, 720), data, QByteArray(), 80);
        QCOMPARE(s.command.surfaceId, 4u);
        QCOMPARE(s.command.format, uint32_t(PIXEL_FORMAT_BGRX32));
        QCOMPARE(s.command.left, 0u);
        QCOMPARE(s.command.top, 0u);
        QCOMPARE(s.command.right, 1280u);
        QCOMPARE(s.command.bottom, 720u);
        QCOMPARE(s.command.codecId, uint32_t(VideoCodecSupport::PrivateHevcCodecId));
        QCOMPARE(s.command.length, 123u);
        QVERIFY(s.command.data == reinterpret_cast<const BYTE *>(data.constData()));
        QVERIFY(s.command.extra == nullptr);

        build(s, VideoCodec::Av1, 4, QSize(1280, 720), data, QByteArray(), 80);
        QCOMPARE(s.command.codecId, uint32_t(VideoCodecSupport::PrivateAv1CodecId));
        QCOMPARE(s.command.length, 123u);
        QVERIFY(s.command.extra == nullptr);
    }
    void avc444v2BothStreams()
    {
        const QByteArray data(1000, 'm'), aux(300, 'a');
        Storage s;
        build(s, VideoCodec::Avc444v2, 9, QSize(1920, 1080), data, aux, 60);
        checkCommon(s, 9, QSize(1920, 1080), 60);
        QCOMPARE(s.command.codecId, uint32_t(RDPGFX_CODECID_AVC444v2));
        QVERIFY(s.command.extra == &s.avc444);
        QCOMPARE(s.avc444.LC, BYTE(0));
        QCOMPARE(s.avc444.cbAvc420EncodedBitstream1, uint32_t(4 + 10 + 1000));
        QVERIFY(s.avc444.bitstream[0].data == reinterpret_cast<const BYTE *>(data.constData()));
        QCOMPARE(s.avc444.bitstream[0].length, 1000u);
        QVERIFY(s.avc444.bitstream[1].data == reinterpret_cast<const BYTE *>(aux.constData()));
        QCOMPARE(s.avc444.bitstream[1].length, 300u);
        checkMeta(s.avc444.bitstream[0].meta, s);
        checkMeta(s.avc444.bitstream[1].meta, s);
    }
    void avc444LumaOnly()
    {
        const QByteArray data(500, 'm');
        Storage s;
        build(s, VideoCodec::Avc444, 3, QSize(640, 480), data, QByteArray(), 90);
        QCOMPARE(s.command.codecId, uint32_t(RDPGFX_CODECID_AVC444));
        QCOMPARE(s.avc444.LC, BYTE(1));
        QCOMPARE(s.avc444.cbAvc420EncodedBitstream1, uint32_t(4 + 10 + 500));
        QCOMPARE(s.avc444.bitstream[1].length, 0u);
        QVERIFY(s.avc444.bitstream[1].data == nullptr);
        QCOMPARE(s.avc444.bitstream[1].meta.numRegionRects, 0u);
        checkMeta(s.avc444.bitstream[0].meta, s);
    }
    void avc444ChromaOnlyRefresh()
    {
        // The encoder's at-rest refresh: no main picture, one aux picture -> LC = 2 with the aux in bitstream[0] (R3 as amended).
        const QByteArray aux(240, 'a');
        Storage s;
        build(s, VideoCodec::Avc444v2, 3, QSize(64, 64), QByteArray(), aux, 90);
        QCOMPARE(s.command.codecId, uint32_t(RDPGFX_CODECID_AVC444v2));
        QCOMPARE(s.avc444.LC, BYTE(2));
        QVERIFY(s.avc444.bitstream[0].data == reinterpret_cast<const BYTE *>(aux.constData()));
        QCOMPARE(s.avc444.bitstream[0].length, 240u);
        QCOMPARE(s.avc444.cbAvc420EncodedBitstream1, uint32_t(4 + 10 + 240));
        QCOMPARE(s.avc444.bitstream[1].length, 0u);
        QVERIFY(s.avc444.bitstream[1].data == nullptr);
        checkMeta(s.avc444.bitstream[0].meta, s);
    }
    void nothingToSendIsNeverBuilt()
    {
        // VideoStream drops a frame with neither picture before building; the builder still yields a harmless LC = 1 empty main.
        Storage s;
        build(s, VideoCodec::Avc444, 3, QSize(64, 64), QByteArray(), QByteArray(), 90);
        QCOMPARE(s.avc444.LC, BYTE(1));
        QCOMPARE(s.avc444.bitstream[0].length, 0u);
    }
};

QTEST_GUILESS_MAIN(GfxSurfaceCommandTest)
#include "GfxSurfaceCommandTest.moc"
