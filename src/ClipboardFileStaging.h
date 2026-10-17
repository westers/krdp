// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QtGlobal>

#include "ClipboardFileLimits.h"

namespace KRdp::ClipboardFileStaging
{
/**
 * OPT-078 (spec 3.2): the PLANNING half of staging, pure. The file-system half (openat/O_EXCL/unlinkat
 * relative to a dirfd, the janitor that executes a SweepPlan, statvfs) arrives with the phase 3
 * transport and its link-wrap failure tests; this decides what to do, from a listing the caller read.
 *
 * Layout: `<cache>/krdp-clip/<connection-id>/<seq>/<relative path>`; a file is written as `name.part` and
 * renamed to `name` when complete, so a reader never sees a half file as final.
 */
constexpr qint64 MaxAgeSeconds = 24 * 60 * 60;
inline const QString PartSuffix = QStringLiteral(".part");

/// A connection directory name: `c-` and 1-32 lowercase hex digits (what staging creates; anything else is not ours).
inline bool isConnectionDirName(const QString &name)
{
    if (!name.startsWith(QLatin1String("c-")) || name.size() < 3 || name.size() > 34) {
        return false;
    }
    for (int i = 2; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
            return false;
        }
    }
    return true;
}

inline QString partNameFor(const QString &finalName)
{
    return finalName + PartSuffix;
}
inline bool isPartName(const QString &name)
{
    return name.size() > PartSuffix.size() && name.endsWith(PartSuffix);
}

/// One entry of the staging root as listed (lstat, never following links).
struct RootEntry {
    QString name;
    qint64 mtimeSeconds = 0;
    bool isDirectory = false;
    bool isSymlink = false;
};

enum class SweepAction {
    Keep, ///< a live connection's directory, or young enough
    RemoveTree, ///< stale: delete below the root with unlinkat (never following links)
    UnlinkOnly, ///< a symlink (or other non-directory) in the root: remove the entry itself, never what it points to
    LeaveAlone, ///< not ours (name does not match what staging creates)
};
enum class SweepReason { Live, Young, UnknownConnection, TooOld, NotOurs, Symlink, NotADirectory };

struct SweepItem {
    QString name;
    SweepAction action = SweepAction::Keep;
    SweepReason reason = SweepReason::Live;
    bool operator==(const SweepItem &) const = default;
};

/**
 * What the worker's start-up sweep does with \a entries: (a) removes directories whose connection id is not live
 * (the connection ended or the worker crashed), (b) removes anything older than 24 h even when its connection id is
 * live (a connection that outlived a day), (c) unlinks symlinks without following them, (d) leaves names it did not create.
 * A directory of a live connection that is young enough stays.
 */
inline QList<SweepItem> planSweep(const QList<RootEntry> &entries, const QSet<QString> &liveConnectionIds, qint64 nowSeconds, qint64 maxAgeSeconds = MaxAgeSeconds)
{
    QList<SweepItem> plan;
    plan.reserve(entries.size());
    for (const RootEntry &entry : entries) {
        SweepItem item;
        item.name = entry.name;
        if (entry.isSymlink) {
            // Checked first: a link must never be followed. One named like ours is unlinked; any other name is not ours to touch.
            item = isConnectionDirName(entry.name) ? SweepItem{entry.name, SweepAction::UnlinkOnly, SweepReason::Symlink} : SweepItem{entry.name, SweepAction::LeaveAlone, SweepReason::NotOurs};
        } else if (!entry.isDirectory) {
            item = {entry.name, SweepAction::UnlinkOnly, SweepReason::NotADirectory};
            if (!isConnectionDirName(entry.name)) item = {entry.name, SweepAction::LeaveAlone, SweepReason::NotOurs};
        } else if (!isConnectionDirName(entry.name)) {
            item = {entry.name, SweepAction::LeaveAlone, SweepReason::NotOurs};
        } else if (nowSeconds - entry.mtimeSeconds > maxAgeSeconds) {
            item = {entry.name, SweepAction::RemoveTree, SweepReason::TooOld};
        } else if (!liveConnectionIds.contains(entry.name)) {
            item = {entry.name, SweepAction::RemoveTree, SweepReason::UnknownConnection};
        } else {
            item = {entry.name, SweepAction::Keep, SweepReason::Live};
        }
        plan.append(item);
    }
    return plan;
}

enum class OfferTiming {
    Now, ///< hand the selection to the host clipboard right away
    AfterFirstFile, ///< a large selection: after the first file is complete, with a progress notice
    Never, ///< nothing to offer
};

/**
 * When the staged selection becomes visible as `text/uri-list` (spec 3.2): small selections (total at most 256 MiB, saturating
 * arithmetic) when everything is complete; larger ones after the first file. \a sizes are the files' declared sizes.
 */
inline OfferTiming offerTiming(const QList<quint64> &fileSizes)
{
    if (fileSizes.isEmpty()) {
        return OfferTiming::Never;
    }
    quint64 total = 0;
    for (const quint64 size : fileSizes) {
        total = size > ~quint64(0) - total ? ~quint64(0) : total + size;
    }
    return total <= ClipboardFileLimits::OfferWhenCompleteBytes ? OfferTiming::Now : OfferTiming::AfterFirstFile;
}
}
