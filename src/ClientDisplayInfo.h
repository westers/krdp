// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <cstdlib>
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
/**
 * The RDP desktop limit (MS-RDPBCGR TS_UD_CS_CORE desktopWidth/desktopHeight),
 * used to bound the *union* of a sanitised client monitor list. This is not
 * the same limit as MaxDimension: `VirtualMonitorLayout=client` opens one
 * virtual output - and one VA-API surface - per client monitor
 * (SessionController::buildVirtualSessions()), each already checked against
 * MaxDimension on its own above, so the union only has to fit inside what RDP
 * itself can carry, not inside one encoder surface. The one-output paths
 * (`VirtualMonitorLayout=single`, and the fallback taken when the monitor
 * list is dropped) still ask singleSize() for a size bounded by MaxDimension.
 */
constexpr int MaxDesktopDimension = 8192;
/** RDPGFX allows 16 monitors; so does MultiLayout::MaxMonitorCount. */
constexpr int MaxMonitors = 16;
/** TS_MONITOR_DEF coordinates are INT32; nothing past this is a monitor position, and a union of it could overflow. */
constexpr int MaxCoordinate = 32767;

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

/** Whether \a size fits the RDP desktop limit: the bound for a client monitor union (see MaxDesktopDimension). */
inline bool usableDesktop(const QSize &size)
{
    return size.width() >= MinDimension && size.width() <= MaxDesktopDimension && size.height() >= MinDimension && size.height() <= MaxDesktopDimension;
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

/** Whether every pair of \a monitors is disjoint (touching edges are fine). */
inline bool disjoint(const QVector<VideoMonitor> &monitors)
{
    for (qsizetype i = 0; i < monitors.size(); ++i) {
        for (qsizetype j = i + 1; j < monitors.size(); ++j) {
            if (monitors.at(i).geometry.intersects(monitors.at(j).geometry)) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Apply the size rules: an unusable desktop becomes \a fallback; a monitor list
 * is kept only when it has 2..MaxMonitors entries, every entry is usable and
 * sanely placed (|x|,|y| <= MaxCoordinate), no two overlap, exactly one is
 * primary and the union fits the RDP desktop limit (MaxDesktopDimension; no
 * position handed to kscreen-doctor may be absurd either), in which case it
 * is translated to a (0,0) origin and the desktop size becomes the union's
 * size (what RDPGFX will be told anyway). The union bound is the RDP desktop
 * limit, not the per-output encoder limit (MaxDimension): the `client` layout
 * opens one virtual output per monitor, each already checked against
 * MaxDimension above, so a union past 4096 but within MaxDesktopDimension is
 * still kept. The one-output paths (`single`, and the fallback when a list is
 * dropped here) ask singleSize() instead, which stays bounded by MaxDimension.
 */
inline Info sanitize(Info info, const QSize &fallback)
{
    const auto dropList = [&info, &fallback]() {
        info.monitors.clear();
        if (!usable(info.desktopSize)) {
            info.desktopSize = fallback;
        }
        return info;
    };
    const bool listUsable = info.monitors.size() >= 2 && info.monitors.size() <= MaxMonitors
        && std::all_of(info.monitors.cbegin(), info.monitors.cend(), [](const VideoMonitor &m) {
               return usable(m.geometry.size()) && std::abs(m.geometry.x()) <= MaxCoordinate && std::abs(m.geometry.y()) <= MaxCoordinate;
           })
        && std::count_if(info.monitors.cbegin(), info.monitors.cend(), [](const VideoMonitor &m) {
               return m.primary;
           }) == 1
        && disjoint(info.monitors);
    if (!listUsable) {
        return dropList();
    }

    QRect unionRect;
    for (const auto &monitor : std::as_const(info.monitors)) {
        unionRect |= monitor.geometry;
    }
    if (!usableDesktop(unionRect.size())) {
        return dropList();
    }
    for (auto &monitor : info.monitors) {
        monitor.geometry.translate(-unionRect.topLeft());
    }
    info.desktopSize = unionRect.size();
    return info;
}

/**
 * The size to ask one virtual output for when the client gets a single one
 * (`VirtualMonitorLayout=single`, or the one-output fallback): the sanitised
 * desktop size, which sanitize() keeps usable, with \a fallback as the belt.
 */
inline QSize singleSize(const Info &info, const QSize &fallback)
{
    return usable(info.desktopSize) ? info.desktopSize : fallback;
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
