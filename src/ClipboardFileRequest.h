// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QSet>
#include <QtGlobal>

#include "ClipboardFileLimits.h"

namespace KRdp::ClipboardFileRequest
{
/**
 * OPT-078 (spec 4.3, 4.4): validation of the peer's FILECONTENTS requests and the clipboard-lock
 * bookkeeping, pure. The sender of file bytes runs one RequestValidator per connection and offer
 * direction; every request from the peer goes through validate() before a single byte is read.
 *
 * Rules (spec 4.3): listIndex < count; flags exactly FILECONTENTS_SIZE (0x1) or FILECONTENTS_RANGE
 * (0x2); SIZE carries cbRequested == 8; RANGE carries 1..MaxRangeBytes, offset + length is checked
 * in 64 bits for overflow and must not pass the file size (a range on a directory is invalid); the
 * streamId is unique among the outstanding requests; clipDataId names a locked snapshot when locking
 * is negotiated (a request without one reads the current offer), and is a violation when it is not.
 * Without the negotiated huge-file flag (CB_HUGE_FILE_SUPPORT_ENABLED) nothing at or beyond 4 GiB may be asked
 * for (offset + length must stay within 2^32 - 1): spike S1 (2026-10-17) showed that a stock libfreerdp client
 * refuses to send such a request itself, but a server must not depend on that, and a file of that size is not
 * offered to such a peer at all. Invalid requests are answered with a failed response and counted; the third violation aborts the
 * transfer. At most MaxOutstanding requests are in flight: extra ones are refused (not queued, not a
 * violation).
 */
constexpr quint32 FlagSize = 0x1;
constexpr quint32 FlagRange = 0x2;

struct Request {
    quint32 streamId = 0;
    quint32 listIndex = 0;
    quint32 flags = 0;
    quint64 offset = 0; ///< nPositionHigh:Low
    quint32 length = 0; ///< cbRequested
    std::optional<quint32> clipDataId;
};

/// What a sender serves: the entries of one offer (descriptor index order).
struct Snapshot {
    struct Item {
        quint64 size = 0;
        bool isDirectory = false;
    };
    QList<Item> items;
};

enum class Verdict {
    Accept, ///< answer it
    Fail, ///< answer with a failed response (counted as a violation)
    Refuse, ///< over the outstanding limit: failed response, not a violation
    Abort, ///< the third violation: cancel the transfer
};

enum class Reason {
    None,
    IndexOutOfRange,
    BadFlags,
    BadSizeLength,
    BadRangeLength,
    RangeOverflow,
    HugeNotNegotiated,
    RangePastEnd,
    RangeOnDirectory,
    DuplicateStream,
    UnknownLock,
    LockNotNegotiated,
    NoOffer,
    TooManyOutstanding,
    Aborted, ///< already aborted
};

inline const char *reasonName(Reason reason)
{
    switch (reason) {
    case Reason::None: return "none";
    case Reason::IndexOutOfRange: return "index-out-of-range";
    case Reason::BadFlags: return "bad-flags";
    case Reason::BadSizeLength: return "bad-size-length";
    case Reason::BadRangeLength: return "bad-range-length";
    case Reason::RangeOverflow: return "range-overflow";
    case Reason::HugeNotNegotiated: return "huge-not-negotiated";
    case Reason::RangePastEnd: return "range-past-end";
    case Reason::RangeOnDirectory: return "range-on-directory";
    case Reason::DuplicateStream: return "duplicate-stream";
    case Reason::UnknownLock: return "unknown-lock";
    case Reason::LockNotNegotiated: return "lock-not-negotiated";
    case Reason::NoOffer: return "no-offer";
    case Reason::TooManyOutstanding: return "too-many-outstanding";
    case Reason::Aborted: return "aborted";
    }
    return "unknown";
}

struct Decision {
    Verdict verdict = Verdict::Fail;
    Reason reason = Reason::NoOffer;
    quint64 fileSize = 0; ///< of the requested entry (Accept)
    bool operator==(const Decision &) const = default;
};

/// What lock() did.
struct LockResult {
    bool ok = false; ///< false: locking not negotiated or the id is already locked
    std::optional<quint32> evicted; ///< the least recently used lock dropped to make room (surface a notice)
};

class RequestValidator
{
public:
    /// \a lockingNegotiated: CB_CAN_LOCK_CLIPDATA; \a hugeNegotiated: CB_HUGE_FILE_SUPPORT_ENABLED (both from the cliprdr context after the capability exchange: it holds the intersection).
    explicit RequestValidator(bool lockingNegotiated = false, int maxOutstanding = ClipboardFileLimits::MaxOutstanding, bool hugeNegotiated = false)
        : m_locking(lockingNegotiated)
        , m_huge(hugeNegotiated)
        , m_maxOutstanding(maxOutstanding)
    {
    }

