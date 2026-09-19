// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <cmath>

#include <QList>
#include <QPointF>
#include <QRect>
#include <QStringList>

namespace KRdp
{
/**
 * The pure half of a live `KRDPCTL` apply on one connection (OPT-044,
 * Task 4): given the KWin outputs the connection's sessions capture now and
 * the outputs the new layout wants it to capture, which sessions keep
 * streaming, which are dropped and which are created. A session is keyed by
 * the output NAME it captures: a real monitor's connector while it is lit,
 * its stand-in's name while it is dark or stood in, a virtual monitor's
 * output name - so every change of state a monitor can go through changes
 * the name under it, and a session over the same name is the same stream.
 * No KWin, no processes; unit-tested from autotests/LayoutSessionDiffTest.cpp.
 */
namespace LayoutSessions
{
struct Diff {
    /**
     * Parallel to the wanted list: the index into the current list of the
     * session that keeps streaming that output, or -1 for one to create.
     */
    QList<qsizetype> source;
    /** Output names, for the log: kept in wanted order, created likewise, dropped in current order. */
    QStringList kept;
    QStringList created;
    QStringList dropped;

    /** Every wanted output already has its session and no session is left over. */
    bool unchanged() const
    {
        return created.isEmpty() && dropped.isEmpty();
    }
};

/**
 * Diff \a current (the outputs the connection's sessions capture, in RDP
 * surface order) against \a wanted (the outputs the new layout needs, in
 * its order). \a changed names outputs the executor removed or created by
 * this apply: a session over one of those is dropped and recreated even
 * when the name is wanted again (belt: the executor never reuses a name
 * within one apply, since size and scale are part of it, but the ruling is
 * that its word on what changed wins over a name match).
 */
inline Diff diff(const QStringList &current, const QStringList &wanted, const QStringList &changed = {})
{
    Diff out;
    out.source.reserve(wanted.size());
    QList<bool> taken(current.size(), false);
    for (const auto &name : wanted) {
        qsizetype from = -1;
        if (!changed.contains(name)) {
            for (qsizetype i = 0; i < current.size(); ++i) {
                if (!taken.at(i) && current.at(i) == name) {
                    from = i;
                    break;
                }
            }
        }
        if (from >= 0) {
            taken[from] = true;
            out.kept.push_back(name);
        } else {
            out.created.push_back(name);
        }
        out.source.push_back(from);
    }
    for (qsizetype i = 0; i < current.size(); ++i) {
        if (!taken.at(i)) {
            out.dropped.push_back(current.at(i));
        }
    }
    return out;
}

/**
 * A position in the capture PIXELS of the session over \a entry (an RDP
 * pointer position, or the screencast's cursor metadata) → KWin-global
 * logical coordinates, the space fake input works in. \a entry is the
 * wrapper's layout entry for that session: KWin logical origin, pixel size;
 * \a scale its pixels per logical unit. The same arithmetic as
 * KRdp::AbstractSession::mapToGlobal() - normalised over (pixels-1), spanned
 * over (logical-1), offset by the origin - so a sample mapped here compares
 * with a move injected through the same table. Used instead of the
 * session's own mapToGlobal() for a layout session, whose output stream reads
 * its place once at setup and never follows a move; the wrapper's table is
 * refreshed by every build.
 */
inline QPointF captureToGlobal(const QRect &entry, qreal scale, const QPointF &local)
{
    const QPoint origin = entry.topLeft();
    const QSize pixels = entry.size();
    const qreal ratio = scale > 0.0 ? scale : 1.0;
    const QSize logical(int(std::ceil(pixels.width() / ratio)), int(std::ceil(pixels.height() / ratio)));
    if (pixels.isEmpty() || logical.isEmpty()) {
        return local + origin;
    }
    const int inputWidth = std::max(1, pixels.width() - 1);
    const int inputHeight = std::max(1, pixels.height() - 1);
    const int logicalWidth = std::max(1, logical.width() - 1);
    const int logicalHeight = std::max(1, logical.height() - 1);
    const qreal normalizedX = std::clamp(local.x() / qreal(inputWidth), 0.0, 1.0);
    const qreal normalizedY = std::clamp(local.y() / qreal(inputHeight), 0.0, 1.0);
    return QPointF{normalizedX * logicalWidth + origin.x(), normalizedY * logicalHeight + origin.y()};
}
}
}
