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
