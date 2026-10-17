// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <deque>
#include <functional>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QtGlobal>

#include "ClipboardFileLimits.h"
#include "ClipboardFilePacing.h"
#include "ClipboardFileRequest.h"

namespace KRdp::ClipboardFileServe
{
/**
 * OPT-078 phase 2 (spec 5, 6.3): the host-to-client side of FILECONTENTS, as a state machine with no I/O of its own, so the
 * window, the pacing, the locks, the failures and the aborts are tested with a fake clock and fake callbacks.
 *
 * The owner (the broker's Clipboard) feeds it the peer's requests and the worker's answers; it calls back to read a range, to write
 * a response and to end an offer. One request becomes one worker read and one response:
 *
 *   onRequest()  -> RequestValidator (spec 4.3). SIZE is answered at once from the snapshot (8 bytes, no worker). A bad request is
 *                   answered with a failed response (the third one aborts the transfer); one over the outstanding limit is refused.
 *                   A RANGE waits in a FIFO. Events only change state: the owner calls pump() after each one (with the live send
 *                   queue) and from a timer while delayUntilNext() says something waits.
 *   pump()       -> reads are issued while (reads in flight + responses waiting for the pacer) < the pacer's window W; a ready response
 *                   is written when the pacer allows (token bucket, and the socket's send queue below the head-of-line bound), at
 *                   most MaxResponsesPerPass per call so the session thread is never held. A failed read answers its stream FAIL.
 *   A changed or vanished file (or any read that cannot be served) fails THAT stream and ends the transfer: every other request of the
 *   same offer is answered FAIL and the worker is told to cancel; new requests for it are answered FAIL until a new offer. Nothing is
 *   ever truncated or padded.
 *
 * Without locking (CB_CAN_LOCK_CLIPDATA not negotiated) a new offer ends the old one's requests (the simple model). With locking the
 * locks keep their snapshots; a request carrying a clipDataId is served from that snapshot's offer.
 */
constexpr int MaxResponsesPerPass = 4;

enum class ReadStatus { Ok, Changed, Gone, Denied, IoError, NoSuchOffer, NotSent };

inline const char *readStatusName(ReadStatus status)
{
    switch (status) {
    case ReadStatus::Ok: return "ok";
    case ReadStatus::Changed: return "changed";
    case ReadStatus::Gone: return "gone";
    case ReadStatus::Denied: return "denied";
    case ReadStatus::IoError: return "io-error";
    case ReadStatus::NoSuchOffer: return "no-such-offer";
    case ReadStatus::NotSent: return "not-sent";
    }
    return "unknown";
}

struct ReadIssue {
    quint32 streamId = 0;
    quint64 seq = 0;
    quint32 index = 0;
    quint64 offset = 0;
    quint32 length = 0;
};

/// Why a transfer ended (counted, shown to the client as a reason code, never with a name).
enum class EndReason { Failed, Aborted, Reset };

struct Callbacks {
    std::function<bool(const ReadIssue &)> issueRead; ///< false: the read could not be sent (counts as a failed read)
    std::function<void(quint32 streamId, bool ok, const QByteArray &data)> respond; ///< one FILECONTENTS_RESPONSE
    std::function<void(quint64 seq)> cancelOffer; ///< the worker should drop the reads of \a seq
    std::function<void(quint32 clipDataId)> unlockEvicted; ///< a lock dropped to make room (the worker releases it too)
    std::function<void(quint64 seq, EndReason reason, ReadStatus cause)> ended; ///< a transfer ended before the client was done
};

struct Counters {
    quint64 requests = 0;
    quint64 sizes = 0;
    quint64 ranges = 0; ///< ranges answered with data
    quint64 bytes = 0; ///< bytes written in responses
    quint64 failedResponses = 0;
    quint64 violations = 0;
    quint64 refused = 0;
    quint64 readsFailed = 0;
    quint64 aborts = 0;
    int peakWindow = 0;
};

class Server
{
public:
    using Clock = ClipboardFilePacing::Clock;

    Server(Callbacks callbacks, bool lockingNegotiated, bool hugeNegotiated, ClipboardFilePacing::Policy policy = {})
        : m_callbacks(std::move(callbacks))
        , m_locking(lockingNegotiated)
        , m_huge(hugeNegotiated)
        , m_validator(lockingNegotiated, ClipboardFileLimits::MaxOutstanding, hugeNegotiated)
        , m_pacer(policy)
    {
    }

    // ---- the offer and its locks ----

