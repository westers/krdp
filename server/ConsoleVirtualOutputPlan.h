// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "ConsoleVirtualOutputPolicy.h"
#include "OutputSnapshot.h"

#include <cmath>
#include <QSet>

namespace KRdp::ConsoleVirtualOutputPlan
{
struct Output {
    QString name; // KWin name, including Virtual- prefix.
    QSize pixels;
    QPoint position;
    QPoint parkPosition;
    bool primary = false; // Captured layout's primary, not the physical seat's.
    qreal scale = 1.0;
    bool operator==(const Output &) const = default;
};
struct Plan {
    QVector<Output> outputs;
    bool replace = false;
    bool mirroredPhysical = false;
    bool singleFallback = false;
    bool operator==(const Plan &) const = default;
};

// The lifecycle supplies a fresh PhysicalOutputGuard snapshot and the complete
// pre-creation inventory. This pure plan never takes a hold, creates an
// output, or treats a matching name as evidence that we own an existing output.
inline std::optional<Plan> build(const ConsoleVirtualOutputPolicy &request,
    const QVector<OutputSnapshot::Output> &physical, const QVector<OutputSnapshot::Output> &inventory,
    bool forceSingle = false, int maximumOutputs = ClientDisplay::MaxMonitors)
{
    if (!request.isValid() || !request.enabled || physical.size() > 16 || inventory.size() > 16
        || maximumOutputs < 1 || maximumOutputs > ClientDisplay::MaxMonitors) return {};
    QSet<QString> existingNames;
    QSet<int> priorities;
    for (const auto &output : inventory) {
        if (output.name.isEmpty() || output.name.size() > 128 || existingNames.contains(output.name)
            || !std::all_of(output.name.cbegin(), output.name.cend(), [](const auto c) {
                return c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_');
            })
            || !std::isfinite(output.scale) || output.scale < 1 || output.scale > 4
            || output.position.x() < -32767 || output.position.x() > 32767
            || output.position.y() < -32767 || output.position.y() > 32767
            || (output.enabled && (output.size.width() < 1 || output.size.width() > 16384
                || output.size.height() < 1 || output.size.height() > 16384
                || output.priority < 1 || output.priority > 16 || priorities.contains(output.priority)))) return {};
        existingNames.insert(output.name);
        if (output.enabled) priorities.insert(output.priority);
    }
    QSet<QString> physicalNames;
    for (const auto &output : physical) {
        if (output.name.isEmpty() || output.name.size() > 128 || OutputSnapshot::isVirtual(output.name)
            || physicalNames.contains(output.name) || !existingNames.contains(output.name)
            || !inventory.contains(output)
            || !std::isfinite(output.scale) || output.scale < 1 || output.scale > 4
            || output.position.x() < -32767 || output.position.x() > 32767
            || output.position.y() < -32767 || output.position.y() > 32767
            || (output.enabled && (output.size.width() < 1 || output.size.width() > 16384
                || output.size.height() < 1 || output.size.height() > 16384))) return {};
        physicalNames.insert(output.name);
    }
    const bool hasPhysical = std::any_of(physical.cbegin(), physical.cend(), [](const auto &o) { return o.enabled; });
    Plan result;
    result.replace = request.policy == ConsoleVirtualOutputPolicy::Policy::Replace && hasPhysical;
    const QPoint park = OutputSnapshot::rightmostEnabledAnchor(inventory);
    const QPoint anchor = result.replace ? QPoint(0, 0) : park;
    auto info = request.client;
    if (request.layout == ConsoleVirtualOutputPolicy::Layout::Physical && hasPhysical) {
        info = request.normalize(OutputSnapshot::toClientDisplayInfo(physical));
        if (!forceSingle) {
            auto enabled = physical;
            enabled.erase(std::remove_if(enabled.begin(), enabled.end(), [](const auto &o) { return !o.enabled; }), enabled.end());
            std::sort(enabled.begin(), enabled.end(), [](const auto &a, const auto &b) { return a.priority < b.priority; });
            const auto bounds = OutputSnapshot::enabledUnion(enabled);
            bool supported = ClientDisplay::usableDesktop(bounds.size());
            for (int i = 0; i < enabled.size(); ++i) {
                const auto &output = enabled[i];
                supported = supported && ClientDisplay::usable(output.size);
                const auto rectangle = RemoteMonitorGeometry::logicalRect(output.position, output.size, output.scale);
                for (int j = 0; j < i; ++j)
                    supported = supported && !rectangle.intersects(RemoteMonitorGeometry::logicalRect(
                        enabled[j].position, enabled[j].size, enabled[j].scale));
                result.outputs.append({OutputSnapshot::VirtualPrefix + ClientDisplay::virtualMonitorName(i, output.size), output.size,
                    output.position - bounds.topLeft() + anchor, output.position - bounds.topLeft() + park, i == 0, output.scale});
            }
            if (supported) result.mirroredPhysical = true;
            else result.outputs.clear(); // Unsupported mirror uses the bounded single-layout fallback below.
        }
    }
    const bool multiple = request.layout != ConsoleVirtualOutputPolicy::Layout::Single
        && info.monitors.size() >= 2 && !forceSingle;
    result.singleFallback = forceSingle;
    // A physical mirror preserves native pixels AND scale. Treating logical
    // positions as native-pixel rectangles would overlap fractional screens.
    if (result.outputs.isEmpty() && !multiple) {
        result.outputs.append({OutputSnapshot::VirtualPrefix + ClientDisplay::virtualMonitorName(0, ClientDisplay::singleSize(info, request.fallback)),
            ClientDisplay::singleSize(info, request.fallback), anchor, park, true});
    } else if (result.outputs.isEmpty()) {
        for (int i = 0; i < info.monitors.size(); ++i) {
            const auto &monitor = info.monitors[i];
            result.outputs.append({OutputSnapshot::VirtualPrefix + ClientDisplay::virtualMonitorName(i, monitor.geometry.size()),
                monitor.geometry.size(), monitor.geometry.topLeft() + anchor, monitor.geometry.topLeft() + park, monitor.primary});
        }
    }
    // Count connected outputs, including foreign virtual outputs. Creation is
    // sequential but still needs slots for the complete requested set.
    if (existingNames.size() + result.outputs.size() > maximumOutputs) return {};
    for (const auto &output : result.outputs) {
        if (existingNames.contains(output.name) || output.position.x() < -32767 || output.position.x() > 32767
            || output.position.y() < -32767 || output.position.y() > 32767
            || output.parkPosition.x() < -32767 || output.parkPosition.x() > 32767
            || output.parkPosition.y() < -32767 || output.parkPosition.y() > 32767) return {};
    }
    return result;
}
}
