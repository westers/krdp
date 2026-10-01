// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ConsoleWorkerWire.h"
#include "RemoteMonitorGeometry.h"
#include "RemoteTopologyCatalog.h"

#include <algorithm>
#include <cmath>

#include <QSet>

namespace KRdp::ConsoleFrameLayout
{
// The worker's output order is the RDP surface-index order. The public
// catalog is backend-key sorted instead, so never match those by array index.
// Require the captured name/geometry/scale/primary inventory and the exact
// projected pixel atlas before installing a new RDPGFX monitor layout.
inline bool confirmed(const VideoFrame &frame, const ConsoleWorkerWire::Outputs &captured,
    const RemoteTopologyCatalog::Snapshot &snapshot)
{
    if (captured.monitors.isEmpty() || captured.monitors.size() > 16 // VideoStream's surface limit.
        || captured.monitors.size() != snapshot.outputs.size()
        || frame.monitors.size() != captured.monitors.size()
        || frame.monitorIndex < 0 || frame.monitorIndex >= frame.monitors.size()
        || frame.size != frame.monitors[frame.monitorIndex].geometry.size()) return false;
    QVector<RemoteMonitorGeometry::Output> projection;
    QSet<QString> seen;
    int primaryCount = 0;
    for (const auto &monitor : captured.monitors) {
        if (monitor.name.isEmpty() || monitor.geometry.isEmpty() || seen.contains(monitor.name)) return false;
        seen.insert(monitor.name);
        if (monitor.primary) ++primaryCount;
        const auto found = std::find_if(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&monitor](const auto &entry) {
            return entry.output.backendKey == monitor.name;
        });
        if (found == snapshot.outputs.cend() || !found->output.enabled
            || found->output.logicalGeometry != monitor.geometry.translated(captured.compositorOrigin)
            || found->output.primary != monitor.primary
            || std::abs(found->output.scale - monitor.scale) > 0.000001
            || found->output.nativePixels.isEmpty()) return false;
        projection.append({found->output.logicalGeometry.topLeft(), found->output.nativePixels,
            found->output.scale, found->output.primary});
    }
    return primaryCount == 1 && RemoteMonitorGeometry::projectToWire(projection) == frame.monitors;
}
// Workspace capture uses one aggregate surface and the frame's logical monitor
// list. It must not be installed as an independent per-monitor surface layout.
inline bool workspaceConfirmed(const VideoFrame &frame, const ConsoleWorkerWire::Outputs &captured,
    const RemoteTopologyCatalog::Snapshot &snapshot)
{
    if (captured.monitors.isEmpty() || captured.monitors.size() != snapshot.outputs.size()
        || frame.monitors.size() != captured.monitors.size() || frame.monitorIndex != 0) return false;
    QSet<QString> seen;
    QRect bounds;
    int primaries = 0;
    for (qsizetype i = 0; i < captured.monitors.size(); ++i) {
        const auto &monitor = captured.monitors[i];
        const auto found = std::find_if(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&monitor](const auto &entry) {
            return entry.output.backendKey == monitor.name;
        });
        if (seen.contains(monitor.name) || found == snapshot.outputs.cend() || !found->output.enabled
            || found->output.logicalGeometry != monitor.geometry.translated(captured.compositorOrigin)
            || found->output.primary != monitor.primary || std::abs(found->output.scale - monitor.scale) > 0.000001
            || frame.monitors[i].geometry != monitor.geometry || frame.monitors[i].primary != monitor.primary) return false;
        seen.insert(monitor.name);
        primaries += monitor.primary;
        bounds |= monitor.geometry;
    }
    return primaries == 1 && bounds.topLeft() == QPoint(0, 0)
        && frame.size.width() >= bounds.width() && frame.size.height() >= bounds.height();
}

}