    /// The client now has the descriptor of offer \a seq, whose entries are \a snapshot. A lock the client sent before the descriptor binds to it.
    void setOffer(quint64 seq, ClipboardFileRequest::Snapshot snapshot, Clock::time_point now)
    {
        if (m_haveOffer && !m_locking) {
            endOffer(m_currentSeq, EndReason::Reset, ReadStatus::NotSent, false); // the simple model: a new offer ends the old one's requests
        }
        m_now = now;
        m_validator.clearViolations(); // "3 violations in one transfer": a new offer is a new transfer
        m_failed.remove(seq);
        m_currentSeq = seq;
        m_haveOffer = true;
        m_validator.setCurrent(snapshot);
        m_snapshot = snapshot;
        if (!m_started) {
            m_started = true;
            m_pacer.reset(now);
        }
        const auto pending = std::exchange(m_pendingLocks, {});
        for (const quint32 id : pending) bindLock(id);
    }

    /// The announcement was withdrawn or replaced before a descriptor: pending locks are for something that is gone.
    void clearOffer()
    {
        m_pendingLocks.clear();
        if (m_haveOffer && !m_locking) endOffer(m_currentSeq, EndReason::Reset, ReadStatus::NotSent, false);
        m_haveOffer = false;
        m_validator.clearCurrent();
    }

    /// CB_LOCK_CLIPDATA. Binds to the current offer; before the descriptor it waits for it. Returns false when locking is not negotiated or the id repeats.
    bool lock(quint32 clipDataId)
    {
        if (!m_locking) return false;
        if (!m_haveOffer) {
            if (!m_pendingLocks.contains(clipDataId)) m_pendingLocks.append(clipDataId);
            return true;
        }
        return bindLock(clipDataId);
    }

    /// CB_UNLOCK_CLIPDATA. The offer behind it is ended when nothing else holds it and it is not the current one. Returns the offer seq it released (0 = none).
    quint64 unlock(quint32 clipDataId)
    {
        m_pendingLocks.removeAll(clipDataId);
        if (!m_validator.unlock(clipDataId)) return 0;
        const quint64 seq = m_lockSeq.take(clipDataId);
        // Requests still carrying this id lose their snapshot: they cannot be served any more.
        for (auto &item : m_items) {
            if (item.request.clipDataId == clipDataId && item.state != State::Done) fail(item, ReadStatus::Gone);
        }
        return seq;
    }

    bool hasOffer() const { return m_haveOffer; }
    quint64 currentSeq() const { return m_currentSeq; }
    int lockCount() const { return m_validator.lockCount(); }
    quint64 lockedSeq(quint32 clipDataId) const { return m_lockSeq.value(clipDataId, 0); }

    // ---- the client's requests ----

    void onRequest(const ClipboardFileRequest::Request &request, Clock::time_point now)
    {
        m_now = now;
        ++m_counters.requests;
        // Which offer: the lock's, or the current one.
        quint64 seq = m_currentSeq;
        if (request.clipDataId) {
            if (const auto it = m_lockSeq.constFind(*request.clipDataId); it != m_lockSeq.constEnd()) seq = it.value();
        }
        const auto decision = m_validator.validate(request);
        switch (decision.verdict) {
        case ClipboardFileRequest::Verdict::Abort:
            if (decision.reason != ClipboardFileRequest::Reason::Aborted) ++m_counters.violations;
            fail(request.streamId);
            if (decision.reason != ClipboardFileRequest::Reason::Aborted) {
                ++m_counters.aborts;
                endOffer(seq, EndReason::Aborted, ReadStatus::NotSent, true);
            }
            return;
        case ClipboardFileRequest::Verdict::Fail:
            ++m_counters.violations;
            fail(request.streamId);
            return;
        case ClipboardFileRequest::Verdict::Refuse:
            ++m_counters.refused;
            fail(request.streamId);
            return;
        case ClipboardFileRequest::Verdict::Accept:
            break;
        }
        if (m_failed.contains(seq)) { // the transfer already ended: nothing more is served from it
            m_validator.complete(request.streamId);
            fail(request.streamId);
            return;
        }
        if (request.flags == ClipboardFileRequest::FlagSize) {
            QByteArray size(8, '\0');
            for (int i = 0; i < 8; ++i) size[i] = char((decision.fileSize >> (8 * i)) & 0xff);
            m_validator.complete(request.streamId);
            ++m_counters.sizes;
            m_callbacks.respond(request.streamId, true, size);
            return;
        }
        Item item;
        item.request = request;
        item.seq = seq;
        item.state = State::Waiting;
        item.queuedAt = now;
        m_items.push_back(std::move(item));
    }

