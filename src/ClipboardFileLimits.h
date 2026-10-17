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
/// Simultaneous clipboard locks per connection.
constexpr int MaxLocks = 8;

/// Walk time budget for one selection (milliseconds).
constexpr int WalkBudgetMs = 5000;

/// Space checks: before the first byte and every this many staged bytes.
constexpr quint64 SpaceCheckIntervalBytes = 64ULL * 1024 * 1024;
/// ClipboardFileMinFreeBytes default: max(2 GiB, 5% of the filesystem).
constexpr quint64 DefaultReserveFloorBytes = 2ULL * 1024 * 1024 * 1024;
constexpr quint64 DefaultReservePercent = 5;

/// The staged selection becomes visible when it is at most this big, else after its first file.
constexpr quint64 OfferWhenCompleteBytes = 256ULL * 1024 * 1024;

/// The free space that must remain after a transfer: \a configured when non-zero, else max(2 GiB, 5% of \a filesystemBytes).
constexpr quint64 reserveBytes(quint64 configured, quint64 filesystemBytes)
{
    if (configured != 0) {
        return configured;
    }
    return std::max(DefaultReserveFloorBytes, filesystemBytes / 100 * DefaultReservePercent + filesystemBytes % 100 * DefaultReservePercent / 100);
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
