// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <optional>

#include <QtGlobal>

namespace KRdp::ClipboardFileLimits
{
/**
 * OPT-078 (clipboard file transfer, spec section 4.3 / 6.3): the constants of the pure file-transfer core.
 *
 * The entry/depth/path limits are safety valves against memory exhaustion from a hostile descriptor
 * list. They are NOT a size limit: a file of any size is accepted (the free-space guard below decides
 * whether it fits).
 */
/// Entries (files and directories) in one selection.
constexpr quint32 MaxEntries = 100'000;
/// Wire bytes of one FileGroupDescriptorW (4 + MaxEntries * 592 = 59.2 MB fits).
constexpr qint64 MaxDescriptorWireBytes = 64LL * 1024 * 1024;
/// Directory nesting (components in a relative path).
constexpr int MaxDepth = 64;
/// UTF-8 bytes of all relative paths of one selection together.
constexpr qint64 MaxTotalPathBytes = 32LL * 1024 * 1024;
/// UTF-8 bytes of one path component and of one whole relative path (the internal worker wire; the
/// FILEDESCRIPTORW itself carries at most 259 UTF-16 units, see ClipboardFileDescriptor.h).
constexpr int MaxComponentBytes = 255;
constexpr int MaxPathBytes = 4096;

/// One FILECONTENTS range: the validation ceiling and the size the pacer asks for.
constexpr quint32 MaxRangeBytes = 1024 * 1024;
constexpr quint32 RangeBytes = 256 * 1024;
/// Requests in flight: the pacer's window cap, and the cap for a client that did not negotiate KRDPCTL.
constexpr int MaxWindow = 16;
constexpr int StockClientWindow = 4;
/// Requests accepted in flight at most (extra ones are refused, not queued).
constexpr int MaxOutstanding = MaxWindow;
/// Protocol violations (invalid requests) that abort one transfer.
constexpr int MaxViolations = 3;

/**
 * OPT-078 small files (decided 2026-11-08): the in-flight limit is a BYTE budget with a count cap, not "16 requests".
 *
 * A request for a small file is tiny, so many of them fit where 16 requests of 256 KiB did. A request is admitted while
 *     (count in flight < maxCount)  AND  (bytes requested in flight + its length <= maxBytes)
 * and one request is always admitted when none is in flight (a range is at most MaxRangeBytes). The window only bounds CONCURRENCY and memory;
 * the pacer (ClipboardFilePacing: token bucket, floor, Cap/Balanced, the stock-client rule) still bounds the RATE, and it derives the
 * bytes it allows in flight from its rate (about 100 ms of data), so a bigger count cannot raise a rate. The VALIDATOR (windowAdmits) applies the rule
 * above strictly; the REQUESTER (requesterAdmits) sends while the bytes in flight are still below the pacer's budget, so the last request may overshoot
 * it (the old "ceil(rate x 100 ms / size)" behaviour that an Auto transfer's ramp depends on), and the strict limits remain the hard memory bound.
 *
 * Three windows:
 *   - LegacyWindow: what a peer that did not negotiate gets, today's rule: 16 requests of any valid length (16 x MaxRangeBytes = no byte rule beyond the
 *     count). A requester asking a peer that did not advertise a window uses it too (its requests are at most RangeBytes, so 4 MiB in flight).
 *   - NegotiatedWindow: both sides are our own and the peer advertised its window (KRDPCTL capabilities.clipboard.files.window, or the same
 *     `window` object in the client's `clipboard-transfer`): 4 MiB (= the old 16 x 256 KiB, so large files behave exactly as before) and
 *     at most 128 requests. Memory held for requested ranges never exceeds maxBytes (a buffer is never sized from a claimed length: ranges are
 *     validated against MaxRangeBytes and the file size first).
 *   - a requester uses min(own, peer-advertised); an absent or invalid advertisement means LegacyWindow.
 */
struct Window {
    qint64 maxBytes = 0;
    int maxCount = 0;
    bool operator==(const Window &) const = default;
};
constexpr Window LegacyWindow{qint64(MaxWindow) * qint64(MaxRangeBytes), MaxWindow};
constexpr int NegotiatedMaxCount = 128;
constexpr qint64 NegotiatedMaxBytes = qint64(MaxWindow) * qint64(RangeBytes); ///< 4 MiB
constexpr Window NegotiatedWindow{NegotiatedMaxBytes, NegotiatedMaxCount};
/// What a peer may advertise at all (an advertisement outside this is ignored: the window stays LegacyWindow).
constexpr bool validAdvertisedWindow(qint64 maxBytes, qint64 maxCount)
{
    return maxBytes >= qint64(RangeBytes) && maxCount >= 1;
}
/// The window to use towards a peer that advertised \a peer (nullopt = it did not, or the advertisement was invalid): min(own, peer) or LegacyWindow.
inline Window negotiatedWindow(std::optional<Window> peer, const Window &own = NegotiatedWindow)
{
    if (!peer || !validAdvertisedWindow(peer->maxBytes, peer->maxCount)) {
        return LegacyWindow;
    }
    return {std::min(own.maxBytes, peer->maxBytes), std::min(own.maxCount, peer->maxCount)};
}
/// The window a host SERVES a peer under (its validator and its read window): our own client may keep NegotiatedWindow requests in flight (it only learns the
/// size from the host's `capabilities`, which it can answer only when it is ours; a client from before this change never asks for more than 16 anyway), a
/// stock client the old 16.
inline Window windowForServing(bool stockClient)
{
    return stockClient ? LegacyWindow : NegotiatedWindow;
}
/// The window a program REQUESTS under from a peer that advertised \a advertised (nullopt: it said nothing, e.g. an older program): min(own, advertised), or
/// LegacyWindow; a stock peer always LegacyWindow.
inline Window windowForRequesting(bool stockPeer, std::optional<Window> advertised, const Window &own = NegotiatedWindow)
{
    return stockPeer ? LegacyWindow : negotiatedWindow(advertised, own);
}
/// The REQUESTER's rule (OPT-078 small files): \a hard are the negotiated limits (count and the strict byte sum, a hard memory bound), \a effective is what the pacer
/// derives from its rate (FileTransferPacer::window(hard): maxCount a cap, maxBytes the byte BUDGET). A request goes out while the bytes in flight are still BELOW the budget
/// (so the last one may overshoot it, exactly as the old "ceil(rate x 100 ms / request size)" count did: that overshoot is what lets an Auto transfer ramp up, because the
/// pacer measures what arrives and a window that held no more than the rate x RTT would never find a higher rate) and the strict limits still hold.
constexpr bool requesterAdmits(const Window &hard, const Window &effective, int inflightCount, qint64 inflightBytes, quint32 length)
{
    return inflightCount <= 0 || (inflightCount < effective.maxCount && inflightBytes < effective.maxBytes && inflightBytes + qint64(length) <= hard.maxBytes);
}
/// The VALIDATOR's rule: whether one more request of \a length bytes may be accepted with \a inflightCount requests and \a inflightBytes bytes already in flight.
constexpr bool windowAdmits(const Window &window, int inflightCount, qint64 inflightBytes, quint32 length)
{
    return inflightCount <= 0 || (inflightCount < window.maxCount && inflightBytes + qint64(length) <= window.maxBytes);
}
/// Simultaneous clipboard locks per connection.
constexpr int MaxLocks = 8;

/// Walk time budget for one selection (milliseconds).
constexpr int WalkBudgetMs = 5000;

/// Space checks: before the first byte and every this many staged bytes.
constexpr quint64 SpaceCheckIntervalBytes = 64ULL * 1024 * 1024;
/// ClipboardFileMinFreeBytes default: 5% of the filesystem, but never below 2 GiB and never above 8 GiB (a fixed percentage alone asked for 183 GiB of
/// free space on a 3.6 TiB disk and refused every copy; what must stay free is a fixed safety margin, not a share of the disk).
constexpr quint64 DefaultReserveFloorBytes = 2ULL * 1024 * 1024 * 1024;
constexpr quint64 DefaultReserveCeilingBytes = 8ULL * 1024 * 1024 * 1024;
constexpr quint64 DefaultReservePercent = 5;

/// The staged selection becomes visible when it is at most this big, else after its first file.
constexpr quint64 OfferWhenCompleteBytes = 256ULL * 1024 * 1024;

/// The free space that must remain after a transfer: \a configured when non-zero (used as given, even above the ceiling), else 5% of \a filesystemBytes
/// clamped to [2 GiB, 8 GiB].
constexpr quint64 reserveBytes(quint64 configured, quint64 filesystemBytes)
{
    if (configured != 0) {
        return configured;
    }
    const quint64 percent = filesystemBytes / 100 * DefaultReservePercent + filesystemBytes % 100 * DefaultReservePercent / 100;
    return std::clamp(percent, DefaultReserveFloorBytes, DefaultReserveCeilingBytes);
}

enum class SpaceVerdict {
    Ok,
    Unknown, ///< statvfs failed (or reported nothing usable): the caller refuses, never guesses
    NotEnough, ///< the declared total plus the reserve exceeds the free space
};

struct SpaceDecision {
    SpaceVerdict verdict = SpaceVerdict::Unknown;
    quint64 neededBytes = 0; ///< the declared remainder plus the reserve (saturating)
    quint64 freeBytes = 0;
    quint64 reserve = 0;
    bool operator==(const SpaceDecision &) const = default;
};

/**
 * The free-space guard (spec 3.2). \a freeBytes / \a filesystemBytes come from an injected statvfs
 * (f_bavail * f_frsize, f_blocks * f_frsize; nullopt = the call failed). \a remainingBytes is what
 * still has to be written: the declared total before the first byte, the unwritten rest at every
 * later check (mid-transfer a failure means abort, delete the partial, report). All arithmetic
 * saturates; a size near 2^64 is just "not enough", never an overflow.
 */
inline SpaceDecision checkFreeSpace(std::optional<quint64> freeBytes, std::optional<quint64> filesystemBytes, quint64 remainingBytes, quint64 configuredReserve = 0)
{
    SpaceDecision decision;
    if (!freeBytes || !filesystemBytes || *filesystemBytes == 0) {
        return decision;
    }
    decision.freeBytes = *freeBytes;
    decision.reserve = reserveBytes(configuredReserve, *filesystemBytes);
    decision.neededBytes = remainingBytes > ~quint64(0) - decision.reserve ? ~quint64(0) : remainingBytes + decision.reserve;
    decision.verdict = decision.neededBytes <= decision.freeBytes ? SpaceVerdict::Ok : SpaceVerdict::NotEnough;
    return decision;
}

/// Without the negotiated huge-file flag a peer can only name sizes and offsets up to 2^32 - 1: a bigger file is skipped with a notice, never truncated (spec 5, spike S1).
constexpr bool fitsWithoutHugeSupport(quint64 sizeBytes)
{
    return sizeBytes <= 0xffffffffULL;
}

/// Whether a periodic check is due: \a stagedSinceCheck bytes were written since the last one.
constexpr bool spaceCheckDue(quint64 stagedSinceCheck, bool first)
{
    return first || stagedSinceCheck >= SpaceCheckIntervalBytes;
}
}
