// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <utility>

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include "SurfaceLayout.h"

namespace KRdp
{
/**
 * What an RDP client says about its own display, and the pure rules that turn
 * it into virtual-monitor requests (OPT-041). No Qt GUI, no FreeRDP: the
 * connection fills Info from the peer settings and the controller consumes it.
 */
namespace ClientDisplay
{
/** Below this a desktop is unusable (RDP clients send 0x0 when they do not know). */
constexpr int MinDimension = 640;
/** Per-output VA-API surface limit; the same value as MultiLayout::MaxEncodeDimension. */
constexpr int MaxDimension = 4096;
/** RDPGFX allows 16 monitors; so does MultiLayout::MaxMonitorCount. */
constexpr int MaxMonitors = 16;

struct Info {
    /** TS_UD_CS_CORE desktopWidth/desktopHeight. */
    QSize desktopSize;
    /**
     * TS_UD_CS_MONITOR entries in the client's own coordinate space (after
     * sanitize(): translated so the union's top-left is (0,0)). Empty when the
     * client advertised none or the list was unusable; then desktopSize is the
     * only request.
     */
    QVector<VideoMonitor> monitors;

    bool operator==(const Info &other) const = default;
};

inline bool usable(const QSize &size)
{
    return size.width() >= MinDimension && size.width() <= MaxDimension && size.height() >= MinDimension && size.height() <= MaxDimension;
}

/**
 * The name requested from KWin for client monitor \a index at \a size. Stable
 * per (index, size) on purpose: KWin remembers output arrangements keyed on the
 * set of present outputs, so a stable name keeps that list bounded and lets the
 * server re-assert the physical layout on every connect instead of chasing a
 * new identity each time. KWin exposes the output as "Virtual-<name>".
 */
inline QString virtualMonitorName(int index, const QSize &size)
{
    return QStringLiteral("krdp-m%1-%2x%3").arg(index).arg(size.width()).arg(size.height());
}

/**
 * Apply the size rules: an unusable desktop becomes \a fallback; a monitor list
 * is kept only when it has 2..MaxMonitors entries, every entry is usable and
 * exactly one is primary, in which case it is translated to a (0,0) origin and
 * the desktop size becomes the union's size (what RDPGFX will be told anyway).
 */
inline Info sanitize(Info info, const QSize &fallback)
{
    const bool listUsable = info.monitors.size() >= 2 && info.monitors.size() <= MaxMonitors
        && std::all_of(info.monitors.cbegin(), info.monitors.cend(), [](const VideoMonitor &m) {
               return usable(m.geometry.size());
           })
        && std::count_if(info.monitors.cbegin(), info.monitors.cend(), [](const VideoMonitor &m) {
               return m.primary;
           }) == 1;
    if (!listUsable) {
        info.monitors.clear();
        if (!usable(info.desktopSize)) {
            info.desktopSize = fallback;
        }
        return info;
    }

    QRect unionRect;
    for (const auto &monitor : std::as_const(info.monitors)) {
        unionRect |= monitor.geometry;
    }
    for (auto &monitor : info.monitors) {
        monitor.geometry.translate(-unionRect.topLeft());
    }
    info.desktopSize = unionRect.size();
    return info;
}

/** KWin-global logical rects for sanitised \a monitors placed with their union's top-left at \a anchor. */
inline QVector<QRect> placement(const QVector<VideoMonitor> &monitors, const QPoint &anchor)
{
    QVector<QRect> rects;
    rects.reserve(monitors.size());
    for (const auto &monitor : monitors) {
        rects.push_back(monitor.geometry.translated(anchor));
    }
    return rects;
}
}
}
