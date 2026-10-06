// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "ConsoleVirtualOutputPolicy.h"
#include "OutputSnapshot.h"

#include <cmath>
#include <numeric>
#include <tuple>
#include <QSet>
#include <QStringList>
#include "RemoteMonitorGeometry.h"

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

enum class Refusal : quint8 {
    None,
    Invalid, // The request, the snapshot or the inventory failed validation.
    NoHostScreens, // Mapped needs at least one enabled host screen.
    TooManyScreens, // More enabled host screens than ConsoleVirtualOutputPolicy::MaxMappedScreens.
    BadMonitorSize, // A mapped client monitor cannot be sized within the per-output limits.
    DesktopTooLarge, // Even packed edge to edge, the desktop exceeds 8192 on a side.
    TooManyOutputs, // Existing outputs plus the planned ones exceed the creation bound.
};

// One replaced host screen of a Layout::Mapped plan (what the later `console-screens` record reports).
struct MappedScreen {
    QString host;    // The host screen's own connector name.
    QString output;  // KWin name of the stand-in, Virtual- prefix included.
    QString monitor; // The client monitor it is shown on.
    QSize pixels;
    qreal scale = 1.0;
    bool primary = false;
    bool isDefault = true; // Mapped by the default rule, not by an entry of the request.
    bool clamped = false;  // Scaled down to the per-output limit.
    bool operator==(const MappedScreen &) const = default;
};
struct MappedReport {
    Refusal refusal = Refusal::None;
    QVector<MappedScreen> screens;   // In host order (primary first, then left to right, top to bottom).
    QStringList unknownHosts;        // Mapping entries that name no enabled host screen (ignored).
    QStringList unmappedMonitors;    // Client monitors that show no host screen (the client draws a panel).
    bool packed = false;             // Positions came from the packing rule, not the host's own positions.
};

