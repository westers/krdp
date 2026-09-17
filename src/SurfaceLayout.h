// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVector>

namespace KRdp
{

/**
 * One monitor of the layout that is being streamed.
 *
 * Lives here rather than in VideoFrame.h/VideoStream.h so the layout maths
 * below stays free of FreeRDP and Qt GUI and can be unit tested on its own.
 */
struct VideoMonitor {
    QRect geometry;
    bool primary = false;

    bool operator==(const VideoMonitor &other) const
    {
        return geometry == other.geometry && primary == other.primary;
    }
};

/**
 * Translation from a monitor layout to the RDPGFX surfaces that carry it.
 *
 * The RDP desktop is the bounding union of the monitors, anchored at that
 * union's top-left, so `rdp = input - union.topLeft()` and no coordinate is
 * ever negative. The primary is identified by the MONITOR_PRIMARY flag alone,
 * not by sitting at the origin: `MapSurfaceToOutput`'s outputOriginX/Y are
 * UINT32 on the wire (MS-RDPEGFX 2.2.2.10), so a negative origin would wrap.
 * Anchoring on the primary instead is an mstsc convention; FreeRDP's own
 * shadow server sends union-anchored X11 coordinates in ResetGraphics, and the
 * client this fork targets is a FreeRDP 3 client.
 *
 * This is the one and only place that conversion is done; everything else
 * consumes the entries below unchanged, and originOf() inverts it for the
 * input path.
 *
 * Pure: no Qt GUI, no globals, no I/O, so it links (and tests) without a
 * display.
 */
namespace SurfaceLayout
{

/**
 * One RDPGFX surface: created at \a size, mapped to the output at \a origin.
 */
struct Entry {
    /** Size of the surface, in pixels (RDPGFX_CREATE_SURFACE_PDU). */
    QSize size;
    /** Top-left in RDP desktop space (RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU). */
    QPoint origin;
    /** Whether this monitor gets MONITOR_PRIMARY in RDPGFX_RESET_GRAPHICS. */
    bool primary = false;

    bool operator==(const Entry &other) const
    {
        return size == other.size && origin == other.origin && primary == other.primary;
    }
};

/**
 * The four edges of one TS_MONITOR_DEF, as RDPGFX_RESET_GRAPHICS wants them.
 */
struct MonitorEdges {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

/**
 * Turn \a geometry into the edges of a TS_MONITOR_DEF.
 *
 * MS-RDPBCGR 2.2.1.3.6.1 defines right and bottom as INCLUSIVE: a 1024x768
 * primary at the origin is left 0, top 0, right 1023, bottom 767, and a
 * 2560x1440 monitor at (2560, 0) ends at right 5119. Sending the exclusive
 * edges instead overstates every monitor by one pixel in each direction, which
 * is what this fork did until now - on the single-surface path too, not only
 * in MonitorMode=multi.
 *
 * The one place the conversion happens, so the two paths cannot disagree.
 * std::max keeps a degenerate rect from reporting an edge before its own
 * origin; the layout paths reject empty geometries, so that is belt and braces.
 */
inline MonitorEdges edgesOf(const QRect &geometry)
{
    const int left = geometry.x();
    const int top = geometry.y();
    return MonitorEdges{
        .left = left,
        .top = top,
        .right = std::max(left, left + geometry.width() - 1),
        .bottom = std::max(top, top + geometry.height() - 1),
    };
}

/**
 * The top-left of \a monitors' bounding union, in the coordinate space
 * \a monitors is given in.
 *
 * This is the offset fromMonitors() subtracts, so it is also what the input
 * path adds back: a pointer position in RDP desktop space becomes a position
 * in the caller's space by adding this. An empty layout has its origin at
 * (0, 0).
 */
inline QPoint originOf(const QVector<VideoMonitor> &monitors)
{
    QRect bounds;
    for (const auto &monitor : monitors) {
        bounds = bounds.united(monitor.geometry);
    }
    return bounds.topLeft();
}

/**
 * Turn \a monitors into one surface entry each, translated into RDP desktop
 * space.
 *
 * Every origin is non-negative and the topmost-leftmost monitor sits at
 * (0, 0). The result always has exactly one primary, wherever it lands: a
 * layout with several primaries keeps only the first, and one with none takes
 * its primary from its first monitor.
 *
 * An empty layout yields no entries, meaning "no explicit layout configured".
 */
inline QVector<Entry> fromMonitors(const QVector<VideoMonitor> &monitors)
{
    QVector<Entry> entries;
    if (monitors.isEmpty()) {
        return entries;
    }

    qsizetype primaryIndex = 0;
    for (qsizetype i = 0; i < monitors.size(); ++i) {
        if (monitors.at(i).primary) {
            primaryIndex = i;
            break;
        }
    }

    const QPoint anchor = originOf(monitors);

    entries.reserve(monitors.size());
    for (qsizetype i = 0; i < monitors.size(); ++i) {
        const auto &monitor = monitors.at(i);
        entries.push_back(Entry{
            .size = monitor.geometry.size(),
            .origin = monitor.geometry.topLeft() - anchor,
            .primary = (i == primaryIndex),
        });
    }

    return entries;
}

}
}