    /// The worker's answer to the read issued for \a streamId.
    void readFinished(quint32 streamId, ReadStatus status, const QByteArray &data, Clock::time_point now)
    {
        m_now = now;
        Item *item = find(streamId);
        if (!item || item->state != State::Reading) return; // answered already (the transfer ended) or never asked
        if (status != ReadStatus::Ok) {
            ++m_counters.readsFailed;
            const quint64 seq = item->seq;
            fail(*item, status);
            endOffer(seq, EndReason::Failed, status, true);
            return;
        }
        if (quint32(data.size()) != item->request.length) { // a worker answering with other than what was asked is not served
            ++m_counters.readsFailed;
            const quint64 seq = item->seq;
            fail(*item, ReadStatus::IoError);
            endOffer(seq, EndReason::Failed, ReadStatus::IoError, true);
            return;
        }
        item->state = State::Ready;
        item->data = data;
    }

    // ---- pacing ----

    void setPolicy(const ClipboardFilePacing::Policy &policy) { m_pacer.setPolicy(policy); }
    const ClipboardFilePacing::FileTransferPacer &pacer() const { return m_pacer; }
    /// One pacer control tick (the owner calls it every ClipboardFilePacing::Tick with the connection's evidence).
    void tick(Clock::time_point now, const ClipboardFilePacing::Inputs &inputs)
    {
        m_now = now;
        m_pacer.tick(now, inputs);
    }

    /// Issues reads and writes responses as far as the pacer allows. \a sendQueueBytes: the socket's send queue (SIOCOUTQ), nullopt = unknown.
    void pump(Clock::time_point now, std::optional<qint64> sendQueueBytes)
    {
        if (m_pumping) return; // a callback re-entered (a response arriving synchronously)
        m_now = now;
        m_pumping = true;
        // Failed items are answered at once: there is nothing to pace.
        for (auto it = m_items.begin(); it != m_items.end();) {
            if (it->state == State::Failed) {
                m_validator.complete(it->request.streamId);
                const quint32 stream = it->request.streamId;
                it = m_items.erase(it);
                ++m_counters.failedResponses;
                m_callbacks.respond(stream, false, {});
            } else {
                ++it;
            }
        }
        // 1. reads, inside the window; 2. responses, as the pacer allows; 3. the reads the responses made room for
        if (!issueReads(now, sendQueueBytes)) return;
        std::optional<qint64> queued = sendQueueBytes;
        int passed = 0;
        for (auto it = m_items.begin(); it != m_items.end() && passed < MaxResponsesPerPass;) {
            if (it->state != State::Ready) {
                ++it;
                continue;
            }
            const quint32 length = quint32(it->data.size());
            const quint32 slice = std::min(length, ClipboardFilePacing::MaxSliceBytes);
            if (!m_pacer.maySend(now, queued, slice)) break; // oldest first: do not let a later one overtake
            const quint32 stream = it->request.streamId;
            const QByteArray data = std::move(it->data);
            it = m_items.erase(it);
            m_validator.complete(stream);
            m_pacer.consume(now, length);
            ++m_counters.ranges;
            m_counters.bytes += length;
            m_recent.append({now, length});
            if (queued) *queued += length; // what this pass handed over is queued ahead of the next one
            ++passed;
            m_callbacks.respond(stream, true, data);
        }
        if (passed && !issueReads(now, sendQueueBytes)) return;
        m_pumping = false;
    }

    /// How long until the pacer would let the next ready response go (0: now, or nothing waits).
    std::chrono::milliseconds delayUntilNext(Clock::time_point now)
    {
        for (const auto &item : m_items) {
            if (item.state == State::Ready) return m_pacer.delayUntilSend(now);
        }
        return std::chrono::milliseconds(0);
    }

    /// File bytes written in the last second, in kbit/s (the stats count them as part of the connection's rate: the pacer's "other traffic" excludes them).
    quint32 recentKbps(Clock::time_point now)
    {
        while (!m_recent.isEmpty() && now - m_recent.first().at > std::chrono::seconds(1)) m_recent.removeFirst();
        quint64 bytes = 0;
        for (const auto &entry : std::as_const(m_recent)) bytes += entry.bytes;
        return quint32(std::min<quint64>(bytes * 8 / 1000, 0xffffffffu));
    }

    // ---- end ----

