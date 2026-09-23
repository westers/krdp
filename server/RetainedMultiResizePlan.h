// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RetainedKScreenReadback.h"
#include "RemoteTopologyDraft.h"
#include "VirtualResize.h"

#include <algorithm>
#include <optional>

namespace KRdp::RetainedMultiResizePlan
{
struct Plan {
    RetainedKScreenReadback::Snapshot before;
    QVector<RemoteTopologyCatalog::Output> after;
    QString output;
    QSize pixels;
    double scale = 1;
    bool changed = false;
};

// One-output mode/scale change only. The full inventory, ownership and pixel
// atlas are preflighted before KScreen is allowed to mutate anything. Managed
// dependent reflow belongs to the later multi-operation Fit transaction.
inline std::optional<Plan> make(const RetainedKScreenReadback::Snapshot &before, const QString &owner,
    const QString &output, QSize pixels, double scale)
{
    if (owner.isEmpty() || !RetainedKScreenReadback::outputName(output)
        || !VirtualResize::validRequest(pixels, scale) || before.outputs.size() < 2
        || before.outputs.size() > 16) return {};
    const auto found = std::find_if(before.outputs.cbegin(), before.outputs.cend(), [&output](const auto &candidate) {
        return candidate.backendKey == output;
    });
    if (found == before.outputs.cend() || found->owner != owner || found->physical) return {};
    RemoteTopologyCatalog::Snapshot snapshot;
    snapshot.generation = QStringLiteral("preflight");
    snapshot.revision = 1;
    for (const auto &entry : before.outputs) snapshot.outputs.append({entry.backendKey, entry});
    RemoteTopologyDraft::Request request;
    request.generation = snapshot.generation;
    request.expectedRevision = snapshot.revision;
    request.owner = owner;
    request.operations.append({.kind = RemoteTopologyDraft::Operation::Kind::Resize,
        .id = output, .position = {}, .pixels = pixels, .scale = scale});
    const RemoteTopologyDraft::Capabilities caps{.resizeVirtual = true, .maxOutputs = 16,
        .maxOutputDimension = 4096, .maxAtlasDimension = 32768};
    const auto proposal = RemoteTopologyDraft::preview(snapshot, caps, request);
    if (!proposal.valid()) return {};
    Plan result;
    result.before = before;
    result.output = output;
    result.pixels = pixels;
    result.scale = scale;
    for (const auto &entry : proposal.after) result.after.append(entry.output);
    result.changed = proposal.after != proposal.before;
    return result;
}

inline bool matches(const Plan &plan, const RetainedKScreenReadback::Snapshot &readback)
{
    if (readback.outputs.size() != plan.after.size()) return false;
    for (const auto &expected : plan.after) {
        const auto found = std::find_if(readback.outputs.cbegin(), readback.outputs.cend(), [&expected](const auto &candidate) {
            return candidate.backendKey == expected.backendKey;
        });
        if (found == readback.outputs.cend() || found->nativePixels != expected.nativePixels
            || found->logicalGeometry != expected.logicalGeometry || found->primary != expected.primary
            || !VirtualResize::sameScale(found->scale, expected.scale)) return false;
    }
    return true;
}
}
