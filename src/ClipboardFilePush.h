// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QByteArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "ClipboardFileDescriptor.h"
#include "ClipboardFileLimits.h"
#include "ClipboardFileNames.h"

namespace KRdp::ClipboardFilePush
{
/**
 * OPT-078 phase 3 (client to host, spec 3.2 and 4): turns the FileGroupDescriptorW a client sent into the plan the host follows,
 * pure. The same function runs in the broker (to know which entries to ask the client for) and in the worker (to create the
 * staging tree), so the two can never disagree about which index is fetched: the worker also reports its totals back and the
 * broker compares them with its own.
 *
 * The descriptor is hostile input. Every name goes through ClipboardFileNames::sanitize() in windowsSafe mode (so a Windows-origin set
 * round-trips: reserved device names, trailing dots and spaces and `~1` aliases are renamed), and one bad name refuses the whole
 * transfer. Entries the host will not create are SKIPPED and counted, never silently truncated:
 *   - a reparse point (a Windows symlink or junction), and everything below a skipped directory;
 *   - a file whose size the descriptor does not state (FD_FILESIZE unset: the size is needed before the first byte for the free-space guard);
 *   - a file over 4 GiB - 1 when the peer did not negotiate huge-file support (spec 5, spike S1).
 * Directories carry no content. Nothing here puts a name into an error or a log line.
 */
enum class Refusal : quint8 {
    None,
    Descriptor, ///< the FileGroupDescriptorW is malformed or over the limits
    Names, ///< a name is hostile (traversal, absolute, device, control characters ...)
    NothingToCopy, ///< every entry was skipped, or the list is empty
};
constexpr quint8 LastRefusal = quint8(Refusal::NothingToCopy);

inline const char *refusalName(Refusal refusal)
{
    switch (refusal) {
    case Refusal::None: return "none";
    case Refusal::Descriptor: return "descriptor";
    case Refusal::Names: return "names";
    case Refusal::NothingToCopy: return "nothing-to-copy";
    }
    return "unknown";
}

enum class Skip : quint8 { None, Reparse, UnknownSize, NeedsHuge, BelowSkipped };

struct Entry {
    quint32 index = 0; ///< position in the descriptor (the listIndex of FILECONTENTS requests)
    QString relativePath; ///< sanitized, '/' separators, never empty
    QStringList components;
    bool isDirectory = false;
    quint64 size = 0; ///< a directory: 0
    std::optional<qint64> mtimeSeconds; ///< Unix seconds, when the descriptor stated a last-write time
    Skip skip = Skip::None;
    int topLevel = 0; ///< the unit this entry belongs to: all entries sharing the first path component
    bool wanted() const { return skip == Skip::None; }
    bool isFile() const { return !isDirectory; }
    /// Bytes have to be requested from the client.
    bool needsData() const { return wanted() && !isDirectory && size > 0; }
};

struct Plan {
    Refusal refusal = Refusal::None;
    std::optional<ClipboardFileNames::Reject> nameReject;
    std::optional<ClipboardFileDescriptor::Error> descriptorError;
    QList<Entry> entries; ///< descriptor order, sanitized
    quint64 totalBytes = 0; ///< bytes to fetch (saturating)
    int files = 0; ///< wanted files (including empty ones)
    int directories = 0; ///< wanted directories
    int dataFiles = 0; ///< wanted files with size > 0
    int skippedReparse = 0;
    int skippedUnknownSize = 0;
    int skippedHuge = 0;
    int skippedTotal() const { return skippedReparse + skippedUnknownSize + skippedHuge + skippedBelow; }
    int skippedBelow = 0;
    int topLevelCount = 0;
    bool renamed = false; ///< any name was changed by the sanitizer
    bool ok() const { return refusal == Refusal::None; }
};

inline Plan refused(Refusal refusal)
{
    Plan plan;
    plan.refusal = refusal;
    return plan;
}

/// Builds the plan from an already decoded descriptor list. \a huge: CB_HUGE_FILE_SUPPORT_ENABLED was negotiated.
inline Plan makePlan(const QList<ClipboardFileDescriptor::Descriptor> &items, bool huge)
{
    using ClipboardFileNames::RawEntry;
    if (items.isEmpty()) {
        return refused(Refusal::NothingToCopy);
    }
    QList<RawEntry> raw;
    raw.reserve(items.size());
    for (const auto &item : items) {
        raw.append({item.name, item.isDirectory()});
    }
    ClipboardFileNames::Options options;
    options.windowsSafe = true;
    const auto sanitized = ClipboardFileNames::sanitize(raw, options);
    if (!sanitized.ok()) {
        Plan plan = refused(Refusal::Names);
        plan.nameReject = sanitized.reject;
        return plan;
    }
    Plan plan;
    plan.entries.reserve(items.size());
    QHash<QString, int> topLevelOf;
    QSet<QString> skippedDirectories;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const auto &item = items.at(i);
        const auto &clean = sanitized.entries.at(i);
        Entry entry;
        entry.index = quint32(i);
        entry.relativePath = clean.relativePath;
        entry.components = clean.components;
        entry.isDirectory = clean.isDirectory;
        plan.renamed = plan.renamed || clean.renamed;
        entry.size = entry.isDirectory ? 0 : item.size.value_or(0);
        if (item.writeTime && *item.writeTime != 0) {
            entry.mtimeSeconds = ClipboardFileDescriptor::unixSecondsFromFileTime(*item.writeTime);
        }
        const QString &top = entry.components.first();
        if (!topLevelOf.contains(top)) {
            topLevelOf.insert(top, plan.topLevelCount++);
        }
        entry.topLevel = topLevelOf.value(top);
        // Everything below a directory that was skipped is skipped with it.
        bool below = false;
        for (int depth = 1; depth < entry.components.size() && !below; ++depth) {
            below = skippedDirectories.contains(entry.components.mid(0, depth).join(QLatin1Char('/')));
        }
        if (below) {
            entry.skip = Skip::BelowSkipped;
            ++plan.skippedBelow;
        } else if (item.isReparsePoint()) {
            entry.skip = Skip::Reparse;
            ++plan.skippedReparse;
        } else if (!entry.isDirectory && !item.size) {
            entry.skip = Skip::UnknownSize;
            ++plan.skippedUnknownSize;
        } else if (!entry.isDirectory && !huge && !ClipboardFileLimits::fitsWithoutHugeSupport(entry.size)) {
            entry.skip = Skip::NeedsHuge;
            ++plan.skippedHuge;
        }
        if (entry.skip != Skip::None && entry.isDirectory) {
            skippedDirectories.insert(entry.relativePath);
        }
        if (entry.wanted()) {
            if (entry.isDirectory) {
                ++plan.directories;
            } else {
                ++plan.files;
                if (entry.size > 0) ++plan.dataFiles;
                plan.totalBytes = entry.size > ~quint64(0) - plan.totalBytes ? ~quint64(0) : plan.totalBytes + entry.size;
            }
        }
        plan.entries.append(entry);
    }
    if (plan.files == 0 && plan.directories == 0) {
        Plan none = refused(Refusal::NothingToCopy);
        none.skippedReparse = plan.skippedReparse;
        none.skippedUnknownSize = plan.skippedUnknownSize;
        none.skippedHuge = plan.skippedHuge;
        none.skippedBelow = plan.skippedBelow;
        return none;
    }
    return plan;
}

