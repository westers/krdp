// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <SurfaceLayout.h>

namespace KRdp
{

/**
 * Choosing which monitors `MonitorMode=multi` streams, and at what geometry.
 *
 * Lives here rather than in SessionController so the decision is a pure
 * function of plain values: no QScreen, no QGuiApplication, no globals. That
 * is what makes it testable without a display (autotests/MultiLayoutTest.cpp);
 * SessionController::computeMultiLayout() does nothing but read
 * qGuiApp->screens() into ScreenInfo and call selectMultiLayout().
 */
namespace MultiLayout
{

/** The VA-API H.264 encoder on the Radeon 780M tops out at a 4096x4096 surface. */
constexpr int MaxEncodeDimension = 4096;
/**
 * RDPGFX_RESET_GRAPHICS carries at most 16 monitors, and
 * VideoStream::setMonitorLayout() rejects a longer layout outright.
 */
constexpr int MaxMonitorCount = 16;
/** Below this, per-monitor streaming buys nothing over the single-surface path. */
constexpr int MinMonitorCount = 2;

/**
 * One candidate monitor, as read off a QScreen.
 */
struct ScreenInfo {
    /** Connector name, e.g. "DP-1"; used for logging only. */
    QString name;
    /** QScreen::geometry(), i.e. KWin-global LOGICAL coordinates. */
    QRect logicalGeometry;
    /** QScreen::devicePixelRatio(): logical units to capture pixels. */
    qreal devicePixelRatio = 1.0;
    /** Whether this is the user's primary monitor. */
    bool primary = false;
};

/**
 * Pick the monitors `MonitorMode=multi` can stream, in KWin-global PIXEL
 * coordinates and in the order \a screens was given in.
 *
 * Left out, with the name appended to \a dropped when it is not null:
 * - a screen with an empty geometry (KWin reports that briefly while it
 *   re-adds its outputs after a DPMS wake, and VideoStream::setMonitorLayout()
 *   would reject the whole layout over it),
 * - a screen larger than MaxEncodeDimension in either direction once scaled to
 *   pixels, because no encoder can produce that surface,
 * - every screen past MaxMonitorCount.
 *
 * \a keptIndices, when not null, receives the index into \a screens behind each
 * returned entry, so the caller can map a surface back to its screen.
 *
 * The geometries are NOT translated: VideoStream::setMonitorLayout() owns the
 * translation into RDP desktop space and SurfaceLayout::originOf() inverts it.
 * Exactly one entry is always flagged primary - the first flagged one, or the
 * first entry when the configured primary was among the ones left out.
 *
 * Returns an empty layout when fewer than \a minimumCount monitors survive,
 * meaning "multi is not usable, fall back".
 */
inline QVector<VideoMonitor> selectMultiLayout(const QVector<ScreenInfo> &screens,
                                               QStringList *dropped = nullptr,
                                               QList<qsizetype> *keptIndices = nullptr,
                                               int minimumCount = MinMonitorCount)
{
    QVector<VideoMonitor> layout;
    QList<qsizetype> kept;

    for (qsizetype i = 0; i < screens.size(); ++i) {
        const auto &screen = screens.at(i);

        if (layout.size() >= MaxMonitorCount) {
            if (dropped) {
                dropped->append(screen.name);
            }
            continue;
        }

        if (screen.logicalGeometry.isEmpty()) {
            if (dropped) {
                dropped->append(screen.name);
            }
            continue;
        }

        const qreal scale = screen.devicePixelRatio > 0.0 ? screen.devicePixelRatio : 1.0;
        const QRect pixelGeometry(QPoint(qRound(screen.logicalGeometry.x() * scale), qRound(screen.logicalGeometry.y() * scale)),
                                  QSize(qRound(screen.logicalGeometry.width() * scale), qRound(screen.logicalGeometry.height() * scale)));

        if (pixelGeometry.width() > MaxEncodeDimension || pixelGeometry.height() > MaxEncodeDimension) {
            if (dropped) {
                dropped->append(screen.name);
            }
            continue;
        }

        kept.append(i);
        layout.push_back(VideoMonitor{
            .geometry = pixelGeometry,
            .primary = screen.primary,
        });
    }

    if (layout.size() < minimumCount) {
        return {};
    }

    // setMonitorLayout() wants exactly one primary: keep the first flagged one
    // and promote the first entry when the configured primary was left out.
    bool foundPrimary = false;
    for (auto &monitor : layout) {
        if (monitor.primary && !foundPrimary) {
            foundPrimary = true;
        } else {
            monitor.primary = false;
        }
    }
    if (!foundPrimary) {
        layout.first().primary = true;
    }

    if (keptIndices) {
        *keptIndices = kept;
    }
    return layout;
}

/**
 * What is left after one monitor of a layout is dropped.
 */
struct DropOutcome {
    /**
     * The old index of each survivor, in order. Its NEW surface index is its
     * position in this list, so entry i is "the session that was at
     * survivors[i] now feeds surface i".
     */
    QList<qsizetype> survivors;
    /**
     * The NEW index of the primary, or -1 when nothing survives.
     */
    qsizetype primary = -1;
};

/**
 * Re-index \a count monitors after the one at \a droppedIndex goes away.
 *
 * Surface indices have to stay 0..N-1 and line up with the layout handed to
 * VideoStream::setMonitorLayout(), so every monitor after the dropped one moves
 * down a place. \a primaryIndex is the old index of the primary, or -1 when
 * there is none; when it is the monitor being dropped (or there was none), the
 * first survivor is promoted, because setMonitorLayout() rejects a layout
 * without exactly one primary and rejecting would leave the stream on the old
 * N-surface layout while the re-indexed survivors sent to it.
 *
 * Pure, so autotests/MultiLayoutTest.cpp can state the rule without a session.
 */
inline DropOutcome dropMonitor(qsizetype count, qsizetype droppedIndex, qsizetype primaryIndex)
{
    DropOutcome outcome;
    if (count <= 0) {
        return outcome;
    }

    outcome.survivors.reserve(count - 1);
    for (qsizetype i = 0; i < count; ++i) {
        if (i == droppedIndex) {
            continue;
        }
        if (i == primaryIndex) {
            outcome.primary = outcome.survivors.size();
        }
        outcome.survivors.append(i);
    }

    if (outcome.primary < 0 && !outcome.survivors.isEmpty()) {
        outcome.primary = 0;
    }
    return outcome;
}

}
}
