// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <deque>
#include <functional>
#include <optional>

#include <QByteArray>
#include <QList>
#include <QSet>
#include <QtGlobal>

#include "ClipboardFileLimits.h"
#include "ClipboardFilePacing.h"
#include "ClipboardFilePush.h"

namespace KRdp::ClipboardFileFetch
{
/**
 * OPT-078 phase 3 (client to host, spec 5 and 6.3): the broker's side of a client-to-host file copy, as a state machine with no I/O of
 * its own (the mirror image of ClipboardFileServe, which serves the host's files to a client). It asks the CLIENT for the bytes of the
 * files in a push plan with FILECONTENTS_RANGE requests, hands each answer to the host's worker to write, and counts a request as done
 * only when the worker has written it:
 *
 *   in flight = requested + answered-but-undelivered + delivered-but-unacknowledged  <=  the pacer's window (16; 4 for a stock client)
 *
 * so a slow disk or a slow link both stop new requests, and the bytes in flight on the link are bounded by the window times the
 * request size. Request size and the rate at which new requests are issued come from the same FileTransferPacer as the other
 * direction (floor 256 kbit/s, AIMD), so a transfer yields to video and audio; the client additionally paces its own responses.
 *
 * Hostile peers (spec 4.3): a response must be exactly the bytes asked for (shorter: the file shrank under the client; longer: cut
 * off), a response for a stream that is not outstanding is a violation (the third one in a transfer aborts it), a failed response
 * ends the transfer (the client could not read the file), nothing is ever requested beyond the sizes the descriptor declared,
 * and the worker's answers are checked the same way. The worker writes strictly in order, so answers that arrive early are held back
 * and delivered in request order.
 *
 * The class reads no clock and makes no call except through Callbacks; it ends exactly once (\a ended) and is silent afterwards:
 * late answers to requests that were still out are dropped without counting as violations.
 */
constexpr int ResponseTimeoutMs = 30'000; ///< a request unanswered for this long: the client is gone or stuck
constexpr int AckTimeoutMs = 60'000; ///< a delivered chunk unacknowledged for this long: the worker is stuck

/// How the worker answered one chunk (mirrors the wire's FilePushAck status).
enum class AckStatus : quint8 { Ok, NoSpace, IoError, Denied, Gone, OutOfOrder, Exists };
constexpr quint8 LastAckStatus = quint8(AckStatus::Exists);

inline const char *ackStatusName(AckStatus status)
{
    switch (status) {
    case AckStatus::Ok: return "ok";
    case AckStatus::NoSpace: return "no-space";
    case AckStatus::IoError: return "io-error";
    case AckStatus::Denied: return "denied";
    case AckStatus::Gone: return "gone";
    case AckStatus::OutOfOrder: return "out-of-order";
    case AckStatus::Exists: return "exists";
    }
    return "unknown";
}

enum class EndReason : quint8 {
    Done, ///< every byte requested, answered and written
    Cancelled, ///< cancel(): a newer copy, the policy, the connection
    PeerFailed, ///< the client answered a request with a failure
    PeerShort, ///< the client answered with fewer bytes than asked
    PeerOversize, ///< the client answered with more bytes than asked
    Violations, ///< the third answer nobody asked for
    Timeout, ///< the client did not answer in time
    WorkerRefused, ///< the worker refused a chunk (see the cause)
    WorkerTimeout, ///< the worker did not acknowledge in time
    WorkerUnreachable, ///< a chunk could not be handed to the worker
    PeerUnreachable, ///< a request could not be sent to the client
};

inline const char *endReasonName(EndReason reason)
{
    switch (reason) {
    case EndReason::Done: return "done";
    case EndReason::Cancelled: return "cancelled";
    case EndReason::PeerFailed: return "peer-failed";
    case EndReason::PeerShort: return "peer-short";
    case EndReason::PeerOversize: return "peer-oversize";
    case EndReason::Violations: return "violations";
    case EndReason::Timeout: return "timeout";
    case EndReason::WorkerRefused: return "worker-refused";
    case EndReason::WorkerTimeout: return "worker-timeout";
    case EndReason::WorkerUnreachable: return "worker-unreachable";
    case EndReason::PeerUnreachable: return "peer-unreachable";
    }
    return "unknown";
}

/// One FILECONTENTS_RANGE request for the client.
struct Request {
    quint32 streamId = 0;
    quint32 index = 0; ///< the descriptor's listIndex
    quint64 offset = 0;
    quint32 length = 0;
    std::optional<quint32> clipDataId; ///< the locked snapshot the request reads from (when locking was negotiated)
};

/// Bytes for the worker to write at (index, offset); delivered in request order.
struct Chunk {
    quint32 index = 0;
    quint64 offset = 0;
    QByteArray data;
};

struct Callbacks {
    std::function<bool(const Request &)> sendRequest; ///< false: could not be sent (ends the transfer)
    std::function<bool(const Chunk &)> deliver; ///< false: could not reach the worker (ends the transfer)
    std::function<void(EndReason reason, AckStatus cause)> ended; ///< exactly once
};

struct Options {
    std::optional<quint32> clipDataId;
};

struct Counters {
    quint64 requests = 0;
    quint64 bytesRequested = 0;
    quint64 responses = 0;
    quint64 bytesDelivered = 0;
    quint64 bytesAcked = 0;
    int violations = 0;
    int strayAcks = 0;
    int lateDropped = 0;
    int peakInflight = 0;
};

class Fetcher
{
public:
    using Clock = ClipboardFilePacing::Clock;