    /// Everything pending is answered FAIL and the worker told to cancel: the policy turned off, control moved, the client left or asked to cancel.
    void reset(EndReason reason = EndReason::Reset)
    {
        m_pendingLocks.clear();
        QSet<quint64> seqs;
        for (const auto &item : m_items) seqs.insert(item.seq);
        for (const quint64 seq : std::as_const(seqs)) endOffer(seq, reason, ReadStatus::NotSent, false);
        for (auto &item : m_items) {
            if (item.state != State::Done && item.state != State::Failed) fail(item, ReadStatus::NotSent);
        }
        pump(m_now, std::nullopt);
        m_haveOffer = false;
        m_validator.clearCurrent();
    }

    int pending() const { return int(m_items.size()); }
    bool aborted() const { return m_validator.aborted(); }
    const Counters &counters() const { return m_counters; }
    bool locking() const { return m_locking; }
    bool huge() const { return m_huge; }

private:
    enum class State { Waiting, Reading, Ready, Failed, Done };
    struct Item {
        ClipboardFileRequest::Request request;
        quint64 seq = 0;
        State state = State::Waiting;
        QByteArray data;
        Clock::time_point queuedAt;
    };
    struct Recent {
        Clock::time_point at;
        quint64 bytes;
    };

    Item *find(quint32 streamId)
    {
        for (auto &item : m_items) {
            if (item.request.streamId == streamId) return &item;
        }
        return nullptr;
    }

    // A response with no request behind it (a request the validator refused).
    void fail(quint32 streamId)
    {
        ++m_counters.failedResponses;
        m_callbacks.respond(streamId, false, {});
    }
    void fail(Item &item, ReadStatus)
    {
        item.state = State::Failed;
        item.data.clear();
    }

    // Issues reads for the waiting requests while the window has room. Returns false when a read could not be sent: the transfer was
    // ended and pump() was run again (the caller returns).
    bool issueReads(Clock::time_point now, std::optional<qint64> sendQueueBytes)
    {
        const int window = m_pacer.window();
        m_counters.peakWindow = std::max(m_counters.peakWindow, window);
        int inflight = 0;
        for (const auto &item : m_items) {
            if (item.state == State::Reading || item.state == State::Ready) ++inflight;
        }
        for (auto &item : m_items) {
            if (inflight >= window) break;
            if (item.state != State::Waiting) continue;
            ReadIssue issue{item.request.streamId, item.seq, item.request.listIndex, item.request.offset, item.request.length};
            item.state = State::Reading;
            ++inflight;
            if (!m_callbacks.issueRead || !m_callbacks.issueRead(issue)) {
                ++m_counters.readsFailed;
                const quint64 seq = item.seq;
                fail(item, ReadStatus::NotSent);
                m_pumping = false;
                endOffer(seq, EndReason::Failed, ReadStatus::NotSent, true);
                pump(now, sendQueueBytes);
                return false;
            }
        }
        return true;
    }

    bool bindLock(quint32 clipDataId)
    {
        const auto result = m_validator.lock(clipDataId, m_snapshot);
        if (!result.ok) return false;
        m_lockSeq.insert(clipDataId, m_currentSeq);
        if (result.evicted) {
            m_lockSeq.remove(*result.evicted);
            if (m_callbacks.unlockEvicted) m_callbacks.unlockEvicted(*result.evicted);
        }
        return true;
    }

    // The transfer of \a seq is over: its requests are answered FAIL on the next pump, the worker drops its reads.
    void endOffer(quint64 seq, EndReason reason, ReadStatus cause, bool notify)
    {
        m_failed.insert(seq);
        for (auto &item : m_items) {
            if (item.seq == seq && (item.state == State::Waiting || item.state == State::Reading || item.state == State::Ready)) fail(item, cause);
        }
        if (m_callbacks.cancelOffer) m_callbacks.cancelOffer(seq);
        if (notify && m_callbacks.ended) m_callbacks.ended(seq, reason, cause);
    }

    Callbacks m_callbacks;
    bool m_locking;
    bool m_huge;
    ClipboardFileRequest::RequestValidator m_validator;
    ClipboardFilePacing::FileTransferPacer m_pacer;
    ClipboardFileRequest::Snapshot m_snapshot;
    quint64 m_currentSeq = 0;
    bool m_haveOffer = false;
    bool m_started = false;
    bool m_pumping = false;
    Clock::time_point m_now{}; ///< the latest time any call was given (internal pumps use it; the class reads no clock)
    QList<quint32> m_pendingLocks;
    QHash<quint32, quint64> m_lockSeq;
    QSet<quint64> m_failed; ///< offers whose transfer ended
    std::deque<Item> m_items;
    QList<Recent> m_recent;
    Counters m_counters;
};
}
