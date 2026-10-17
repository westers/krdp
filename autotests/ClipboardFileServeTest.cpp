// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 2: the host-to-client FILECONTENTS state machine (window, pacing, locks, failures, aborts) with a fake clock and fake callbacks.

#include <QTest>

#include "ClipboardFileServe.h"

using namespace KRdp;
using namespace KRdp::ClipboardFileServe;
using Clock = ClipboardFilePacing::Clock;
using ClipboardFileRequest::FlagRange;
using ClipboardFileRequest::FlagSize;
using namespace std::chrono_literals;

namespace
{
constexpr quint64 GiB = 1024ULL * 1024 * 1024;
constexpr quint32 Range = ClipboardFileLimits::RangeBytes;

ClipboardFileRequest::Snapshot snapshot()
{
    // 0: a 1 MiB file, 1: a directory, 2: a 5 GiB file, 3: an empty file
    return {{{1024 * 1024, false}, {0, true}, {5 * GiB, false}, {0, false}}};
}

ClipboardFileRequest::Request range(quint32 stream, quint32 index, quint64 offset, quint32 length, std::optional<quint32> lock = std::nullopt)
{
    return {stream, index, FlagRange, offset, length, lock};
}
ClipboardFileRequest::Request sizeOf(quint32 stream, quint32 index, std::optional<quint32> lock = std::nullopt)
{
    return {stream, index, FlagSize, 0, 8, lock};
}

struct Rig {
    struct Response {
        quint32 stream;
        bool ok;
        QByteArray data;
    };
    QList<ReadIssue> reads;
    QList<Response> responses;
    QList<quint64> cancelled;
    QList<quint32> evicted;
    struct Ended {
        quint64 seq;
        EndReason reason;
        ReadStatus cause;
    };
    QList<Ended> ended;
    bool refuseReads = false;
    Clock::time_point now = Clock::time_point(std::chrono::seconds(1000));
    std::unique_ptr<Server> server;

    explicit Rig(bool locking = true, bool huge = true, ClipboardFilePacing::Policy policy = unlimited())
    {
        Callbacks callbacks;
        callbacks.issueRead = [this](const ReadIssue &issue) {
            if (refuseReads) return false;
            reads.append(issue);
            return true;
        };
        callbacks.respond = [this](quint32 stream, bool ok, const QByteArray &data, bool) { responses.append({stream, ok, data}); };
        callbacks.cancelOffer = [this](quint64 seq) { cancelled.append(seq); };
        callbacks.unlockEvicted = [this](quint32 id) { evicted.append(id); };
        callbacks.ended = [this](quint64 seq, EndReason reason, ReadStatus cause) { ended.append({seq, reason, cause}); };
        server = std::make_unique<Server>(callbacks, locking, huge, policy);
        server->setOffer(1, snapshot(), now);
    }
    static ClipboardFilePacing::Policy unlimited()
    {
        ClipboardFilePacing::Policy policy;
        policy.unlimited = true;
        return policy;
    }
    void advance(std::chrono::milliseconds ms) { now += ms; }
    void pump(std::optional<qint64> queue = std::nullopt) { server->pump(now, queue); }
    /// A request arrives and the owner pumps (as the broker does after every event).
    void request(const ClipboardFileRequest::Request &r)
    {
        server->onRequest(r, now);
        pump();
    }
    /// The worker answers a read without the owner pumping yet.
    void finish(quint32 stream, ReadStatus status = ReadStatus::Ok)
    {
        for (const auto &issue : std::as_const(reads)) {
            if (issue.streamId == stream) {
                server->readFinished(stream, status, status == ReadStatus::Ok ? QByteArray(int(issue.length), char('a' + stream % 26)) : QByteArray(), now);
                return;
            }
        }
        QFAIL("no read was issued for that stream");
    }
    void answer(quint32 stream, ReadStatus status = ReadStatus::Ok)
    {
        finish(stream, status);
        pump();
    }
    int respondedOk() const
    {
        int n = 0;
        for (const auto &r : responses) n += r.ok;
        return n;
    }
};
}

class ClipboardFileServeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sizeIsAnsweredFromTheSnapshotWithoutTheWorker()
    {
        Rig rig;
        rig.request(sizeOf(10, 2));
        QCOMPARE(rig.responses.size(), 1);
        QVERIFY(rig.responses.first().ok);
        QCOMPARE(rig.responses.first().data.size(), 8);
        quint64 value = 0;
        for (int i = 0; i < 8; ++i) value |= quint64(quint8(rig.responses.first().data[i])) << (8 * i);
        QCOMPARE(value, 5 * GiB); // 64-bit
        QVERIFY(rig.reads.isEmpty());
        rig.request(sizeOf(11, 3));
        QCOMPARE(rig.responses.last().data, QByteArray(8, '\0'));
        QCOMPARE(rig.server->counters().sizes, quint64(2));
    }

    void aRangeBecomesOneReadAndOneResponse()
    {
        Rig rig;
        rig.request(range(20, 2, 4 * GiB + 5, Range));
        QCOMPARE(rig.reads.size(), 1);
        const auto issue = rig.reads.first();
        QCOMPARE(issue.seq, quint64(1));
        QCOMPARE(issue.index, quint32(2));
        QCOMPARE(issue.offset, 4 * GiB + 5); // 64-bit offsets reach the worker intact
        QCOMPARE(issue.length, Range);
        QVERIFY(rig.responses.isEmpty());
        rig.answer(20);
        QCOMPARE(rig.responses.size(), 1);
        QVERIFY(rig.responses.first().ok);
        QCOMPARE(rig.responses.first().data.size(), int(Range));
        QCOMPARE(rig.server->pending(), 0);
        QCOMPARE(rig.server->counters().bytes, quint64(Range));
        QCOMPARE(rig.server->counters().ranges, quint64(1));
    }

    void theWindowBoundsTheReadsInFlight()
    {
        // Auto at the start rate (128 kB/s): the window is one request.
        Rig slow(true, true, ClipboardFilePacing::Policy{});
        for (quint32 i = 0; i < 5; ++i) slow.request(range(30 + i, 2, i * Range, Range));
        QCOMPARE(slow.reads.size(), 1);
        QCOMPARE(slow.server->pending(), 5);
        slow.answer(30);
        QCOMPARE(slow.responses.size(), 1);
        QCOMPARE(slow.reads.size(), 2); // the next one is issued as the first leaves
        // Unlimited: sixteen at once, never more.
        Rig fast;
        for (quint32 i = 0; i < 16; ++i) fast.request(range(100 + i, 0, 0, 1000));
        QCOMPARE(fast.reads.size(), 16);
        // A stock client: at most four.
        auto stock = ClipboardFilePacing::effectivePolicy(ClipboardFilePacing::RequestedMode::Unlimited, std::nullopt, ClipboardFilePacing::Ceiling::none(), true);
        Rig stockRig(true, true, stock);
        ClipboardFilePacing::Inputs lan;
        lan.capacityKbps = 1'000'000; // an idle gigabit link: the rate climbs, the window still stops at four
        lan.sendQueueBytes = 0;
        for (int i = 0; i < 80; ++i) {
            stockRig.advance(100ms);
            stockRig.server->tick(stockRig.now, lan);
        }
        QCOMPARE(stockRig.server->pacer().window(), ClipboardFileLimits::StockClientWindow);
        for (quint32 i = 0; i < 8; ++i) stockRig.request(range(200 + i, 0, 0, 1000));
        QCOMPARE(stockRig.reads.size(), 4);
        // Beyond the validator's outstanding cap (16) a request is refused, not queued, and not a violation.
        for (quint32 i = 8; i < 20; ++i) stockRig.request(range(200 + i, 0, 0, 1000));
        QCOMPARE(stockRig.server->counters().refused, quint64(4));
        QCOMPARE(stockRig.server->counters().violations, quint64(0));
        QCOMPARE(stockRig.responses.size(), 4);
        QVERIFY(!stockRig.responses.first().ok);
    }

    void aResponseWaitsForTheTokenBucketAndTheSendQueue()
    {
        ClipboardFilePacing::Policy capped;
        capped.capBytesPerSecond = 100'000; // 100 kB/s
        Rig rig(true, true, capped);
        QCOMPARE(rig.server->pacer().window(), 1);
        rig.request(range(40, 0, 0, Range));
        rig.request(range(41, 0, Range, Range));
        rig.answer(40);
        QCOMPARE(rig.responses.size(), 1); // an empty bucket may overdraw once
        rig.answer(41);
        QCOMPARE(rig.responses.size(), 1); // the second waits: 256 KiB at 100 kB/s
        const auto wait = rig.server->delayUntilNext(rig.now);
        QVERIFY2(wait > 2s && wait < 3s, qPrintable(QString::number(wait.count())));
        rig.advance(1s);
        rig.server->pump(rig.now, std::nullopt);
        QCOMPARE(rig.responses.size(), 1);
        rig.advance(2s);
        // ...and then the socket's send queue must be under the bound.
        rig.server->pump(rig.now, 200 * 1024);
        QCOMPARE(rig.responses.size(), 1);
        rig.server->pump(rig.now, 10 * 1024);
        QCOMPARE(rig.responses.size(), 2);
        QVERIFY(rig.responses.last().ok);
        QCOMPARE(rig.server->delayUntilNext(rig.now), 0ms);
    }

    void atMostAFewResponsesGoOutPerPass()
    {
        Rig rig; // unlimited: only the per-pass cap and the queue bound apply
        for (quint32 i = 0; i < 8; ++i) rig.request(range(50 + i, 0, i * 1000, 1000));
        for (quint32 i = 0; i < 8; ++i) rig.finish(50 + i);
        QVERIFY(rig.responses.isEmpty()); // events do not write: the owner pumps
        rig.pump();
        QCOMPARE(rig.responses.size(), MaxResponsesPerPass);
        rig.pump();
        QCOMPARE(rig.responses.size(), 2 * MaxResponsesPerPass);
        // The queue assumed by one pass counts what the pass itself handed over: a 256 KiB response passes an empty queue only once.
        Rig big;
        for (quint32 i = 0; i < 4; ++i) big.request(range(60 + i, 2, i * Range, Range));
        for (quint32 i = 0; i < 4; ++i) big.finish(60 + i);
        big.pump(0);
        QCOMPARE(big.responses.size(), 1);
        big.pump(0);
        QCOMPARE(big.responses.size(), 2); // the socket drained between passes
        big.pump(100 * 1024); // 100 KiB still queued: nothing goes
        QCOMPARE(big.responses.size(), 2);
    }

    void aRequestThatBreaksTheRulesIsAnsweredFailedAndTheThirdAborts()
    {
        Rig rig;
        rig.request(range(70, 9, 0, 10)); // no such index
        QCOMPARE(rig.responses.size(), 1);
        QVERIFY(!rig.responses.last().ok);
        rig.request(range(71, 1, 0, 10)); // a range on a directory
        QVERIFY(!rig.responses.last().ok);
        QVERIFY(rig.ended.isEmpty());
        rig.request(range(72, 0, 1024 * 1024, 1)); // past the end: the third violation
        QCOMPARE(rig.responses.size(), 3);
        QVERIFY(!rig.responses.last().ok);
        QCOMPARE(rig.server->counters().violations, quint64(3));
        QCOMPARE(rig.server->counters().aborts, quint64(1));
        QCOMPARE(rig.ended.size(), 1);
        QCOMPARE(rig.ended.first().reason, EndReason::Aborted);
        QVERIFY(rig.cancelled.contains(1));
        // From now on nothing is served from this transfer, even a good request...
        rig.request(range(73, 0, 0, 10));
        QVERIFY(!rig.responses.last().ok);
        QVERIFY(rig.reads.isEmpty());
        // ...until a new offer starts a new transfer.
        rig.server->setOffer(2, snapshot(), rig.now);
        rig.request(range(74, 0, 0, 10));
        QCOMPARE(rig.reads.size(), 1);
        QCOMPARE(rig.reads.first().seq, quint64(2));
    }

    void hostileRequestsNeverReachTheWorker()
    {
        Rig rig(true, false); // no huge support
        const QList<ClipboardFileRequest::Request> bad = {
            range(80, 0, 0, 0), // zero length
            range(81, 0, 0, ClipboardFileLimits::MaxRangeBytes + 1),
            range(82, 0, ~quint64(0) - 3, 10), // overflow
            range(83, 2, 4 * GiB, 10), // beyond 4 GiB without the huge flag
            range(84, 0, 0, 10, 77), // a lock id that was never taken
            {85, 0, 0x3, 0, 10, std::nullopt}, // both flags
            {86, 0, FlagSize, 0, 4, std::nullopt}, // a SIZE that does not ask for 8 bytes
        };
        for (const auto &request : bad) {
            rig.request(request);
            if (rig.server->aborted()) rig.server->setOffer(rig.server->currentSeq() + 1, snapshot(), rig.now); // a new transfer per three
            QVERIFY2(rig.reads.isEmpty(), "a hostile request reached the worker");
        }
        QCOMPARE(rig.responses.size(), int(bad.size()));
        for (const auto &response : std::as_const(rig.responses)) QVERIFY(!response.ok);
        // a stream id used twice among the outstanding
        rig.request(range(90, 0, 0, 100));
        rig.request(range(90, 0, 100, 100));
        QCOMPARE(rig.reads.size(), 1);
        QVERIFY(!rig.responses.last().ok);
    }

    void aChangedFileFailsItsStreamAndEndsTheTransfer()
    {
        Rig rig;
        for (quint32 i = 0; i < 4; ++i) rig.request(range(100 + i, 0, i * 1000, 1000));
        QCOMPARE(rig.reads.size(), 4);
        rig.answer(100);
        QCOMPARE(rig.respondedOk(), 1);
        rig.answer(101, ReadStatus::Changed);
        // 101 fails, and so does everything else of the offer that is still pending.
        QCOMPARE(rig.responses.size(), 4);
        QCOMPARE(rig.respondedOk(), 1);
        QCOMPARE(rig.server->pending(), 0);
        QCOMPARE(rig.ended.size(), 1);
        QCOMPARE(rig.ended.first().reason, EndReason::Failed);
        QCOMPARE(rig.ended.first().cause, ReadStatus::Changed);
        QCOMPARE(rig.cancelled, (QList<quint64>{1}));
        // The worker's late answers for the others are ignored.
        rig.server->readFinished(102, ReadStatus::Ok, QByteArray(1000, 'x'), rig.now);
        rig.pump();
        QCOMPARE(rig.responses.size(), 4);
        // New requests for the dead transfer are failed, not read.
        rig.reads.clear();
        rig.request(range(110, 0, 0, 10));
        QVERIFY(!rig.responses.last().ok);
        QVERIFY(rig.reads.isEmpty());
        // A new copy is a new transfer.
        rig.server->setOffer(2, snapshot(), rig.now);
        rig.request(range(111, 0, 0, 10));
        QCOMPARE(rig.reads.size(), 1);
        // Gone, io errors and a read that could not even be sent end it the same way.
        rig.answer(111, ReadStatus::Gone);
        QCOMPARE(rig.ended.last().cause, ReadStatus::Gone);
        rig.server->setOffer(3, snapshot(), rig.now);
        rig.refuseReads = true;
        rig.request(range(112, 0, 0, 10));
        QVERIFY(!rig.responses.last().ok);
        QCOMPARE(rig.ended.last().cause, ReadStatus::NotSent);
    }

    void aWorkerAnswerOfTheWrongSizeIsNotServed()
    {
        Rig rig;
        rig.request(range(120, 0, 0, 1000));
        rig.server->readFinished(120, ReadStatus::Ok, QByteArray(999, 'x'), rig.now); // short
        rig.pump();
        QCOMPARE(rig.responses.size(), 1);
        QVERIFY(!rig.responses.last().ok);
        QCOMPARE(rig.ended.last().cause, ReadStatus::IoError);
        // an answer for a stream nobody asked about
        rig.server->readFinished(999, ReadStatus::Ok, QByteArray(10, 'x'), rig.now);
        rig.pump();
        QCOMPARE(rig.responses.size(), 1);
    }

    void locksKeepTheirOffersAndALockBeforeTheDescriptorBinds()
    {
        Rig rig;
        QVERIFY(rig.server->lock(500)); // the current offer (1) is locked
        QCOMPARE(rig.server->lockedSeq(500), quint64(1));
        // A new copy replaces the current offer; requests carrying the lock id still read the OLD list.
        rig.server->setOffer(2, snapshot(), rig.now);
        rig.request(range(130, 0, 0, 100, 500));
        rig.request(range(131, 0, 0, 100));
        QCOMPARE(rig.reads.size(), 2);
        QCOMPARE(rig.reads.at(0).seq, quint64(1));
        QCOMPARE(rig.reads.at(1).seq, quint64(2));
        // The lock is released: the offer is no longer readable through it.
        QCOMPARE(rig.server->unlock(500), quint64(1));
        rig.request(range(132, 0, 0, 100, 500));
        QVERIFY(!rig.responses.last().ok);
        QCOMPARE(rig.server->unlock(500), quint64(0)); // twice is nothing
        // A lock before any descriptor waits for it.
        Rig early;
        early.server->clearOffer();
        QVERIFY(early.server->lock(600));
        QCOMPARE(early.server->lockCount(), 0);
        early.server->setOffer(5, snapshot(), early.now);
        QCOMPARE(early.server->lockCount(), 1);
        QCOMPARE(early.server->lockedSeq(600), quint64(5));
        // Without locking negotiated a lock is refused.
        Rig noLock(false);
        QVERIFY(!noLock.server->lock(1));
    }

    void tooManyLocksEvictTheOldestAndTellTheWorker()
    {
        Rig rig;
        for (quint32 i = 0; i < quint32(ClipboardFileLimits::MaxLocks); ++i) QVERIFY(rig.server->lock(i));
        QVERIFY(rig.evicted.isEmpty());
        QVERIFY(rig.server->lock(100));
        QCOMPARE(rig.evicted, (QList<quint32>{0}));
        QCOMPARE(rig.server->lockCount(), ClipboardFileLimits::MaxLocks);
    }

    void withoutLockingANewOfferEndsTheOldRequests()
    {
        Rig rig(false);
        rig.request(range(140, 0, 0, 100));
        rig.request(range(141, 0, 100, 100));
        QCOMPARE(rig.server->pending(), 2);
        rig.server->setOffer(2, snapshot(), rig.now);
        rig.pump();
        QCOMPARE(rig.server->pending(), 0);
        QCOMPARE(rig.responses.size(), 2);
        QVERIFY(!rig.responses.at(0).ok && !rig.responses.at(1).ok);
        QVERIFY(rig.cancelled.contains(1));
    }

    void resetAnswersEverythingFailedAndCancels()
    {
        Rig rig;
        for (quint32 i = 0; i < 6; ++i) rig.request(range(150 + i, 0, i * 100, 100));
        rig.answer(150);
        rig.answer(151);
        QCOMPARE(rig.respondedOk(), 2);
        rig.server->reset();
        QCOMPARE(rig.responses.size(), 6);
        QCOMPARE(rig.respondedOk(), 2);
        QCOMPARE(rig.server->pending(), 0);
        QVERIFY(rig.cancelled.contains(1));
        QVERIFY(!rig.server->hasOffer());
        // nothing is served without an offer
        rig.reads.clear();
        rig.request(range(160, 0, 0, 10));
        QVERIFY(rig.reads.isEmpty());
        QVERIFY(!rig.responses.last().ok);
    }

    void responsesGoOldestFirstAndALaterOneDoesNotOvertake()
    {
        ClipboardFilePacing::Policy capped;
        capped.capBytesPerSecond = 100'000;
        Rig rig(true, true, capped);
        for (quint32 i = 0; i < 3; ++i) rig.request(range(170 + i, 0, i * 1000, 1000));
        // the window is one: 170 is read first
        QCOMPARE(rig.reads.size(), 1);
        rig.answer(170);
        QCOMPARE(rig.responses.size(), 1);
        QCOMPARE(rig.responses.last().stream, quint32(170));
        // the overdrawn bucket now holds 171 back: it is read (the window opened) but not written
        QCOMPARE(rig.reads.size(), 2);
        rig.answer(171);
        QCOMPARE(rig.responses.size(), 1);
        QVERIFY(rig.server->delayUntilNext(rig.now) > 0ms);
    }

    void recentRateCountsTheLastSecondOfFileBytes()
    {
        Rig rig;
        rig.request(range(180, 0, 0, Range));
        rig.answer(180);
        QCOMPARE(rig.server->recentKbps(rig.now), quint32(Range * 8 / 1000));
        rig.advance(1500ms);
        QCOMPARE(rig.server->recentKbps(rig.now), quint32(0));
    }

    void theControlTickLowersTheWindowUnderPressure()
    {
        Rig rig(true, true, ClipboardFilePacing::Policy{});
        ClipboardFilePacing::Inputs quiet;
        quiet.capacityKbps = 1'000'000; // 1 Gbit/s link, idle
        quiet.sendQueueBytes = 0;
        for (int i = 0; i < 60; ++i) {
            rig.advance(100ms);
            rig.server->tick(rig.now, quiet);
        }
        QVERIFY(rig.server->pacer().window() >= 8);
        ClipboardFilePacing::Inputs busy = quiet;
        busy.sendQueueBytes = 200 * 1024;
        for (int i = 0; i < 12; ++i) {
            rig.advance(100ms);
            rig.server->tick(rig.now, busy);
        }
        QCOMPARE(rig.server->pacer().window(), 1);
        QVERIFY(rig.server->pacer().rateBitsPerSecond() <= ClipboardFilePacing::FloorBitsPerSecond * 2);
    }
};

QTEST_GUILESS_MAIN(ClipboardFileServeTest)
#include "ClipboardFileServeTest.moc"
