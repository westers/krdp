// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 1: FILECONTENTS request validation, stream bookkeeping, violations, lock/unlock and the free-space guard.

#include <QTest>

#include "ClipboardFileLimits.h"
#include "ClipboardFileRequest.h"

using namespace KRdp::ClipboardFileRequest;
namespace Limits = KRdp::ClipboardFileLimits;

Q_DECLARE_METATYPE(KRdp::ClipboardFileRequest::Reason)
Q_DECLARE_METATYPE(KRdp::ClipboardFileRequest::Verdict)

namespace
{
constexpr quint64 GiB = 1024ULL * 1024 * 1024;

Snapshot snapshot()
{
    Snapshot s;
    s.items = {{100, false}, {5 * GiB, false}, {0, true}, {0, false}};
    return s;
}
Request range(quint32 stream, quint32 index, quint64 offset, quint32 length, std::optional<quint32> lockId = std::nullopt)
{
    return {stream, index, FlagRange, offset, length, lockId};
}
Request size(quint32 stream, quint32 index, quint32 length = 8)
{
    return {stream, index, FlagSize, 0, length, std::nullopt};
}
}

class ClipboardFileRequestTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void validRequestsAreAccepted()
    {
        RequestValidator v(false, Limits::MaxOutstanding, true);
        v.setCurrent(snapshot());
        auto d = v.validate(size(1, 0));
        QCOMPARE(d.verdict, Verdict::Accept);
        QCOMPARE(d.fileSize, quint64(100));
        QCOMPARE(v.validate(range(2, 0, 0, 100)).verdict, Verdict::Accept);     // the whole small file
        QCOMPARE(v.validate(range(3, 0, 99, 1)).verdict, Verdict::Accept);      // the last byte
        QCOMPARE(v.validate(range(4, 1, 5 * GiB - 1, 1)).verdict, Verdict::Accept); // 64-bit offsets
        QCOMPARE(v.validate(range(5, 1, 4 * GiB + 12345, 256 * 1024)).verdict, Verdict::Accept);
        QCOMPARE(v.validate(range(6, 1, 0, Limits::MaxRangeBytes)).verdict, Verdict::Accept);
        QCOMPARE(v.validate(size(7, 2)).verdict, Verdict::Accept);              // the size of a directory is fine
        QCOMPARE(v.validate(size(8, 3)).fileSize, quint64(0));
        QCOMPARE(v.violations(), 0);
    }

    void invalidRequests_data()
    {
        QTest::addColumn<Request>("request");
        QTest::addColumn<Reason>("reason");
        QTest::newRow("index = count") << size(1, 4) << Reason::IndexOutOfRange;
        QTest::newRow("index huge") << size(1, 0xffffffffu) << Reason::IndexOutOfRange;
        QTest::newRow("no flags") << Request{1, 0, 0, 0, 8, std::nullopt} << Reason::BadFlags;
        QTest::newRow("both flags") << Request{1, 0, FlagSize | FlagRange, 0, 8, std::nullopt} << Reason::BadFlags;
        QTest::newRow("unknown flag bit") << Request{1, 0, FlagRange | 0x100, 0, 8, std::nullopt} << Reason::BadFlags;
        QTest::newRow("size with length 4") << size(1, 0, 4) << Reason::BadSizeLength;
        QTest::newRow("size with length 0") << size(1, 0, 0) << Reason::BadSizeLength;
        QTest::newRow("size with length 16") << size(1, 0, 16) << Reason::BadSizeLength;
        QTest::newRow("range of 0") << range(1, 0, 0, 0) << Reason::BadRangeLength;
        QTest::newRow("range over 1 MiB") << range(1, 1, 0, Limits::MaxRangeBytes + 1) << Reason::BadRangeLength;
        QTest::newRow("range 4 GiB") << range(1, 1, 0, 0xffffffffu) << Reason::BadRangeLength;
        QTest::newRow("offset + length overflows 64 bits") << range(1, 1, 0xffffffffffffffffULL, 2) << Reason::RangeOverflow;
        QTest::newRow("offset 2^64 - 1, length 1 (sum is 2^64)") << range(1, 1, 0xffffffffffffffffULL, 1) << Reason::RangeOverflow;
        QTest::newRow("offset 2^64 - length (sum is 2^64)") << range(1, 1, 0xffffffffffffffffULL - 4096 + 1, 4096) << Reason::RangeOverflow;
        QTest::newRow("offset 2^64 - 2, length 1 (no overflow, far past the end)") << range(1, 1, 0xfffffffffffffffeULL, 1) << Reason::RangePastEnd;
        QTest::newRow("range past the end by one") << range(1, 0, 1, 100) << Reason::RangePastEnd;
        QTest::newRow("offset at size") << range(1, 0, 100, 1) << Reason::RangePastEnd;
        QTest::newRow("range past 5 GiB") << range(1, 1, 5 * GiB, 1) << Reason::RangePastEnd;
        QTest::newRow("range of a directory") << range(1, 2, 0, 1) << Reason::RangeOnDirectory;
        QTest::newRow("range of an empty file") << range(1, 3, 0, 1) << Reason::RangePastEnd;
        QTest::newRow("clip id without locking") << range(1, 0, 0, 1, 7u) << Reason::LockNotNegotiated;
    }
    void invalidRequests()
    {
        QFETCH(Request, request);
        QFETCH(Reason, reason);
        RequestValidator v(false, Limits::MaxOutstanding, true);
        v.setCurrent(snapshot());
        const auto d = v.validate(request);
        QCOMPARE(d.verdict, Verdict::Fail);
        QCOMPARE(d.reason, reason);
        QCOMPARE(v.violations(), 1);
        QCOMPARE(v.outstanding(), 0); // an invalid request never occupies a slot
        QVERIFY(qstrlen(reasonName(reason)) > 0);
    }
    void thirdViolationAborts()
    {
        RequestValidator v(false, Limits::MaxOutstanding, true);
        v.setCurrent(snapshot());
        QCOMPARE(v.validate(size(1, 99)).verdict, Verdict::Fail);
        QCOMPARE(v.validate(size(2, 99)).verdict, Verdict::Fail);
        const auto third = v.validate(size(3, 99));
        QCOMPARE(third.verdict, Verdict::Abort);
        QVERIFY(v.aborted());
        // Even a valid request is no longer answered.
        QCOMPARE(v.validate(size(4, 0)).verdict, Verdict::Abort);
        QCOMPARE(v.validate(size(4, 0)).reason, Reason::Aborted);
    }
    void validRequestsDoNotCountAsViolations()
    {
        RequestValidator v(false, Limits::MaxOutstanding, true);
        v.setCurrent(snapshot());
        for (quint32 i = 0; i < 100; ++i) {
            QCOMPARE(v.validate(range(i, 0, 0, 10)).verdict, Verdict::Accept);
            v.complete(i);
        }
        QCOMPARE(v.violations(), 0);
        QVERIFY(!v.aborted());
    }

    void streamIdsMustBeUniqueWhileOutstanding()
    {
        RequestValidator v(false, Limits::MaxOutstanding, true);
        v.setCurrent(snapshot());
        QCOMPARE(v.validate(range(5, 0, 0, 10)).verdict, Verdict::Accept);
        QCOMPARE(v.validate(range(5, 0, 10, 10)).reason, Reason::DuplicateStream);
        QCOMPARE(v.outstanding(), 1);
        v.complete(5);
        QCOMPARE(v.validate(range(5, 0, 10, 10)).verdict, Verdict::Accept); // reusable after the answer
    }
    void extraRequestsAreRefusedNotQueuedAndNotViolations()
    {
        RequestValidator v(false, Limits::MaxOutstanding, true);
        v.setCurrent(snapshot());
        for (int i = 0; i < Limits::MaxOutstanding; ++i) {
            QCOMPARE(v.validate(range(quint32(i), 0, 0, 1)).verdict, Verdict::Accept);
        }
        QCOMPARE(v.outstanding(), Limits::MaxOutstanding);
        for (int i = 0; i < 10; ++i) {
            const auto d = v.validate(range(1000 + quint32(i), 0, 0, 1));
            QCOMPARE(d.verdict, Verdict::Refuse);
            QCOMPARE(d.reason, Reason::TooManyOutstanding);
        }
        QCOMPARE(v.refused(), 10);
        QCOMPARE(v.violations(), 0);
        QCOMPARE(v.outstanding(), Limits::MaxOutstanding);
        v.complete(0);
        QCOMPARE(v.validate(range(2000, 0, 0, 1)).verdict, Verdict::Accept);
    }
    void customOutstandingLimit()
    {
        RequestValidator v(false, Limits::StockClientWindow, true);
        v.setCurrent(snapshot());
        for (int i = 0; i < Limits::StockClientWindow; ++i) QCOMPARE(v.validate(range(quint32(i), 0, 0, 1)).verdict, Verdict::Accept);
        QCOMPARE(v.validate(range(99, 0, 0, 1)).verdict, Verdict::Refuse);
    }
    void noOfferMeansNoRequests()
    {
        RequestValidator v(false, Limits::MaxOutstanding, true);
        QCOMPARE(v.validate(size(1, 0)).reason, Reason::NoOffer);
        v.setCurrent(snapshot());
        QCOMPARE(v.validate(size(2, 0)).verdict, Verdict::Accept);
        v.clearCurrent();
        QCOMPARE(v.validate(size(3, 0)).reason, Reason::NoOffer);
    }
    void withoutLockingAnewOfferEndsOutstandingRequests()
    {
        RequestValidator v(false, Limits::MaxOutstanding, true);
        v.setCurrent(snapshot());
        v.validate(range(1, 0, 0, 1));
        v.validate(range(2, 0, 0, 1));
        QCOMPARE(v.outstanding(), 2);
        v.setCurrent(snapshot());
        QCOMPARE(v.outstanding(), 0);
    }

    void lockedSnapshotsOutliveTheOffer()
    {
        RequestValidator v(true, Limits::MaxOutstanding, true);
        Snapshot old;
        old.items = {{10, false}};
        v.setCurrent(old);
        QVERIFY(v.lock(7, old).ok);
        Snapshot fresh;
        fresh.items = {{1000, false}, {2000, false}};
        v.setCurrent(fresh); // a new copy while locked does not invalidate the lock
        QCOMPARE(v.validate(range(1, 0, 0, 10, 7u)).verdict, Verdict::Accept);
        QCOMPARE(v.validate(range(2, 0, 0, 11, 7u)).reason, Reason::RangePastEnd);   // the old list: 10 bytes
        QCOMPARE(v.validate(range(3, 1, 0, 11, 7u)).reason, Reason::IndexOutOfRange); // the old list has one entry
        QCOMPARE(v.validate(range(4, 1, 0, 1500)).verdict, Verdict::Accept);          // no id: the current offer (fresh)
    }
    void unknownLockIsAViolation()
    {
        RequestValidator v(true, Limits::MaxOutstanding, true);
        v.setCurrent(snapshot());
        QCOMPARE(v.validate(range(1, 0, 0, 1, 9u)).reason, Reason::UnknownLock);
        v.lock(9, snapshot());
        QCOMPARE(v.validate(range(2, 0, 0, 1, 9u)).verdict, Verdict::Accept);
        QVERIFY(v.unlock(9));
        QVERIFY(!v.unlock(9));
        QCOMPARE(v.validate(range(3, 0, 0, 1, 9u)).reason, Reason::UnknownLock);
    }
    void locksNeedNegotiationAndAreUnique()
    {
        RequestValidator off(false);
        QVERIFY(!off.lock(1, snapshot()).ok);
        RequestValidator on(true);
        QVERIFY(on.lock(1, snapshot()).ok);
        QVERIFY(!on.lock(1, snapshot()).ok);
        QCOMPARE(on.lockCount(), 1);
    }
    void atMostEightLocksLeastRecentlyUsedIsDropped()
    {
        RequestValidator v(true, Limits::MaxOutstanding, true);
        v.setCurrent(snapshot());
        for (quint32 id = 1; id <= quint32(Limits::MaxLocks); ++id) QVERIFY(v.lock(id, snapshot()).ok);
        QCOMPARE(v.lockCount(), Limits::MaxLocks);
        // Using lock 1 makes lock 2 the least recently used.
        QCOMPARE(v.validate(range(1, 0, 0, 1, 1u)).verdict, Verdict::Accept);
        const auto result = v.lock(100, snapshot());
        QVERIFY(result.ok);
        QCOMPARE(result.evicted, std::optional<quint32>(2));
        QCOMPARE(v.lockCount(), Limits::MaxLocks);
        QVERIFY(v.isLocked(1) && !v.isLocked(2) && v.isLocked(100));
    }

    // Spike S1 (2026-10-17): without CB_HUGE_FILE_SUPPORT_ENABLED nothing at or beyond 4 GiB is served, whatever the peer sends.
    void hugeOffsetsNeedTheNegotiatedFlag()
    {
        RequestValidator v(false, Limits::MaxOutstanding, false);
        v.setCurrent(snapshot());
        QCOMPARE(v.validate(range(1, 1, 0, 65536)).verdict, Verdict::Accept);
        QCOMPARE(v.validate(range(2, 1, 0xffffffffULL - 10, 10)).verdict, Verdict::Accept); // ends at 2^32 - 1: the last offset a 32-bit peer can name
        QCOMPARE(v.validate(range(3, 1, 0xffffffffULL - 10, 11)).reason, Reason::HugeNotNegotiated);
        QCOMPARE(v.validate(range(4, 1, 0x100000000ULL, 1)).reason, Reason::HugeNotNegotiated);
        QCOMPARE(v.validate(range(5, 1, 4 * GiB - 100, 200)).reason, Reason::HugeNotNegotiated); // crossing 4 GiB
        QCOMPARE(v.violations(), 3);
        QCOMPARE(v.validate(range(6, 1, 5 * GiB - 1, 1)).verdict, Verdict::Abort);
        // with the flag the same requests are fine
        RequestValidator huge(false, Limits::MaxOutstanding, true);
        huge.setCurrent(snapshot());
        QCOMPARE(huge.validate(range(3, 1, 0xffffffffULL - 10, 11)).verdict, Verdict::Accept);
        QCOMPARE(huge.validate(range(4, 1, 4 * GiB - 100, 200)).verdict, Verdict::Accept);
    }
    void requestsAreBoundedByTheLimitsConstants()
    {
        QCOMPARE(Limits::MaxRangeBytes, quint32(1024 * 1024));
        QVERIFY(Limits::RangeBytes <= Limits::MaxRangeBytes);
        QCOMPARE(Limits::MaxOutstanding, 16);
        QCOMPARE(Limits::MaxViolations, 3);
    }
};

QTEST_GUILESS_MAIN(ClipboardFileRequestTest)

#include "ClipboardFileRequestTest.moc"