namespace detail
{
inline int evenFloor(int value) { return value & ~1; }

// Clamp a monitor's physical size to the per-output limit, keeping the aspect ratio. Sizes up to 4096 pass
// unchanged (4096 is a multiple of 16 and 64, so encoder padding never exceeds it; docs/amd-encoder-padding.md).
inline std::optional<QSize> mappedPixels(const QSize &pixels, bool *clamped)
{
    int w = evenFloor(pixels.width()), h = evenFloor(pixels.height());
    *clamped = false;
    const int longest = std::max(w, h);
    if (longest > ClientDisplay::MaxDimension) {
        const double f = double(ClientDisplay::MaxDimension) / longest;
        w = evenFloor(int(std::floor(w * f)));
        h = evenFloor(int(std::floor(h * f)));
        *clamped = true;
    }
    const QSize result(w, h);
    return ClientDisplay::usable(result) ? std::optional(result) : std::nullopt;
}

inline bool mappedFits(const QVector<QRect> &logical, const QVector<RemoteMonitorGeometry::Output> &atlas)
{
    QRect logicalUnion;
    for (const auto &r : logical) logicalUnion |= r;
    QRect atlasUnion;
    for (const auto &m : RemoteMonitorGeometry::projectToWire(atlas)) atlasUnion |= m.geometry;
    return ClientDisplay::usableDesktop(logicalUnion.size()) && ClientDisplay::usableDesktop(atlasUnion.size());
}

// Layout::Mapped: one output per enabled host screen (spec 2026-10-06 section 4.1/4.2).
inline std::optional<QVector<Output>> planMapped(const ConsoleVirtualOutputPolicy &request,
    QVector<OutputSnapshot::Output> hosts, bool forceSingle, const QPoint &anchor, const QPoint &park, MappedReport &report)
{
    using P = ConsoleVirtualOutputPolicy;
    const auto refuse = [&report](Refusal reason) -> std::optional<QVector<Output>> { report.refusal = reason; return std::nullopt; };
    if (hosts.isEmpty()) return refuse(Refusal::NoHostScreens);
    if (hosts.size() > P::MaxMappedScreens) return refuse(Refusal::TooManyScreens);

    // Host order: the primary (lowest KWin priority) first, then left to right, then top to bottom.
    int primaryPriority = hosts.first().priority;
    for (const auto &h : hosts) primaryPriority = std::min(primaryPriority, h.priority);
    std::stable_sort(hosts.begin(), hosts.end(), [primaryPriority](const auto &a, const auto &b) {
        const bool pa = a.priority == primaryPriority, pb = b.priority == primaryPriority;
        if (pa != pb) return pa;
        return std::make_tuple(a.position.x(), a.position.y(), a.name) < std::make_tuple(b.position.x(), b.position.y(), b.name);
    });
    // Client order: the primary first, then left to right, then top to bottom.
    auto clients = request.mappedMonitors;
    std::stable_sort(clients.begin(), clients.end(), [](const auto &a, const auto &b) {
        if (a.primary != b.primary) return a.primary;
        const auto ka = std::make_tuple(a.geometry.x(), a.geometry.y(), a.id), kb = std::make_tuple(b.geometry.x(), b.geometry.y(), b.id);
        return ka < kb;
    });
    const auto clientIndex = [&clients](const QString &id) {
        for (int i = 0; i < clients.size(); ++i) if (clients[i].id == id) return i;
        return -1;
    };

    for (const auto &entry : request.mapping) {
        const bool known = std::any_of(hosts.cbegin(), hosts.cend(), [&entry](const auto &h) { return h.name == entry.host; });
        if (!known) report.unknownHosts.append(entry.host);
    }

    if (forceSingle) {
        // The fallback ladder's single output: the client primary's size, at the anchor. Unmapped = everything else.
        bool clamped = false;
        const auto &primary = clients.first();
        const auto size = mappedPixels(primary.geometry.size(), &clamped);
        if (!size) return refuse(Refusal::BadMonitorSize);
        for (int i = 1; i < clients.size(); ++i) report.unmappedMonitors.append(clients[i].id);
        return QVector<Output>{{OutputSnapshot::VirtualPrefix + ClientDisplay::virtualMonitorName(0, *size), *size, anchor, park, true,
            primary.scalePercent / 100.0}};
    }

    const int M = clients.size();
    QVector<Output> outputs;
    QVector<QRect> hostRects;
    QVector<RemoteMonitorGeometry::Output> atlasProbe;
    QVector<bool> used(M, false);
    QVector<QRect> logical;
    for (int i = 0; i < hosts.size(); ++i) {
        const auto &host = hosts[i];
        int target = std::min(i, M - 1);
        bool isDefault = true;
        for (const auto &entry : request.mapping) {
            if (entry.host != host.name) continue;
            target = clientIndex(entry.monitor); // validated by isValid(): always found
            isDefault = false;
        }
        if (target < 0) return refuse(Refusal::Invalid);
        const auto &monitor = clients[target];
        bool clamped = false;
        const auto size = mappedPixels(monitor.geometry.size(), &clamped);
        if (!size) return refuse(Refusal::BadMonitorSize);
        used[target] = true;
        const qreal scale = monitor.scalePercent / 100.0;
        const QString name = OutputSnapshot::VirtualPrefix + ClientDisplay::mappedMonitorName(i, *size);
        outputs.append({name, *size, {}, {}, i == 0, scale});
        report.screens.append({host.name, name, monitor.id, *size, scale, i == 0, isDefault, clamped});
        hostRects.append(RemoteMonitorGeometry::logicalRect(host.position, host.size, host.scale));
        logical.append(RemoteMonitorGeometry::logicalRect({}, *size, scale));
    }
    for (int i = 0; i < M; ++i) if (!used[i]) report.unmappedMonitors.append(clients[i].id);

    const auto place = [&](const QVector<QPoint> &positions) {
        QVector<QRect> rects;
        QVector<RemoteMonitorGeometry::Output> atlas;
        for (int i = 0; i < outputs.size(); ++i) {
            rects.append(QRect(positions[i], logical[i].size()));
            atlas.append({positions[i], outputs[i].pixels, outputs[i].scale, i == 0});
        }
        return std::make_pair(rects, atlas);
    };
    // Rule A: the host screen's own logical top-left, relative to the host desktop's top-left.
    QRect hostUnion;
    for (const auto &r : hostRects) hostUnion |= r;
    QVector<QPoint> positions;
    for (const auto &r : hostRects) positions.append(r.topLeft() - hostUnion.topLeft());
    bool ok;
    {
        const auto [rects, atlas] = place(positions);
        ok = mappedFits(rects, atlas);
        for (int i = 0; ok && i < rects.size(); ++i)
            for (int j = 0; ok && j < i; ++j) ok = !rects[i].intersects(rects[j]);
    }
    if (!ok) {
        // Rule B: pack. Rows are host screens whose vertical ranges overlap; edge to edge in host order, top aligned,
        // rows stacked in host order.
        report.packed = true;
        QVector<int> order(hosts.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&hostRects](int a, int b) {
            return std::make_pair(hostRects[a].y(), hostRects[a].x()) < std::make_pair(hostRects[b].y(), hostRects[b].x());
        });
        QVector<QVector<int>> rows;
        QVector<std::pair<int, int>> spans; // [top, bottom) of each row on the host.
        for (const int i : std::as_const(order)) {
            const int top = hostRects[i].top(), bottom = hostRects[i].top() + hostRects[i].height();
            int found = -1;
            for (int r = 0; r < rows.size() && found < 0; ++r)
                if (top < spans[r].second && bottom > spans[r].first) found = r;
            if (found < 0) { rows.append(QVector<int>{i}); spans.append(std::make_pair(top, bottom)); }
            else { rows[found].append(i); spans[found] = std::make_pair(std::min(spans[found].first, top), std::max(spans[found].second, bottom)); }
        }
        std::stable_sort(rows.begin(), rows.end(), [&hostRects](const auto &a, const auto &b) {
            return hostRects[a.first()].y() < hostRects[b.first()].y();
        });
        int y = 0;
        for (auto &row : rows) {
            std::stable_sort(row.begin(), row.end(), [&hostRects](int a, int b) { return hostRects[a].x() < hostRects[b].x(); });
            int x = 0, height = 0;
            for (const int i : std::as_const(row)) {
                positions[i] = QPoint(x, y);
                x += logical[i].width();
                height = std::max(height, logical[i].height());
            }
            y += height;
        }
        const auto [rects, atlas] = place(positions);
        if (!mappedFits(rects, atlas)) return refuse(Refusal::DesktopTooLarge);
    }
    for (int i = 0; i < outputs.size(); ++i) {
        outputs[i].position = positions[i] + anchor;
        outputs[i].parkPosition = positions[i] + park;
    }
    return outputs;
}
}