    Fetcher(Callbacks callbacks, Options options, ClipboardFilePacing::Policy policy = {})
        : m_callbacks(std::move(callbacks))
        , m_options(options)
        , m_pacer(policy)
    {
    }

    /// The files to fetch, in request order (the plan's fetchList()). An empty list is done at once.
    void begin(const QList<ClipboardFilePush::FetchFile> &files, Clock::time_point now)
    {
        m_now = now;
        m_files = files;
        m_fileIndex = 0;
        m_offset = 0;
        m_began = true;
        m_pacer.reset(now);
        if (m_files.isEmpty()) {
            end(EndReason::Done, AckStatus::Ok);
        }
    }

    void setPolicy(const ClipboardFilePacing::Policy &policy) { m_pacer.setPolicy(policy); }
    const ClipboardFilePacing::FileTransferPacer &pacer() const { return m_pacer; }
    int window() const { return m_pacer.window(); }
    /// The size of the next request: what the window holds for about 100 ms at the current rate, between the slice floor and the range size.
    quint32 requestBytes() const
    {
        if (m_pacer.unlimited()) return ClipboardFileLimits::RangeBytes;
        const double wanted = m_pacer.rateBytesPerSecond() * ClipboardFilePacing::WindowSeconds;
        return quint32(std::clamp(wanted, double(ClipboardFilePacing::MinSliceBytes), double(ClipboardFileLimits::RangeBytes)));
    }

    /// One pacer control tick (the owner calls it every ClipboardFilePacing::Tick with the connection's evidence).
    void tick(Clock::time_point now, const ClipboardFilePacing::Inputs &inputs)
    {
        m_now = now;
        m_pacer.tick(now, inputs);
    }

    /// Issues requests while the window and the pacer's bucket allow, and delivers what is ready. Call after every event and while delayUntilNext() says so.
    void pump(Clock::time_point now)
    {
        m_now = now;
        if (!m_began || m_ended || m_pumping) return;
        m_pumping = true;
        deliverReady();
        while (!m_ended && moreToRequest() && inflight() < m_pacer.window()) {
            const quint32 length = nextLength();
            if (!m_pacer.maySend(now, std::nullopt, length)) break;
            if (!issue(length, now)) break;
        }
        if (!m_ended) checkDone();
        m_pumping = false;
    }

    /// How long until the bucket would let the next request go (0: now, or nothing waits).
    std::chrono::milliseconds delayUntilNext(Clock::time_point now)
    {
        if (m_ended || !moreToRequest() || inflight() >= m_pacer.window()) return std::chrono::milliseconds(0);
        return m_pacer.delayUntilSend(now);
    }

    /// The client's answer to the request for \a streamId. \a ok false: it answered with a failure.
    void onResponse(quint32 streamId, bool ok, const QByteArray &data, Clock::time_point now)
    {
        m_now = now;
        if (m_ended) {
            ++m_counters.lateDropped; // requests were still out when it ended
            return;
        }
        Item *item = nullptr;
        for (auto &candidate : m_items) {
            if (candidate.request.streamId == streamId && candidate.state == State::Requested) {
                item = &candidate;
                break;
            }
        }
        if (!item) {
            violation(); // not outstanding (never asked, or already answered)
            return;
        }
        ++m_counters.responses;
        if (!ok) {
            end(EndReason::PeerFailed, AckStatus::Ok);
            return;
        }
        if (quint64(data.size()) < item->request.length) {
            end(EndReason::PeerShort, AckStatus::Ok);
            return;
        }
        if (quint64(data.size()) > item->request.length) {
            end(EndReason::PeerOversize, AckStatus::Ok);
            return;
        }
        item->state = State::Received;
        item->data = data;
        deliverReady();
    }

    /// The client's answer to \a streamId declared more than a range can be, so it was not even copied: the same as an overlong answer.
    void onResponseTooLong(quint32 streamId, Clock::time_point now)
    {
        m_now = now;
        if (m_ended) {
            ++m_counters.lateDropped;
            return;
        }
        const bool outstanding = std::any_of(m_items.begin(), m_items.end(), [&](const Item &item) { return item.request.streamId == streamId && item.state == State::Requested; });
        if (!outstanding) {
            violation();
            return;
        }
        ++m_counters.responses;
        end(EndReason::PeerOversize, AckStatus::Ok);
    }

