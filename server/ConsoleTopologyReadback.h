// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ConsoleWorkerWire.h"
#include "H264KeyframeSize.h"
#include "RetainedKScreenReadback.h"
#include "RetainedMultiPrimaryPlan.h"

#include <algorithm>
#include <optional>

namespace KRdp::ConsoleTopologyReadback
{
// A physical Console catalog needs capture proof before any transaction.
// KScreen supplies exact native mode/scale and global logical
// coordinates; QScreen and the encoded frame independently prove that this is
// the desktop currently being captured. Unsupported disabled/rotated/mirrored
// configurations fail closed in the KScreen parser for now.
inline std::optional<ConsoleWorkerWire::Topology> confirmed(const RetainedKScreenReadback::Snapshot &kscreen,
    const ConsoleWorkerWire::Outputs &worker, const VideoFrame &frame)
{
    if (!frame.isKeyFrame || h264KeyframeSize(frame.data) != frame.size
        || kscreen.outputs.isEmpty() || kscreen.outputs.size() != worker.monitors.size()
        || frame.monitors.size() != worker.monitors.size() || frame.monitorIndex != 0) return {};
    QRect workspace;
    for (const auto &output : kscreen.outputs) workspace |= output.logicalGeometry;
    if (workspace.isEmpty() || frame.size.width() < workspace.width() || frame.size.height() < workspace.height()) return {};
    if (kscreen.outputs.size() == 1 && frame.size != kscreen.outputs.first().nativePixels) return {};
    ConsoleWorkerWire::Topology result;
    for (qsizetype i = 0; i < worker.monitors.size(); ++i) {
        const auto &published = worker.monitors[i];
        const auto it = std::find_if(kscreen.outputs.cbegin(), kscreen.outputs.cend(), [&published](const auto &output) {
            return output.backendKey == published.name;
        });
        if (it == kscreen.outputs.cend() || published.geometry != it->logicalGeometry.translated(-workspace.topLeft())
            || frame.monitors[i].geometry != published.geometry || frame.monitors[i].primary != it->primary
            || published.primary != it->primary) return {};
        result.outputs.append({it->name, it->nativePixels, it->logicalGeometry, it->scale, it->primary});
    }
    return result;
}

// Physical multi-output capture must prove *each* encoded surface. An
// aggregate workspace keyframe can agree with KScreen while one physical
// output has no independently decodable stream (or exceeds an encoder's
// per-surface limit). The same geometry/payload gate used by the retained
// compositor is valid here; ownership is supplied by the authenticated
// physical worker, never inferred from output names.
inline std::optional<ConsoleWorkerWire::Topology> confirmedMulti(const RetainedKScreenReadback::Snapshot &kscreen,
    const ConsoleWorkerWire::Outputs &worker, const QVector<VideoFrame> &keyframes)
{
    if (!RetainedKScreenReadback::matchesPublished(kscreen, worker, keyframes)) return {};
    ConsoleWorkerWire::Topology result;
    for (const auto &published : worker.monitors) {
        const auto it = std::find_if(kscreen.outputs.cbegin(), kscreen.outputs.cend(), [&published](const auto &output) {
            return output.backendKey == published.name;
        });
        if (it == kscreen.outputs.cend()) return {};
        result.outputs.append({it->name, it->nativePixels, it->logicalGeometry, it->scale, it->primary});
    }
    return result;
}

// The capture proof above does not contain KScreen's complete priority order.
// Bind the exact order from the *same* fresh JSON/snapshot before publishing a
// broker topology; boolean primary or QScreen enumeration cannot reconstruct it.
inline std::optional<ConsoleWorkerWire::Topology> withPriorities(ConsoleWorkerWire::Topology topology,
    const QByteArray &json, const RetainedKScreenReadback::Snapshot &kscreen)
{
    const auto priorities = RetainedMultiPrimaryPlan::priorities(json, kscreen);
    if (!priorities || topology.outputs.size() != kscreen.outputs.size()) return {};
    QSet<QString> matched;
    for (auto &output : topology.outputs) {
        const auto found = std::find_if(kscreen.outputs.cbegin(), kscreen.outputs.cend(), [&output](const auto &candidate) {
            return candidate.backendKey == output.name;
        });
        if (found == kscreen.outputs.cend() || matched.contains(output.name) || !priorities->contains(output.name)
            || output.pixels != found->nativePixels || output.logical != found->logicalGeometry
            || !VirtualResize::sameScale(output.scale, found->scale)
            || output.primary != found->primary || output.primary != (priorities->value(output.name) == 1)) return {};
        matched.insert(output.name);
        output.priority = quint8(priorities->value(output.name));
    }
    return topology;
}
}