// The lifecycle supplies a fresh PhysicalOutputGuard snapshot and the complete
// pre-creation inventory. This pure plan never takes a hold, creates an
// output, or treats a matching name as evidence that we own an existing output.
inline std::optional<Plan> build(const ConsoleVirtualOutputPolicy &request,
    const QVector<OutputSnapshot::Output> &physical, const QVector<OutputSnapshot::Output> &inventory,
    bool forceSingle = false, int maximumOutputs = ClientDisplay::MaxMonitors, MappedReport *mapped = nullptr)
{
    MappedReport scratch;
    auto &report = mapped ? *mapped : scratch;
    report = {};
    if (!request.isValid() || !request.enabled || physical.size() > 16 || inventory.size() > 16
        || maximumOutputs < 1 || maximumOutputs > ClientDisplay::MaxMonitors) {
        report.refusal = Refusal::Invalid;
        return {};
    }
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
                || output.priority < 1 || output.priority > 16 || priorities.contains(output.priority)))) { report.refusal = Refusal::Invalid; return {}; }
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
                || output.size.height() < 1 || output.size.height() > 16384))) { report.refusal = Refusal::Invalid; return {}; }
        physicalNames.insert(output.name);
    }
    const bool hasPhysical = std::any_of(physical.cbegin(), physical.cend(), [](const auto &o) { return o.enabled; });
    Plan result;
    result.replace = request.policy == ConsoleVirtualOutputPolicy::Policy::Replace && hasPhysical;
    const QPoint park = OutputSnapshot::rightmostEnabledAnchor(inventory);
    const QPoint anchor = result.replace ? QPoint(0, 0) : park;
    auto info = request.client;
    if (request.layout == ConsoleVirtualOutputPolicy::Layout::Mapped) {
        auto hosts = physical;
        hosts.erase(std::remove_if(hosts.begin(), hosts.end(), [](const auto &o) { return !o.enabled; }), hosts.end());
        auto outputs = detail::planMapped(request, hosts, forceSingle, anchor, park, report);
        if (!outputs) return {};
        result.outputs = *outputs;
        result.singleFallback = forceSingle;
        // A fallback single output is not a replacement of every screen, but it still replaces when Replace was asked.
        if (existingNames.size() + result.outputs.size() > maximumOutputs) { report.refusal = Refusal::TooManyOutputs; return {}; }
        for (const auto &output : result.outputs) {
            if (existingNames.contains(output.name) || output.position.x() < -32767 || output.position.x() > 32767
                || output.position.y() < -32767 || output.position.y() > 32767
                || output.parkPosition.x() < -32767 || output.parkPosition.x() > 32767
                || output.parkPosition.y() < -32767 || output.parkPosition.y() > 32767) {
                report.refusal = Refusal::Invalid;
                return {};
            }
        }
        return result;
    }
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
