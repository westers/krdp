// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 1: the free-space guard decision (statvfs injected as plain numbers) and the limits constants.

#include <QTest>

#include "ClipboardFileLimits.h"

using namespace KRdp::ClipboardFileLimits;

namespace
{
constexpr quint64 GiB = 1024ULL * 1024 * 1024;
constexpr quint64 TiB = 1024 * GiB;
}

class ClipboardFileLimitsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void defaultReserveIsTheLargerOfTwoGiBAndFivePercent()
    {
        QCOMPARE(reserveBytes(0, 10 * GiB), 2 * GiB);        // 5% = 0.5 GiB
        QCOMPARE(reserveBytes(0, 40 * GiB), 2 * GiB);        // 5% = 2 GiB exactly
        QCOMPARE(reserveBytes(0, 100 * GiB), 5 * GiB);
        QCOMPARE(reserveBytes(0, 4 * TiB), 4 * TiB / 20);
        QCOMPARE(reserveBytes(0, 0), 2 * GiB);
        QCOMPARE(reserveBytes(3 * GiB, 4 * TiB), 3 * GiB);   // an explicit setting wins, both ways
        QCOMPARE(reserveBytes(1, 100 * GiB), quint64(1));
        // No overflow with a huge filesystem
        QVERIFY(reserveBytes(0, ~quint64(0)) > 0);
    }
    void decisionTable_data()
    {
        QTest::addColumn<std::optional<quint64>>("free");
        QTest::addColumn<std::optional<quint64>>("fs");
        QTest::addColumn<quint64>("remaining");
        QTest::addColumn<quint64>("configured");
        QTest::addColumn<int>("verdict");
        const auto ok = int(SpaceVerdict::Ok), no = int(SpaceVerdict::NotEnough), unk = int(SpaceVerdict::Unknown);
        using O = std::optional<quint64>;
        QTest::newRow("plenty") << O(500 * GiB) << O(1 * TiB) << (10 * GiB) << quint64(0) << ok;
        QTest::newRow("exactly fits with the reserve") << O(105 * GiB) << O(100 * GiB) << (100 * GiB) << quint64(0) << ok; // reserve 5 GiB
        QTest::newRow("one byte short") << O(105 * GiB) << O(100 * GiB) << (100 * GiB + 1) << quint64(0) << no;
        QTest::newRow("zero bytes needs only the reserve") << O(3 * GiB) << O(100 * GiB) << quint64(0) << quint64(0) << no; // reserve 5 GiB
        QTest::newRow("zero bytes, enough reserve") << O(6 * GiB) << O(100 * GiB) << quint64(0) << quint64(0) << ok;
        QTest::newRow("small disk: floor reserve") << O(3 * GiB) << O(20 * GiB) << (1 * GiB) << quint64(0) << ok;       // reserve 2 GiB, needs 3
        QTest::newRow("small disk: floor reserve, short") << O(3 * GiB) << O(20 * GiB) << (1 * GiB + 1) << quint64(0) << no;
        QTest::newRow("configured reserve") << O(10 * GiB) << O(1 * TiB) << (4 * GiB) << (5 * GiB) << ok;
        QTest::newRow("configured reserve, short") << O(10 * GiB) << O(1 * TiB) << (6 * GiB) << (5 * GiB) << no;
        QTest::newRow("5 GiB file on a 3 GiB free disk") << O(3 * GiB) << O(500 * GiB) << (5 * GiB) << quint64(0) << no;
        QTest::newRow("size near 2^64 saturates") << O(1 * TiB) << O(1 * TiB) << (~quint64(0) - 5) << quint64(0) << no;
        QTest::newRow("statvfs failed (free)") << O() << O(1 * TiB) << (1 * GiB) << quint64(0) << unk;
        QTest::newRow("statvfs failed (total)") << O(1 * TiB) << O() << (1 * GiB) << quint64(0) << unk;
        QTest::newRow("empty filesystem size") << O(1 * TiB) << O(0) << (1 * GiB) << quint64(0) << unk;
    }
    void decisionTable()
    {
        QFETCH(std::optional<quint64>, free);
        QFETCH(std::optional<quint64>, fs);
        QFETCH(quint64, remaining);
        QFETCH(quint64, configured);
        QFETCH(int, verdict);
        const auto d = checkFreeSpace(free, fs, remaining, configured);
        QCOMPARE(int(d.verdict), verdict);
        if (d.verdict != SpaceVerdict::Unknown) {
            QVERIFY(d.neededBytes >= d.reserve);
            QCOMPARE(d.verdict == SpaceVerdict::Ok, d.neededBytes <= d.freeBytes);
        }
    }
    void midTransferCheckUsesTheRemainder()
    {
        // 50 GiB file: 40 GiB staged, 10 GiB left, the disk now has 12 GiB free (reserve 2 GiB): fits exactly.
        QCOMPARE(checkFreeSpace(12 * GiB, 20 * GiB, 10 * GiB).verdict, SpaceVerdict::Ok);
        QCOMPARE(checkFreeSpace(12 * GiB - 1, 20 * GiB, 10 * GiB).verdict, SpaceVerdict::NotEnough);
    }
    void hugeFilesNeedTheNegotiatedFlag()
    {
        QVERIFY(fitsWithoutHugeSupport(0));
        QVERIFY(fitsWithoutHugeSupport(0xffffffffULL));
        QVERIFY(!fitsWithoutHugeSupport(0x100000000ULL));
        QVERIFY(!fitsWithoutHugeSupport(5 * GiB));
    }
    void periodicCheckSchedule()
    {
        QVERIFY(spaceCheckDue(0, true));
        QVERIFY(!spaceCheckDue(0, false));
        QVERIFY(!spaceCheckDue(SpaceCheckIntervalBytes - 1, false));
        QVERIFY(spaceCheckDue(SpaceCheckIntervalBytes, false));
    }
    void constantsMatchTheSpec()
    {
        QCOMPARE(MaxEntries, 100000u);
        QCOMPARE(MaxDepth, 64);
        QCOMPARE(MaxTotalPathBytes, qint64(32) * 1024 * 1024);
        QCOMPARE(MaxDescriptorWireBytes, qint64(64) * 1024 * 1024);
        QCOMPARE(MaxComponentBytes, 255);
        QCOMPARE(MaxPathBytes, 4096);
        QCOMPARE(RangeBytes, quint32(256 * 1024));
        QCOMPARE(MaxWindow, 16);
        QCOMPARE(MaxLocks, 8);
        QCOMPARE(WalkBudgetMs, 5000);
        QCOMPARE(OfferWhenCompleteBytes, quint64(256) * 1024 * 1024);
    }
};

QTEST_GUILESS_MAIN(ClipboardFileLimitsTest)

#include "ClipboardFileLimitsTest.moc"
