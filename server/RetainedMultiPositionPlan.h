// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RetainedKScreenReadback.h"
#include "RemoteTopologyDraft.h"

#include <optional>

namespace KRdp::RetainedMultiPositionPlan
{
struct Plan {
    QVector<RemoteTopologyCatalog::Output> after;
    bool changed = false;
};

// Preflight the whole arrangement, not just the target of a move. This also
// accepts a batch so Fit's dependent moves can later use one KScreen command.
inline std::optional<Plan> make(const RetainedKScreenReadback::Snapshot &before,
    const QString &owner, const QVector<RetainedKScreenReadback::Placement> &positions)
{
    if (owner.isEmpty() || before.outputs.size() < 2 || before.outputs.size() > 16
        || positions.isEmpty() || positions.size() > before.outputs.size()
        || !RetainedKScreenReadback::positionArguments(before, positions)) return {};
    RemoteTopologyCatalog::Snapshot snapshot;
    snapshot.generation = QStringLiteral("worker-preflight");
    snapshot.revision = 1;
    for (const auto &output : before.outputs) snapshot.outputs.append({output.backendKey, output});
    RemoteTopologyDraft::Request request;
    request.generation = snapshot.generation;
    request.expectedRevision = snapshot.revision;
    request.owner = owner;
    for (const auto &position : positions) {
        request.operations.append({.kind = RemoteTopologyDraft::Operation::Kind::Move,
            .id = position.outputName, .position = position.logicalPosition, .pixels = {}, .scale = 1.0});
    }
    const RemoteTopologyDraft::Capabilities caps{.moveVirtual = true, .maxOutputs = 16,
        .maxOutputDimension = 4096, .maxAtlasDimension = 32768};
    const auto proposal = RemoteTopologyDraft::preview(snapshot, caps, request);
    if (!proposal.valid()) return {};
    Plan result;
    for (const auto &entry : proposal.after) result.after.append(entry.output);
    result.changed = proposal.after != proposal.before;
    return result;
}

inline bool matches(const Plan &plan, const RetainedKScreenReadback::Snapshot &readback)
{
    return readback.outputs == plan.after;
}
}
