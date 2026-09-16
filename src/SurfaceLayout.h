// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

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
 * The RDP desktop is the union of the monitors with the primary's top-left at
 * (0, 0), so `rdp = input - primary.topLeft()`. This is the one and only place
 * that conversion is done; everything else consumes the entries below.
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
 * Turn \a monitors into one surface entry each, translated into RDP desktop
 * space.
 *
 * The result always has exactly one primary and that primary always sits at
 * (0, 0): a layout with several primaries keeps only the first, and one with
 * none is anchored on (and takes its primary from) its first monitor. Monitors
 * left of or above the primary therefore get negative origins, which mstsc
 * accepts as long as the primary contains (0, 0).
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

    const QPoint anchor = monitors.at(primaryIndex).geometry.topLeft();

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
