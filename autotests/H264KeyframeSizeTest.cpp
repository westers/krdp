// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "H264KeyframeSize.h"
#include <QFile>
#include <QTest>

class H264KeyframeSizeTest : public QObject
{
    Q_OBJECT
    QByteArray fixture(const QString &size) {
        QFile file(QFINDTESTDATA(QStringLiteral("data/virtual-fit/%1.h264").arg(size)));
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
};
QTEST_GUILESS_MAIN(H264KeyframeSizeTest)
#include "H264KeyframeSizeTest.moc"
