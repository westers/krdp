// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <RemoteMonitorGeometry.h>
#include <SurfaceLayout.h>

#include <algorithm>

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

/**
 * Per-monitor input mapping for a `multi` layout whose monitors are at
 * different scales (AUD-P8).
 *
 * selectMultiLayout() places each surface at its logical origin times its
 * OWN scale, so at mixed scales the pixel rects gap or overlap, and mapping a
 * pointer back with one (the primary's) scale lands on the wrong spot of every
 * other monitor. This re-packs the surfaces without overlap
 * (RemoteMonitorGeometry::projectToWire(), as a KRDPCTL layout does) and
 * keeps each entry's own scale and KWin logical origin, which is what
 * SessionController's input path (RemoteMonitorGeometry::wireToLogical())
 * maps through. Empty \a scales on the result means uniform scale: the
 * single-scale path applies unchanged.
 */
struct PerMonitorMapping {
    QVector<VideoMonitor> wire;
    QVector<qreal> scales;
    QVector<QPoint> logicalOrigins;
};

inline bool mixedScales(const QList<ScreenInfo> &screens, const QList<qsizetype> &kept)
{
    if (kept.isEmpty()) {
        return false;
    }
    const qreal first = screens.at(kept.first()).devicePixelRatio;
    return std::any_of(kept.cbegin(), kept.cend(), [&screens, first](qsizetype index) {
        return !qFuzzyCompare(screens.at(index).devicePixelRatio, first);
    });
}

inline PerMonitorMapping perMonitorMapping(const QList<ScreenInfo> &screens, const QList<qsizetype> &kept, const QVector<VideoMonitor> &monitors)
{
    PerMonitorMapping result;
    result.wire = monitors;
    if (kept.size() != monitors.size() || !mixedScales(screens, kept)) {
        return result;
    }
    QVector<RemoteMonitorGeometry::Output> outputs;
    outputs.reserve(kept.size());
    for (qsizetype i = 0; i < kept.size(); ++i) {
        const auto &screen = screens.at(kept.at(i));
        const qreal scale = screen.devicePixelRatio > 0.0 ? screen.devicePixelRatio : 1.0;
        outputs.push_back({screen.logicalGeometry.topLeft(), monitors.at(i).geometry.size(), scale, monitors.at(i).primary});
        result.scales.push_back(scale);
        result.logicalOrigins.push_back(screen.logicalGeometry.topLeft());
    }
    result.wire = RemoteMonitorGeometry::projectToWire(outputs);
    return result;
}

/** A wire-atlas pixel to KWin-global logical, through \a mapping (uniform \a scale when it has none). */
inline QPointF wireToLogical(const PerMonitorMapping &mapping, qreal scale, const QPointF &pixel)
{
    if (mapping.scales.size() != mapping.wire.size() || mapping.wire.isEmpty()) {
        return pixel / (scale > 0.0 ? scale : 1.0);
    }
    QVector<RemoteMonitorGeometry::Output> outputs;
    outputs.reserve(mapping.wire.size());
    for (qsizetype i = 0; i < mapping.wire.size(); ++i) {
        outputs.push_back({mapping.logicalOrigins.at(i), mapping.wire.at(i).geometry.size(), mapping.scales.at(i), mapping.wire.at(i).primary});
    }
    return RemoteMonitorGeometry::wireToLogical(pixel, mapping.wire, outputs);
}

}
}