    /// A new offer replaces the current one. Without locking this ends all outstanding requests (the simple model).
    void setCurrent(Snapshot snapshot)
    {
        m_current = std::move(snapshot);
        m_haveCurrent = true;
        if (!m_locking) {
            m_outstanding.clear();
        }
    }
    void clearCurrent()
    {
        m_haveCurrent = false;
        m_current = {};
        if (!m_locking) {
            m_outstanding.clear();
        }
    }

    /// The receiver locked \a clipDataId: \a snapshot (of the current offer) is kept until unlock(). At most MaxLocks, LRU dropped.
    LockResult lock(quint32 clipDataId, Snapshot snapshot)
    {
        LockResult result;
        if (!m_locking || m_locks.contains(clipDataId)) {
            return result;
        }
        if (m_order.size() >= ClipboardFileLimits::MaxLocks) {
            const quint32 victim = m_order.takeFirst();
            m_locks.remove(victim);
            result.evicted = victim;
        }
        m_locks.insert(clipDataId, std::move(snapshot));
        m_order.append(clipDataId);
        result.ok = true;
        return result;
    }
    bool unlock(quint32 clipDataId)
    {
        if (!m_locks.remove(clipDataId)) {
            return false;
        }
        m_order.removeOne(clipDataId);
        return true;
    }
    int lockCount() const
    {
        return int(m_locks.size());
    }
    bool isLocked(quint32 clipDataId) const
    {
        return m_locks.contains(clipDataId);
    }

    Decision validate(const Request &request)
    {
        if (m_aborted) {
            return {Verdict::Abort, Reason::Aborted, 0};
        }
        const Snapshot *snapshot = nullptr;
        if (request.clipDataId) {
            if (!m_locking) {
                return violation(Reason::LockNotNegotiated);
            }
            const auto it = m_locks.constFind(*request.clipDataId);
            if (it == m_locks.constEnd()) {
                return violation(Reason::UnknownLock);
            }
            snapshot = &*it;
            touch(*request.clipDataId);
        } else {
            if (!m_haveCurrent) {
                return violation(Reason::NoOffer);
            }
            snapshot = &m_current;
        }
        if (request.listIndex >= quint32(snapshot->items.size())) {
            return violation(Reason::IndexOutOfRange);
        }
        const Snapshot::Item &item = snapshot->items.at(qsizetype(request.listIndex));
        if (request.flags != FlagSize && request.flags != FlagRange) {
            return violation(Reason::BadFlags);
        }
        if (request.flags == FlagSize) {
            if (request.length != 8) {
                return violation(Reason::BadSizeLength);
            }
        } else {
            if (item.isDirectory) {
                return violation(Reason::RangeOnDirectory);
            }
            if (request.length == 0 || request.length > ClipboardFileLimits::MaxRangeBytes) {
                return violation(Reason::BadRangeLength);
            }
            if (request.offset > ~quint64(0) - request.length) {
                return violation(Reason::RangeOverflow);
            }
            if (!m_huge && request.offset + request.length > 0xffffffffULL) {
                return violation(Reason::HugeNotNegotiated);
            }
            if (request.offset + request.length > item.size) {
                return violation(Reason::RangePastEnd);
            }
        }
        if (m_outstanding.contains(request.streamId)) {
            return violation(Reason::DuplicateStream);
        }
        if (int(m_outstanding.size()) >= m_maxOutstanding) {
            ++m_refused;
            return {Verdict::Refuse, Reason::TooManyOutstanding, 0};
        }
        m_outstanding.insert(request.streamId);
        return {Verdict::Accept, Reason::None, item.size};
    }

    /// The response for \a streamId was sent (or it was cancelled).
    void complete(quint32 streamId)
    {
        m_outstanding.remove(streamId);
    }
    void cancelAll()
    {
        m_outstanding.clear();
    }
    int outstanding() const
    {
        return int(m_outstanding.size());
    }
    int violations() const
    {
        return m_violations;
    }
    /// A new transfer starts: the violations of the last one (and an abort) are forgotten. Locks and outstanding requests are kept.
    void clearViolations()
    {
        m_violations = 0;
        m_aborted = false;
    }
    int refused() const
    {
        return m_refused;
    }
    bool aborted() const
    {
        return m_aborted;
    }

private:
    Decision violation(Reason reason)
    {
        ++m_violations;
        if (m_violations >= ClipboardFileLimits::MaxViolations) {
            m_aborted = true;
            return {Verdict::Abort, reason, 0};
        }
        return {Verdict::Fail, reason, 0};
    }
    void touch(quint32 id)
    {
        m_order.removeOne(id);
        m_order.append(id);
    }

    bool m_locking = false;
    bool m_huge = false;
    int m_maxOutstanding = ClipboardFileLimits::MaxOutstanding;
    Snapshot m_current;
    bool m_haveCurrent = false;
    QHash<quint32, Snapshot> m_locks;
    QList<quint32> m_order; ///< least recently used first
    QSet<quint32> m_outstanding;
    int m_violations = 0;
    int m_refused = 0;
    bool m_aborted = false;
};
}
