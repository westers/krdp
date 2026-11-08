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

    // ---- OPT-078 small files: the byte-budget window ----
    void windowConstants()
    {
        QCOMPARE(LegacyWindow.maxCount, 16);
        QCOMPARE(LegacyWindow.maxBytes, qint64(16) * MaxRangeBytes); // no byte rule beyond the count: today's behaviour
        QCOMPARE(NegotiatedWindow.maxCount, 128);
        QCOMPARE(NegotiatedWindow.maxBytes, qint64(4) * 1024 * 1024); // = 16 x 256 KiB: large files behave as before
        QCOMPARE(NegotiatedWindow.maxBytes, qint64(MaxWindow) * RangeBytes);
    }
    void admissionTable_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<qint64>("bytes");
        QTest::addColumn<quint32>("length");
        QTest::addColumn<bool>("admitted");
        const Window w = NegotiatedWindow; // 4 MiB, 128
        const qint64 mib = 1024 * 1024;
        QTest::newRow("idle, small") << 0 << qint64(0) << quint32(100) << true;
        QTest::newRow("idle, even a request over the budget") << 0 << qint64(0) << quint32(MaxRangeBytes) << true;
        QTest::newRow("127 small in flight, the 128th") << 127 << qint64(127 * 20000) << quint32(20000) << true;
        QTest::newRow("128 small in flight, the 129th (one over the count)") << 128 << qint64(128 * 20000) << quint32(20000) << false;
        QTest::newRow("15 x 256 KiB, the 16th (exactly at the bytes)") << 15 << qint64(15 * RangeBytes) << quint32(RangeBytes) << true;
        QTest::newRow("16 x 256 KiB, the 17th (one request over the bytes)") << 16 << qint64(16 * RangeBytes) << quint32(RangeBytes) << false;
        QTest::newRow("bytes one over by a single byte") << 3 << (w.maxBytes - 1) << quint32(2) << false;
        QTest::newRow("bytes exactly at the limit by a single byte") << 3 << (w.maxBytes - 1) << quint32(1) << true;
        QTest::newRow("mixed: 100 small + one big fits") << 100 << qint64(100 * 4096) << quint32(RangeBytes) << true;
        QTest::newRow("mixed: 100 x 40 KiB (4 MiB) leaves no room") << 100 << qint64(100 * 40 * 1024 + 96 * 1024) << quint32(40 * 1024) << false;
        QTest::newRow("count at the cap with no bytes in flight") << 128 << qint64(0) << quint32(1) << false;
        QTest::newRow("a 1 MiB range with 3 MiB in flight fits") << 12 << (3 * mib) << quint32(mib) << true;
        QTest::newRow("a 1 MiB range with 3 MiB + 1 in flight does not") << 12 << (3 * mib + 1) << quint32(mib) << false;
    }
    void admissionTable()
    {
        QFETCH(int, count);
        QFETCH(qint64, bytes);
        QFETCH(quint32, length);
        QFETCH(bool, admitted);
        QCOMPARE(windowAdmits(NegotiatedWindow, count, bytes, length), admitted);
    }
    void theLegacyWindowIsTheOldCountRule()
    {
        // 16 requests of any valid length (1 MiB each = 16 MiB), the 17th is refused: exactly what MaxOutstanding = 16 did.
        for (int n = 0; n < 16; ++n) QVERIFY(windowAdmits(LegacyWindow, n, qint64(n) * MaxRangeBytes, MaxRangeBytes));
        QVERIFY(!windowAdmits(LegacyWindow, 16, qint64(16) * MaxRangeBytes, 1));
        QVERIFY(!windowAdmits(LegacyWindow, 16, 0, 1));
    }
    void negotiationTable_data()
    {
        QTest::addColumn<bool>("advertised");
        QTest::addColumn<qint64>("bytes");
        QTest::addColumn<int>("count");
        QTest::addColumn<qint64>("expectBytes");
        QTest::addColumn<int>("expectCount");
        const qint64 mib = 1024 * 1024;
        QTest::newRow("absent (old peer, stock peer)") << false << qint64(0) << 0 << LegacyWindow.maxBytes << 16;
        QTest::newRow("same as ours") << true << (4 * mib) << 128 << (4 * mib) << 128;
        QTest::newRow("a bigger peer is capped by us") << true << (64 * mib) << 1000 << (4 * mib) << 128;
        QTest::newRow("a smaller peer wins") << true << (1 * mib) << 32 << (1 * mib) << 32;
        QTest::newRow("peer count only smaller") << true << (4 * mib) << 16 << (4 * mib) << 16;
        QTest::newRow("zero count is ignored") << true << (4 * mib) << 0 << LegacyWindow.maxBytes << 16;
        QTest::newRow("negative bytes is ignored") << true << qint64(-1) << 128 << LegacyWindow.maxBytes << 16;
        QTest::newRow("under one range is ignored") << true << qint64(RangeBytes - 1) << 128 << LegacyWindow.maxBytes << 16;
        QTest::newRow("exactly one range is valid") << true << qint64(RangeBytes) << 1 << qint64(RangeBytes) << 1;
    }
    void negotiationTable()
    {
        QFETCH(bool, advertised);
        QFETCH(qint64, bytes);
        QFETCH(int, count);
        QFETCH(qint64, expectBytes);
        QFETCH(int, expectCount);
        const std::optional<Window> peer = advertised ? std::optional<Window>(Window{bytes, count}) : std::nullopt;
        const Window w = negotiatedWindow(peer);
        QCOMPARE(w.maxBytes, expectBytes);
        QCOMPARE(w.maxCount, expectCount);
    }
    // The four peers a host or a client can meet (the same table runs in the client's tests with the roles swapped):
    //   stock client:           serve 16 / request 16 (4 for the stock pacer)
    //   old own client (0.13.2): serve may give it 128 (it never uses more than 16), request 16 (it advertised nothing)
    //   new own client:         serve 128 / 4 MiB, request min(own, advertised)
    //   new client, old host:   the host advertised nothing: the client uses 16 both ways (client tests)
    void theFourPeerCombinations()
    {
        const Window advertised = NegotiatedWindow;
        QCOMPARE(windowForServing(true), LegacyWindow);
        QCOMPARE(windowForRequesting(true, advertised), LegacyWindow); // a stock client cannot have advertised, but even a claim is ignored
        QCOMPARE(windowForServing(false), NegotiatedWindow);           // old and new own clients
        QCOMPARE(windowForRequesting(false, std::nullopt), LegacyWindow); // old own client: no `window` in its clipboard-transfer
        QCOMPARE(windowForRequesting(false, advertised), NegotiatedWindow); // new own client
        QCOMPARE(windowForRequesting(false, Window{1024 * 1024, 32}), (Window{1024 * 1024, 32})); // a more careful client wins
        QCOMPARE(windowForRequesting(false, Window{4, 4}), LegacyWindow); // a nonsense advertisement is ignored
    }
};

QTEST_GUILESS_MAIN(ClipboardFileLimitsTest)

#include "ClipboardFileLimitsTest.moc"
