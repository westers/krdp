// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "H264KeyframeSize.h"
#include <QFile>
#include <QTest>

class H264KeyframeSizeTest : public QObject
{
    Q_OBJECT
    QByteArray fixture(const QString &size, const QString &extension = QStringLiteral("h264")) {
        QFile file(QFINDTESTDATA(QStringLiteral("data/virtual-fit/%1.%2").arg(size, extension)));
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
private Q_SLOTS:
    void dimensions_data()
    {
        QTest::addColumn<QString>("name"); QTest::addColumn<QSize>("size");
        QTest::newRow("720p") << QStringLiteral("1280x720") << QSize(1280, 720);
        QTest::newRow("cropped-height") << QStringLiteral("1920x1080") << QSize(1920, 1080);
        QTest::newRow("cropped-width") << QStringLiteral("1366x768") << QSize(1366, 768);
        QTest::newRow("multislice") << QStringLiteral("1920x1080-multislice") << QSize(1920, 1080);
        QTest::newRow("production-720p") << QStringLiteral("1280x720-production") << QSize(1280, 720);
        QTest::newRow("production-1080p") << QStringLiteral("1920x1080-production") << QSize(1920, 1080);
    }
    void dimensions()
    {
        QFETCH(QString, name); QFETCH(QSize, size);
        const auto packet = fixture(name); QVERIFY(!packet.isEmpty());
        const auto decoded = KRdp::h264KeyframeSize(packet); QVERIFY(decoded); QCOMPARE(*decoded, size);
    }
    void rejectsMissingTruncatedOrAmbiguousPayload()
    {
        QVERIFY(!KRdp::h264KeyframeSize({}));
        QVERIFY(!KRdp::h264KeyframeSize(QByteArray("garbage")));
        QVERIFY(!KRdp::h264KeyframeSize(QByteArray(16 * 1024 * 1024 + 1, 'x')));
        const auto packet = fixture(QStringLiteral("1920x1080")); QVERIFY(!packet.isEmpty());
        QVERIFY(!KRdp::h264KeyframeSize(packet.left(10))); // Incomplete SPS.
        QVERIFY(!KRdp::h264KeyframeSize(packet.left(packet.size() / 2))); // Headers alone are insufficient.
        QVERIFY(!KRdp::h264KeyframeSize(packet + fixture(QStringLiteral("1280x720")))); // Two parameter sets/frames.
        QVERIFY(!KRdp::h264KeyframeSize(QByteArray("garbage") + packet));
        auto invalidHeader = packet;
        const auto sps = invalidHeader.indexOf(QByteArray::fromHex("00000167")); QVERIFY(sps >= 0);
        invalidHeader[sps + 3] = char(0xe7); // forbidden_zero_bit.
        QVERIFY(!KRdp::h264KeyframeSize(invalidHeader));
        auto missingSps = packet.mid(packet.indexOf(QByteArray::fromHex("00000168")));
        QVERIFY(!KRdp::h264KeyframeSize(missingSps));
        auto nonIdr = packet;
        const auto idr = nonIdr.indexOf(QByteArray::fromHex("00000165")); QVERIFY(idr >= 0);
        nonIdr[idr + 3] = char(0x61);
        QVERIFY(!KRdp::h264KeyframeSize(nonIdr));
        QVERIFY(!KRdp::h264KeyframeSize(packet + packet.mid(idr))); // Another picture reusing the same SPS/PPS.
        const auto unsupported = fixture(QStringLiteral("444")); QVERIFY(!unsupported.isEmpty());
        QVERIFY(!KRdp::h264KeyframeSize(unsupported));
        const auto oversized = fixture(QStringLiteral("4098x200")); QVERIFY(!oversized.isEmpty());
        QVERIFY(!KRdp::h264KeyframeSize(oversized));
    }
    // AV1-Q: the tile layout in real keyframe headers (tile_info(), uniform and non-uniform
    // spacing). Hal's 780M (FFmpeg 8.0.1 av1_vaapi -tiles 1xN on radeonsi) codes rows only;
    // SVT-AV1 2.3.0 tile-columns=1:tile-rows=1 codes 2x2. FFmpeg's trace_headers agrees.
    void av1Tiles_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QSize>("tiles"); // columns x rows
        QTest::newRow("hal one tile") << QStringLiteral("1920x1080-hal") << QSize(1, 1);
        QTest::newRow("cray one tile") << QStringLiteral("1920x1080-cray") << QSize(1, 1);
        QTest::newRow("720p one tile") << QStringLiteral("1280x720") << QSize(1, 1);
        QTest::newRow("hal 4 rows, uniform") << QStringLiteral("1920x1080-hal-tiles1x4") << QSize(1, 4);
        QTest::newRow("hal 8 rows, non-uniform") << QStringLiteral("1920x1080-hal-tiles1x8") << QSize(1, 8);
        QTest::newRow("hal 16 rows at 1080p") << QStringLiteral("1920x1080-hal-tiles1x16") << QSize(1, 16);
        QTest::newRow("hal 16 rows at 4K") << QStringLiteral("3840x2160-hal-tiles1x16") << QSize(1, 16);
        QTest::newRow("svt 2x2") << QStringLiteral("1920x1080-svt-tiles2x2") << QSize(2, 2);
    }
    void av1Tiles()
    {
        QFETCH(QString, name);
        QFETCH(QSize, tiles);
        const auto packet = fixture(name, QStringLiteral("av1"));
        QVERIFY(!packet.isEmpty());
        QCOMPARE(KRdp::av1KeyframeTiles(packet), std::optional<QSize>(tiles));
        // Tiles change nothing about the display-size proof (R5).
        QVERIFY(KRdp::encodedKeyframe(KRdp::VideoCodec::Av1, packet));
    }
    void av1TilesNeedAKeyframeHeader()
    {
        QVERIFY(!KRdp::av1KeyframeTiles({}));
        QVERIFY(!KRdp::av1KeyframeTiles(QByteArray("garbage")));
        const auto packet = fixture(QStringLiteral("1920x1080-hal-tiles1x8"), QStringLiteral("av1"));
        QVERIFY(!packet.isEmpty());
        QVERIFY(!KRdp::av1KeyframeTiles(packet.left(12))); // the sequence header alone
    }
    // AUD-FIX10 R5: coded vs display size of real hardware keyframes (Hal's 780M and cray's
    // Strix Halo, FFmpeg 8.0.1 VA-API on radeonsi) and of the H.264 fixtures.
    void codedAndDisplaySizes_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("extension");
        QTest::addColumn<int>("codec");
        QTest::addColumn<QSize>("coded");
        QTest::addColumn<QSize>("display");
        QTest::addColumn<bool>("signalled");
        const int avc = int(KRdp::VideoCodec::Avc420), hevc = int(KRdp::VideoCodec::Hevc), av1 = int(KRdp::VideoCodec::Av1);
        const auto h264 = QStringLiteral("h264"), h265 = QStringLiteral("hevc"), obu = QStringLiteral("av1");
        QTest::newRow("h264 1080p cropped") << QStringLiteral("1920x1080") << h264 << avc << QSize(1920, 1088) << QSize(1920, 1080) << true;
        QTest::newRow("h264 1366 cropped") << QStringLiteral("1366x768") << h264 << avc << QSize(1376, 768) << QSize(1366, 768) << true;
        QTest::newRow("hevc 720p") << QStringLiteral("1280x720") << h265 << hevc << QSize(1280, 720) << QSize(1280, 720) << true;
        QTest::newRow("hevc 1080p hal") << QStringLiteral("1920x1080-hal") << h265 << hevc << QSize(1920, 1088) << QSize(1920, 1080) << true;
        QTest::newRow("hevc 1080p cray") << QStringLiteral("1920x1080-cray") << h265 << hevc << QSize(1920, 1088) << QSize(1920, 1080) << true;
        QTest::newRow("hevc 1366 hal") << QStringLiteral("1366x768-hal") << h265 << hevc << QSize(1408, 768) << QSize(1366, 768) << true;
        QTest::newRow("av1 720p") << QStringLiteral("1280x720") << obu << av1 << QSize(1280, 720) << QSize(1280, 720) << false;
        QTest::newRow("av1 1080p hal") << QStringLiteral("1920x1080-hal") << obu << av1 << QSize(1920, 1082) << QSize(1920, 1082) << false;
        QTest::newRow("av1 1080p cray") << QStringLiteral("1920x1080-cray") << obu << av1 << QSize(1920, 1082) << QSize(1920, 1082) << false;
        QTest::newRow("av1 1366 hal") << QStringLiteral("1366x768-hal") << obu << av1 << QSize(1408, 768) << QSize(1408, 768) << false;
        QTest::newRow("av1 render 1080") << QStringLiteral("1920x1080-render1080") << obu << av1 << QSize(1920, 1082) << QSize(1920, 1080) << true;
        QTest::newRow("av1 render 1076") << QStringLiteral("1920x1080-render1076") << obu << av1 << QSize(1920, 1082) << QSize(1920, 1076) << true;
    }
    void codedAndDisplaySizes()
    {
        QFETCH(QString, name); QFETCH(QString, extension); QFETCH(int, codec);
        QFETCH(QSize, coded); QFETCH(QSize, display); QFETCH(bool, signalled);
        const auto packet = fixture(name, extension); QVERIFY(!packet.isEmpty());
        const auto keyframe = KRdp::encodedKeyframe(KRdp::VideoCodec(codec), packet);
        QVERIFY(keyframe);
        QCOMPARE(keyframe->coded, coded);
        QCOMPARE(keyframe->display, display);
        QCOMPARE(keyframe->displaySignalled, signalled);
        QCOMPARE(KRdp::encodedKeyframeSize(KRdp::VideoCodec(codec), packet), std::optional(display));
    }

    // AUD-FIX10 R5: which output size a keyframe proves. 299cd25 compared the AV1 frame size
    // (1920x1082) with the output (1920x1080), so cray's layout was never confirmed.
    void showsOutput_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("extension");
        QTest::addColumn<int>("codec");
        QTest::addColumn<QSize>("output");
        QTest::addColumn<bool>("shows");
        const int avc = int(KRdp::VideoCodec::Avc420), hevc = int(KRdp::VideoCodec::Hevc), av1 = int(KRdp::VideoCodec::Av1);
        const auto h264 = QStringLiteral("h264"), h265 = QStringLiteral("hevc"), obu = QStringLiteral("av1");
        QTest::newRow("av1 hal 1082 shows 1080") << QStringLiteral("1920x1080-hal") << obu << av1 << QSize(1920, 1080) << true;
        QTest::newRow("av1 cray 1082 shows 1080") << QStringLiteral("1920x1080-cray") << obu << av1 << QSize(1920, 1080) << true;
        QTest::newRow("av1 hal 1408 shows 1366") << QStringLiteral("1366x768-hal") << obu << av1 << QSize(1366, 768) << true;
        QTest::newRow("av1 720p exact") << QStringLiteral("1280x720") << obu << av1 << QSize(1280, 720) << true;
        // Never smaller than the output, never past its alignment (64 wide, 16 high).
        QTest::newRow("av1 1082 not 1920x1084") << QStringLiteral("1920x1080-hal") << obu << av1 << QSize(1920, 1084) << false;
        QTest::newRow("av1 1082 not 1920x1072") << QStringLiteral("1920x1080-hal") << obu << av1 << QSize(1920, 1072) << false;
        QTest::newRow("av1 1082 not 1856x1080") << QStringLiteral("1920x1080-hal") << obu << av1 << QSize(1856, 1080) << false;
        QTest::newRow("av1 720p not 1280x704") << QStringLiteral("1280x720") << obu << av1 << QSize(1280, 704) << false;
        QTest::newRow("av1 1408 not 1344x768") << QStringLiteral("1366x768-hal") << obu << av1 << QSize(1344, 768) << false;
        QTest::newRow("av1 empty output") << QStringLiteral("1280x720") << obu << av1 << QSize() << false;
        // A signalled render size is exact: no alignment tolerance.
        QTest::newRow("av1 render 1080 shows 1080") << QStringLiteral("1920x1080-render1080") << obu << av1 << QSize(1920, 1080) << true;
        QTest::newRow("av1 render 1080 not 1082") << QStringLiteral("1920x1080-render1080") << obu << av1 << QSize(1920, 1082) << false;
        QTest::newRow("av1 render 1076 not 1080") << QStringLiteral("1920x1080-render1076") << obu << av1 << QSize(1920, 1080) << false;
        QTest::newRow("av1 render 1076 shows 1076") << QStringLiteral("1920x1080-render1076") << obu << av1 << QSize(1920, 1076) << true;
        // HEVC and H.264 carry their crop: exact only.
        QTest::newRow("hevc hal 1080") << QStringLiteral("1920x1080-hal") << h265 << hevc << QSize(1920, 1080) << true;
        QTest::newRow("hevc cray 1080") << QStringLiteral("1920x1080-cray") << h265 << hevc << QSize(1920, 1080) << true;
        QTest::newRow("hevc 1080 not 1088") << QStringLiteral("1920x1080-hal") << h265 << hevc << QSize(1920, 1088) << false;
        QTest::newRow("hevc 1366") << QStringLiteral("1366x768-hal") << h265 << hevc << QSize(1366, 768) << true;
        QTest::newRow("hevc 1366 not 1408") << QStringLiteral("1366x768-hal") << h265 << hevc << QSize(1408, 768) << false;
        QTest::newRow("h264 1080") << QStringLiteral("1920x1080") << h264 << avc << QSize(1920, 1080) << true;
        QTest::newRow("h264 1080 not 1088") << QStringLiteral("1920x1080") << h264 << avc << QSize(1920, 1088) << false;
        QTest::newRow("h264 1080 not 1078") << QStringLiteral("1920x1080") << h264 << avc << QSize(1920, 1078) << false;
        // The codec label must match the bytes.
        QTest::newRow("av1 bytes as hevc") << QStringLiteral("1920x1080-hal") << obu << hevc << QSize(1920, 1080) << false;
        QTest::newRow("hevc bytes as av1") << QStringLiteral("1920x1080-hal") << h265 << av1 << QSize(1920, 1080) << false;
        QTest::newRow("av1 bytes as h264") << QStringLiteral("1920x1080-hal") << obu << avc << QSize(1920, 1080) << false;
    }
    void showsOutput()
    {
        QFETCH(QString, name); QFETCH(QString, extension); QFETCH(int, codec); QFETCH(QSize, output); QFETCH(bool, shows);
        const auto packet = fixture(name, extension); QVERIFY(!packet.isEmpty());
        QCOMPARE(KRdp::encodedKeyframeShows(KRdp::VideoCodec(codec), packet, output), shows);
    }

    // The AV1 header walk rejects what it cannot read rather than guess a size.
    void rejectsBrokenAv1Headers()
    {
        const auto packet = fixture(QStringLiteral("1920x1080-hal"), QStringLiteral("av1")); QVERIFY(!packet.isEmpty());
        const auto av1 = KRdp::VideoCodec::Av1;
        QVERIFY(KRdp::encodedKeyframeShows(av1, packet, QSize(1920, 1080)));
        for (qsizetype cut = 1; cut < packet.size(); ++cut) {
            QVERIFY2(!KRdp::encodedKeyframeShows(av1, packet.left(cut), QSize(1920, 1080)), qPrintable(QStringLiteral("cut at %1").arg(cut)));
        }
        // Two sequence headers.
        const qsizetype sequence = 2; // after the temporal delimiter 12 00
        QCOMPARE(quint8(packet[sequence]), quint8(0x0a));
        const qsizetype sequenceSize = 2 + quint8(packet[sequence + 1]);
        auto twice = packet;
        twice.insert(sequence, packet.mid(sequence, sequenceSize));
        QVERIFY(!KRdp::encodedKeyframe(av1, twice));
        // No sequence header before the frame header.
        auto missing = packet;
        missing.remove(sequence, sequenceSize);
        QVERIFY(!KRdp::encodedKeyframe(av1, missing));
        // An OBU without obu_has_size_field, and the forbidden bit.
        auto unsized = packet;
        unsized[0] = char(quint8(unsized[0]) & ~0x02);
        QVERIFY(!KRdp::encodedKeyframe(av1, unsized));
        auto forbidden = packet;
        forbidden[0] = char(quint8(forbidden[0]) | 0x80);
        QVERIFY(!KRdp::encodedKeyframe(av1, forbidden));
    }
    void rejectsIncompleteMultislice_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("synthetic") << QStringLiteral("1920x1080-multislice");
        QTest::newRow("production-720p") << QStringLiteral("1280x720-production");
        QTest::newRow("production-1080p") << QStringLiteral("1920x1080-production");
    }
    void rejectsIncompleteMultislice()
    {
        QFETCH(QString, name);
        const auto packet = fixture(name); QVERIFY(!packet.isEmpty());
        QList<qsizetype> starts;
        for (qsizetype i = 0; i + 3 < packet.size(); ++i) {
            if (packet[i] == 0 && packet[i + 1] == 0 && packet[i + 2] == 1) {
                starts.append(i > 0 && packet[i - 1] == 0 ? i - 1 : i);
                i += 3;
            }
        }
        starts.append(packet.size());
        int slices = 0;
        for (qsizetype i = 0; i + 1 < starts.size(); ++i) {
            const auto begin = starts[i], end = starts[i + 1];
            const auto header = begin + (packet[begin + 2] == 1 ? 3 : 4);
            if ((static_cast<unsigned char>(packet[header]) & 0x1f) != 5) continue;
            ++slices;
            auto missing = packet;
            missing.remove(begin, end - begin);
            QVERIFY2(!KRdp::h264KeyframeSize(missing), qPrintable(QStringLiteral("missing slice %1").arg(slices)));
            // Keep its header/first_mb but remove half the coded slice, including
            // its end. Remaining later slices must not conceal an incomplete one.
            QVERIFY(end - header > 4);
            auto truncated = packet;
            const auto cut = header + (end - header) / 2;
            truncated.remove(cut, end - cut);
            QVERIFY2(!KRdp::h264KeyframeSize(truncated), qPrintable(QStringLiteral("truncated slice %1").arg(slices)));
        }
        QVERIFY(slices > 1);
    }
};
QTEST_GUILESS_MAIN(H264KeyframeSizeTest)
#include "H264KeyframeSizeTest.moc"
