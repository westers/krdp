// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ConsoleWorkerWire.h"
#include "H264KeyframeSize.h"
#include "RetainedKScreenReadback.h"

#include <algorithm>
#include <optional>

namespace KRdp::ConsoleTopologyReadback
{
// A physical Console topology is read-only until it has its own transactional
// mutation path. KScreen supplies exact native mode/scale and global logical
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
}