    /// The worker's answer to the chunk at (\a index, \a offset).
    void onAck(quint32 index, quint64 offset, quint32 length, AckStatus status, Clock::time_point now)
    {
        m_now = now;
        if (m_ended) return;
        auto it = std::find_if(m_items.begin(), m_items.end(), [&](const Item &item) {
            return item.state == State::Delivered && item.request.index == index && item.request.offset == offset && item.request.length == length;
        });
        if (it == m_items.end()) {
            ++m_counters.strayAcks;
            return;
        }
        if (status != AckStatus::Ok) {
            end(EndReason::WorkerRefused, status);
            return;
        }
        m_counters.bytesAcked += length;
        m_items.erase(it);
    }

    void checkTimeouts(Clock::time_point now)
    {
        m_now = now;
        if (m_ended) return;
        for (const auto &item : m_items) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - item.at).count();
            if (item.state == State::Requested && age > ResponseTimeoutMs) {
                end(EndReason::Timeout, AckStatus::Ok);
                return;
            }
            if (item.state == State::Delivered && age > AckTimeoutMs) {
                end(EndReason::WorkerTimeout, AckStatus::Ok);
                return;
            }
        }
    }

    /// A newer copy, a policy change, the end of the connection: nothing more is asked or passed on.
    void cancel(Clock::time_point now)
    {
        m_now = now;
        end(EndReason::Cancelled, AckStatus::Ok);
    }

    int inflight() const { return int(m_items.size()); }
    bool ended() const { return m_ended; }
    const Counters &counters() const { return m_counters; }
    /// Bytes the worker has acknowledged (for progress).
    quint64 doneBytes() const { return m_counters.bytesAcked; }

private:
    enum class State { Requested, Received, Delivered };
    struct Item {
        Request request;
        State state = State::Requested;
        QByteArray data;
        Clock::time_point at;
    };

    bool moreToRequest() const { return m_fileIndex < m_files.size(); }

    quint32 nextLength() const
    {
        const quint64 remaining = m_files.at(m_fileIndex).size - m_offset;
        return quint32(std::min<quint64>(remaining, requestBytes()));
    }

    bool issue(quint32 length, Clock::time_point now)
    {
        const auto &file = m_files.at(m_fileIndex);
        Request request;
        request.streamId = ++m_streamId;
        if (request.streamId == 0) request.streamId = ++m_streamId;
        request.index = file.index;
        request.offset = m_offset;
        request.length = length;
        request.clipDataId = m_options.clipDataId;
        Item item;
        item.request = request;
        item.at = now;
        m_items.push_back(std::move(item));
        ++m_counters.requests;
        m_counters.bytesRequested += length;
        m_counters.peakInflight = std::max(m_counters.peakInflight, inflight());
        m_pacer.consume(now, length);
        m_offset += length;
        if (m_offset >= file.size) {
            ++m_fileIndex;
            m_offset = 0;
        }
        if (!m_callbacks.sendRequest || !m_callbacks.sendRequest(request)) {
            end(EndReason::PeerUnreachable, AckStatus::Ok);
            return false;
        }
        return true;
    }

    void deliverReady()
    {
        // deliver() can re-enter this object (a worker that acknowledges at once reaches onAck(), which erases items or ends the transfer),
        // so no iterator or reference into m_items is kept across it: the next item to hand over is found again every time.
        while (!m_ended) {
            const auto it = std::find_if(m_items.begin(), m_items.end(), [](const Item &item) { return item.state != State::Delivered; });
            if (it == m_items.end() || it->state == State::Requested) return; // nothing more, or in order: an earlier request is still out
            const Chunk chunk{it->request.index, it->request.offset, it->data};
            it->data.clear();
            it->state = State::Delivered;
            it->at = m_now;
            m_counters.bytesDelivered += quint64(chunk.data.size());
            const bool delivered = m_callbacks.deliver && m_callbacks.deliver(chunk);
            if (m_ended) return; // it was ended meanwhile (and has said so)
            if (!delivered) {
                end(EndReason::WorkerUnreachable, AckStatus::Ok);
                return;
            }
        }
    }

    void violation()
    {
        ++m_counters.violations;
        if (m_counters.violations >= ClipboardFileLimits::MaxViolations) {
            end(EndReason::Violations, AckStatus::Ok);
        }
    }

    void checkDone()
    {
        if (!moreToRequest() && m_items.empty()) {
            end(EndReason::Done, AckStatus::Ok);
        }
    }

    void end(EndReason reason, AckStatus cause)
    {
        if (m_ended) return;
        m_ended = true;
        m_items.clear();
        if (m_callbacks.ended) m_callbacks.ended(reason, cause);
    }

    Callbacks m_callbacks;
    Options m_options;
    ClipboardFilePacing::FileTransferPacer m_pacer;
    QList<ClipboardFilePush::FetchFile> m_files;
    qsizetype m_fileIndex = 0;
    quint64 m_offset = 0;
    quint32 m_streamId = 0;
    std::deque<Item> m_items;
    Counters m_counters;
    Clock::time_point m_now{};
    bool m_began = false;
    bool m_ended = false;
    bool m_pumping = false;
};
}
