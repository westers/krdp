// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-078 phase 3: the broker's windowed FILECONTENTS requester for client-to-host copies, driven by a fake clock and fake callbacks.
// Spec 4.3 (responses are checked against what was requested, 3 violations abort), 5 (64-bit offsets, huge files, cancel), 6.3 (window, pacing).

#include <QTest>

#include "ClipboardFileFetch.h"

using namespace KRdp;
using namespace KRdp::ClipboardFileFetch;
using Clock = ClipboardFilePacing::Clock;
using namespace std::chrono_literals;

namespace
{
struct Harness {
    Clock::time_point now = Clock::time_point(std::chrono::seconds(1000));
    QList<Request> requests; // everything sent to the client
    QList<Chunk> chunks; // everything handed to the worker
    QList<QPair<EndReason, AckStatus>> ends;
    bool failSend = false;
    bool failDeliver = false;
    std::unique_ptr<Fetcher> fetcher;

    explicit Harness(const ClipboardFilePacing::Policy &policy = unlimited(), std::optional<quint32> clipDataId = std::nullopt)
    {
        Callbacks callbacks;
        callbacks.sendRequest = [this](const Request &request) {
            if (failSend) return false;
            requests.append(request);
            return true;
        };
        callbacks.deliver = [this](const Chunk &chunk) {
            if (failDeliver) return false;
            chunks.append(chunk);
            return true;
        };
        callbacks.ended = [this](EndReason reason, AckStatus cause) { ends.append({reason, cause}); };
        Options options;
        options.clipDataId = clipDataId;
        fetcher = std::make_unique<Fetcher>(callbacks, options, policy);
    }
    static ClipboardFilePacing::Policy unlimited()
    {
        ClipboardFilePacing::Policy policy;
        policy.unlimited = true;
        return policy;
    }
    void start(const QList<ClipboardFilePush::FetchFile> &files)
    {
        fetcher->begin(files, now);
        fetcher->pump(now);
    }
    // the client answers request #i with the right number of bytes (a pattern keyed by offset, so a mix-up shows)
    static QByteArray payload(const Request &request)
    {
        QByteArray data(int(request.length), '\0');
        for (int i = 0; i < data.size(); ++i) data[i] = char((request.offset + quint64(i) + request.index) & 0xff);
        return data;
    }
    void answer(int i, bool ok = true)
    {
        fetcher->onResponse(requests.at(i).streamId, ok, ok ? payload(requests.at(i)) : QByteArray(), now);
        fetcher->pump(now);
    }
    void ackAll()
    {
        // the worker acks everything delivered and not yet acked
        while (acked < chunks.size()) {
            const Chunk &chunk = chunks.at(acked++);
            fetcher->onAck(chunk.index, chunk.offset, quint32(chunk.data.size()), AckStatus::Ok, now);
        }
        fetcher->pump(now);
    }
    int acked = 0;
};

quint64 requestedFor(const Harness &h, quint32 index)
{
    quint64 total = 0;
    for (const auto &request : h.requests) {
        if (request.index == index) total += request.length;
    }
    return total;
}
}

class ClipboardFileFetchTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void oneSmallFile()
    {
        Harness h;
        h.start({{3, 1000}});
        QCOMPARE(h.requests.size(), 1);
        QCOMPARE(h.requests[0].index, quint32(3));
        QCOMPARE(h.requests[0].offset, quint64(0));
        QCOMPARE(h.requests[0].length, quint32(1000));
        QVERIFY(h.requests[0].streamId != 0);
        QVERIFY(!h.requests[0].clipDataId);
        h.answer(0);
        QCOMPARE(h.chunks.size(), 1);
        QCOMPARE(h.chunks[0].index, quint32(3));
        QCOMPARE(h.chunks[0].data, Harness::payload(h.requests[0]));
        QVERIFY(h.ends.isEmpty()); // not done until the worker has written it
        h.ackAll();
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::Done);
        QCOMPARE(h.fetcher->counters().bytesAcked, quint64(1000));
    }

    void nothingToFetchIsDoneAtOnce()
    {
        Harness h;
        h.start({});
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::Done);
        QVERIFY(h.requests.isEmpty());
    }

    // spec 4.4: with a locked snapshot every request names it; without, none does.
    void requestsCarryTheLockId()
    {
        Harness locked(Harness::unlimited(), 77);
        locked.start({{0, 10}});
        QCOMPARE(locked.requests[0].clipDataId, std::optional<quint32>(77));
        Harness plain;
        plain.start({{0, 10}});
        QVERIFY(!plain.requests[0].clipDataId);
    }

    // spec 6.3: the window caps requests in flight (requested + received + unacknowledged).
    void windowIsRespected()
    {
        Harness h;
        h.start({{0, 100ULL * ClipboardFileLimits::RangeBytes}});
        QCOMPARE(h.requests.size(), ClipboardFileLimits::MaxWindow);
        QCOMPARE(h.fetcher->inflight(), ClipboardFileLimits::MaxWindow);
        // an answer alone does not free a slot: the worker has not written it yet
        h.answer(0);
        QCOMPARE(h.requests.size(), ClipboardFileLimits::MaxWindow);
        h.ackAll();
        QCOMPARE(h.requests.size(), ClipboardFileLimits::MaxWindow + 1);
        QCOMPARE(h.fetcher->counters().peakInflight, ClipboardFileLimits::MaxWindow);
    }

    void aStockClientWindowIsFour()
    {
        auto policy = Harness::unlimited();
        policy.stockClient = true;
        Harness h(policy);
        h.start({{0, 100ULL * ClipboardFileLimits::RangeBytes}});
        QCOMPARE(h.requests.size(), ClipboardFileLimits::StockClientWindow);
    }

    // The worker writes strictly in order, so responses that arrive out of order are held back.
    void responsesAreDeliveredInOrder()
    {
        Harness h;
        h.start({{0, 3ULL * ClipboardFileLimits::RangeBytes}, {1, 100}});
        QCOMPARE(h.requests.size(), 4);
        h.answer(2);
        h.answer(1);
        QVERIFY(h.chunks.isEmpty());
        h.answer(0);
        QCOMPARE(h.chunks.size(), 3);
        for (int i = 0; i < 3; ++i) QCOMPARE(h.chunks[i].offset, quint64(i) * ClipboardFileLimits::RangeBytes);
        h.answer(3);
        QCOMPARE(h.chunks.size(), 4);
        QCOMPARE(h.chunks[3].index, quint32(1));
        QCOMPARE(h.chunks[3].offset, quint64(0));
    }

    void everyByteIsRequestedExactlyOnce()
    {
        Harness h;
        const QList<ClipboardFilePush::FetchFile> files{{0, 1}, {1, ClipboardFileLimits::RangeBytes}, {2, ClipboardFileLimits::RangeBytes + 1}, {5, 3ULL * ClipboardFileLimits::RangeBytes - 7}};
        h.start(files);
        int answered = 0;
        while (h.ends.isEmpty()) {
            QVERIFY2(answered < h.requests.size(), "stalled");
            h.answer(answered++);
            h.ackAll();
        }
        QCOMPARE(h.ends[0].first, EndReason::Done);
        for (const auto &file : files) QCOMPARE(requestedFor(h, file.index), file.size);
        // contiguous, in order, no request longer than the range size
        quint64 expected = 0;
        quint32 current = 0;
        for (const auto &request : h.requests) {
            if (request.index != current) {
                current = request.index;
                expected = 0;
            }
            QCOMPARE(request.offset, expected);
            QVERIFY(request.length >= 1 && request.length <= ClipboardFileLimits::RangeBytes);
            expected += request.length;
        }
        quint64 delivered = 0;
        for (const auto &chunk : h.chunks) delivered += quint64(chunk.data.size());
        QCOMPARE(delivered, quint64(1) + ClipboardFileLimits::RangeBytes + ClipboardFileLimits::RangeBytes + 1 + 3ULL * ClipboardFileLimits::RangeBytes - 7);
    }

    // spec 5: offsets are 64-bit; a 5 GiB file is walked completely and the last range is the partial tail.
    void aFiveGibFileIsWalkedWithSixtyFourBitOffsets()
    {
        Harness h;
        const quint64 size = (5ULL << 30) + 12345;
        h.start({{0, size}});
        quint64 next = 0;
        quint64 highest = 0;
        QByteArray shared(int(ClipboardFileLimits::RangeBytes), 'x');
        int answered = 0;
        while (h.ends.isEmpty()) {
            QVERIFY2(answered < h.requests.size(), "stalled");
            const Request request = h.requests.at(answered++);
            QCOMPARE(request.offset, next);
            next += request.length;
            highest = std::max(highest, request.offset);
            h.fetcher->onResponse(request.streamId, true, shared.left(int(request.length)), h.now);
            // acknowledge as the worker would
            while (h.acked < h.chunks.size()) {
                const Chunk &chunk = h.chunks.at(h.acked++);
                h.fetcher->onAck(chunk.index, chunk.offset, quint32(chunk.data.size()), AckStatus::Ok, h.now);
            }
            h.chunks.clear();
            h.acked = 0;
            h.fetcher->pump(h.now);
        }
        QCOMPARE(next, size);
        QVERIFY(highest > 0xffffffffULL);
        QCOMPARE(h.requests.last().length, quint32(size % ClipboardFileLimits::RangeBytes));
        QCOMPARE(h.ends[0].first, EndReason::Done);
    }

    // spec 4.3: a response is exactly what was asked for.
    void aShortResponseEndsTheTransfer()
    {
        Harness h;
        h.start({{0, 1000}});
        h.fetcher->onResponse(h.requests[0].streamId, true, QByteArray(999, 'a'), h.now);
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::PeerShort);
        QVERIFY(h.chunks.isEmpty()); // nothing is passed on
    }
    void anOverlongResponseIsCutOff()
    {
        Harness h;
        h.start({{0, 1000}});
        h.fetcher->onResponse(h.requests[0].streamId, true, QByteArray(1001, 'a'), h.now);
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::PeerOversize);
        QVERIFY(h.chunks.isEmpty());
    }
    void aFailedResponseEndsTheTransfer()
    {
        Harness h;
        h.start({{0, 1000}, {1, 10}});
        h.fetcher->onResponse(h.requests[0].streamId, false, {}, h.now);
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::PeerFailed);
    }
    void responsesNobodyAskedForAreViolations()
    {
        Harness h;
        h.start({{0, 5ULL * ClipboardFileLimits::RangeBytes}});
        const auto before = h.requests.size();
        h.fetcher->onResponse(0xdead, true, QByteArray(10, 'a'), h.now);
        h.fetcher->onResponse(0xbeef, true, QByteArray(10, 'a'), h.now);
        QVERIFY(h.ends.isEmpty());
        QCOMPARE(h.fetcher->counters().violations, 2);
        QVERIFY(h.chunks.isEmpty());
        h.fetcher->onResponse(0xf00d, true, QByteArray(10, 'a'), h.now); // the third in one transfer aborts it
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::Violations);
        QCOMPARE(h.requests.size(), before); // and nothing more is asked for
    }
    void aStreamIsAnsweredOnce()
    {
        Harness h;
        h.start({{0, 3ULL * ClipboardFileLimits::RangeBytes}});
        h.answer(0);
        h.fetcher->onResponse(h.requests[0].streamId, true, Harness::payload(h.requests[0]), h.now); // the same stream again
        QCOMPARE(h.fetcher->counters().violations, 1);
        QCOMPARE(h.chunks.size(), 1); // the duplicate was not passed on
    }
    void streamIdsAreUniqueAmongOutstanding()
    {
        Harness h;
        h.start({{0, 100ULL * ClipboardFileLimits::RangeBytes}});
        QSet<quint32> ids;
        for (const auto &request : h.requests) ids.insert(request.streamId);
        QCOMPARE(ids.size(), h.requests.size());
    }

    void theWorkerRefusingStopsTheTransfer_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("no space") << int(AckStatus::NoSpace);
        QTest::newRow("io error") << int(AckStatus::IoError);
        QTest::newRow("denied") << int(AckStatus::Denied);
        QTest::newRow("gone") << int(AckStatus::Gone);
        QTest::newRow("out of order") << int(AckStatus::OutOfOrder);
    }
    void theWorkerRefusingStopsTheTransfer()
    {
        QFETCH(int, status);
        Harness h;
        h.start({{0, 3ULL * ClipboardFileLimits::RangeBytes}});
        h.answer(0);
        h.fetcher->onAck(h.chunks[0].index, h.chunks[0].offset, quint32(h.chunks[0].data.size()), AckStatus(status), h.now);
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::WorkerRefused);
        QCOMPARE(int(h.ends[0].second), status);
        const auto sent = h.requests.size();
        h.fetcher->pump(h.now);
        QCOMPARE(h.requests.size(), sent);
    }
    void aWorkerThatCannotBeWrittenToEndsTheTransfer()
    {
        Harness h;
        h.start({{0, 100}});
        h.failDeliver = true;
        h.answer(0);
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::WorkerUnreachable);
    }
    void aClientThatCannotBeWrittenToEndsTheTransfer()
    {
        Harness h;
        h.failSend = true;
        h.start({{0, 100}});
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::PeerUnreachable);
    }
    void anAckNobodyWaitsForIsIgnored()
    {
        Harness h;
        h.start({{0, 3ULL * ClipboardFileLimits::RangeBytes}});
        h.fetcher->onAck(0, 0, 10, AckStatus::Ok, h.now);
        h.fetcher->onAck(9, 12345, 10, AckStatus::NoSpace, h.now);
        QVERIFY(h.ends.isEmpty());
        QCOMPARE(h.fetcher->counters().strayAcks, 2);
    }

    void timeouts()
    {
        Harness h;
        h.start({{0, 2ULL * ClipboardFileLimits::RangeBytes}});
        h.now += std::chrono::milliseconds(ResponseTimeoutMs - 1);
        h.fetcher->checkTimeouts(h.now);
        QVERIFY(h.ends.isEmpty());
        h.now += 2ms;
        h.fetcher->checkTimeouts(h.now);
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::Timeout);

        Harness w;
        w.start({{0, 100}});
        w.answer(0);
        w.now += std::chrono::milliseconds(AckTimeoutMs + 1);
        w.fetcher->checkTimeouts(w.now);
        QCOMPARE(w.ends.size(), 1);
        QCOMPARE(w.ends[0].first, EndReason::WorkerTimeout);
    }

    // spec 5: cancel is honoured at once; answers to requests already out are dropped, not violations.
    void cancelDropsLateAnswers()
    {
        Harness h;
        h.start({{0, 10ULL * ClipboardFileLimits::RangeBytes}});
        const int out = h.requests.size();
        h.fetcher->cancel(h.now);
        QCOMPARE(h.ends.size(), 1);
        QCOMPARE(h.ends[0].first, EndReason::Cancelled);
        for (int i = 0; i < out; ++i) h.answer(i);
        h.fetcher->onAck(0, 0, 1, AckStatus::Ok, h.now);
        QVERIFY(h.chunks.isEmpty());
        QCOMPARE(h.requests.size(), out);
        QCOMPARE(h.ends.size(), 1); // exactly once
        QCOMPARE(h.fetcher->counters().violations, 0);
        h.fetcher->cancel(h.now);
        QCOMPARE(h.ends.size(), 1);
    }

    // spec 6.3: under pressure the window shrinks to one request and the request size to the slice floor; nothing is asked faster than the bucket allows.
    void pressureShrinksTheWindowAndTheRequest()
    {
        ClipboardFilePacing::Policy policy; // Auto
        Harness h(policy);
        h.start({{0, 1000ULL * ClipboardFileLimits::RangeBytes}});
        // the start rate is 4 x floor = 128 kB/s: window 1, requests of 16 KiB (the slice floor)
        QCOMPARE(h.requests.size(), 1);
        QCOMPARE(h.requests[0].length, ClipboardFilePacing::MinSliceBytes);
        // a LAN: capacity known, link idle, no pressure: it ramps up
        ClipboardFilePacing::Inputs lan;
        lan.capacityKbps = 800'000;
        lan.otherKbps = 0;
        for (int i = 0; i < 400; ++i) {
            h.now += ClipboardFilePacing::Tick;
            h.fetcher->tick(h.now, lan);
        }
        QVERIFY(h.fetcher->window() > 4);
        QVERIFY(h.fetcher->requestBytes() > ClipboardFilePacing::MinSliceBytes);
        // video backlog: back to the floor within a second
        ClipboardFilePacing::Inputs busy = lan;
        busy.videoBacklog = true;
        for (int i = 0; i < 12; ++i) {
            h.now += ClipboardFilePacing::Tick;
            h.fetcher->tick(h.now, busy);
        }
        QCOMPARE(h.fetcher->window(), 1);
        QCOMPARE(h.fetcher->requestBytes(), ClipboardFilePacing::MinSliceBytes);
    }
    void theTokenBucketPacesNewRequests()
    {
        ClipboardFilePacing::Policy policy;
        policy.capBytesPerSecond = 160'000; // 160 kB/s fixed cap
        Harness h(policy);
        h.start({{0, 1000ULL * ClipboardFileLimits::RangeBytes}});
        // answer and ack each request as soon as it is sent; count how many are sent in 10 s of fake time
        int answered = 0;
        for (int step = 0; step < 1000; ++step) { // 10 ms steps
            while (answered < h.requests.size()) {
                h.answer(answered++);
                h.ackAll();
            }
            h.now += 10ms;
            h.fetcher->tick(h.now, {});
            h.fetcher->pump(h.now);
        }
        quint64 bytes = 0;
        for (const auto &request : h.requests) bytes += request.length;
        QVERIFY2(bytes <= 160'000ULL * 10 + 100'000, qPrintable(QString::number(bytes)));
        QVERIFY2(bytes >= 160'000ULL * 10 / 2, qPrintable(QString::number(bytes)));
        QVERIFY(h.fetcher->delayUntilNext(h.now) >= 0ms);
    }

    // spec 4: counters only, and the end reason names are fixed words.
    void reasonNamesAreFixedWords()
    {
        for (quint8 r = 0; r <= quint8(EndReason::PeerUnreachable); ++r) {
            const QByteArray name = endReasonName(EndReason(r));
            QVERIFY(!name.isEmpty() && !name.contains(' '));
        }
        for (quint8 s = 0; s <= LastAckStatus; ++s) QVERIFY(QByteArray(ackStatusName(AckStatus(s))).size() > 0);
    }
};

QTEST_APPLESS_MAIN(ClipboardFileFetchTest)
#include "ClipboardFileFetchTest.moc"