/// Decodes (hostile input: counts, lengths and strings are checked) and plans.
inline Plan makePlan(QByteArrayView descriptor, bool huge)
{
    const auto decoded = ClipboardFileDescriptor::decode(descriptor);
    if (!decoded.ok()) {
        Plan plan = refused(Refusal::Descriptor);
        plan.descriptorError = decoded.error;
        return plan;
    }
    return makePlan(decoded.items, huge);
}

/// One range source the broker asks the client for: the wanted files with data, in plan order.
struct FetchFile {
    quint32 index = 0;
    quint64 size = 0;
};
inline QList<FetchFile> fetchList(const Plan &plan)
{
    QList<FetchFile> list;
    for (const auto &entry : plan.entries) {
        if (entry.needsData()) list.append({entry.index, entry.size});
    }
    return list;
}

/**
 * The staged selection as the host clipboard sees it: the top-level units (the first path component), each finished when every wanted
 * entry below it has all its bytes. \a completeFiles: descriptor indexes whose file is complete (renamed to its final name).
 * Returns the sanitized first components of the finished units, in plan order (spec 3.2 offer rule; the client's host-to-client side does the same).
 */
inline QStringList finishedTopLevel(const Plan &plan, const QSet<quint32> &completeFiles)
{
    QList<bool> finished(plan.topLevelCount, true);
    QList<bool> anyWanted(plan.topLevelCount, false);
    QStringList names(plan.topLevelCount);
    for (const auto &entry : plan.entries) {
        names[entry.topLevel] = entry.components.first();
        if (!entry.wanted()) continue;
        anyWanted[entry.topLevel] = true;
        if (entry.isFile() && !completeFiles.contains(entry.index)) {
            finished[entry.topLevel] = false;
        }
    }
    QStringList out;
    for (int i = 0; i < plan.topLevelCount; ++i) {
        // A unit whose every entry was skipped has nothing to offer.
        if (finished[i] && anyWanted[i]) out.append(names[i]);
    }
    return out;
}
}
