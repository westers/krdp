// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RemoteTopologyFit.h"
#include "RetainedKScreenReadback.h"
#include "VirtualResize.h"

#include <algorithm>
#include <optional>

namespace KRdp::RetainedMultiFitPlan
{
struct Plan {
    RetainedKScreenReadback::Snapshot before;
    QVector<RemoteTopologyCatalog::Output> after;
    QString selected;
    QSize pixels;
    double scale = 1;
    QVector<RetainedKScreenReadback::Placement> placements;
    bool resizeChanged = false;
    bool changed = false;
};

// Backend-key relations must already have been resolved from the broker's
// generation-scoped IDs. This is only a whole-layout worker preflight: it does
// not authorize a transaction or mutate KWin. The broker must bind the exact
// relations/after-state to a revisioned one-use preview before dispatch.
inline std::optional<Plan> make(const RetainedKScreenReadback::Snapshot &before, const QString &owner,
    const QString &selected, QSize pixels, double scale, const QVector<RemoteTopologyFit::Relation> &relations)
{
    if (owner.isEmpty() || !RetainedKScreenReadback::outputName(selected)
        || !VirtualResize::validRequest(pixels, scale) || before.outputs.size() < 2
        || before.outputs.size() > 16 || relations.size() > 16) return {};
    RemoteTopologyCatalog::Snapshot snapshot;
    snapshot.generation = QStringLiteral("worker-fit-preflight");
    snapshot.revision = 1;
    for (const auto &output : before.outputs) snapshot.outputs.append({output.backendKey, output});
    RemoteTopologyDraft::Request request;
    request.generation = snapshot.generation;
    request.expectedRevision = snapshot.revision;
    request.owner = owner;
    const RemoteTopologyDraft::Capabilities caps{.moveVirtual = true, .resizeVirtual = true,
        .maxOutputs = 16, .maxOutputDimension = 4096, .maxAtlasDimension = 8192};
    const auto fit = RemoteTopologyFit::plan(snapshot, caps, request, selected, pixels, scale, relations);
    if (!fit.valid() || fit.request.operations.size() > 16) return {};
    Plan result;
    result.before = before;
    result.selected = selected;
    result.pixels = pixels;
    result.scale = scale;
    for (const auto &operation : fit.request.operations) {
        if (operation.kind == RemoteTopologyDraft::Operation::Kind::Resize) {
            if (result.resizeChanged || operation.id != selected) return {};
            result.resizeChanged = true;
        } else if (operation.kind == RemoteTopologyDraft::Operation::Kind::Move) {
            if (operation.position.x() < 0 || operation.position.y() < 0) return {};
            result.placements.append({operation.id, operation.position});
        } else return {};
    }
    if (!result.placements.isEmpty() && !RetainedKScreenReadback::positionArguments(before, result.placements)) return {};
    for (const auto &entry : fit.proposal.after) {
        if (entry.output.logicalGeometry.x() < 0 || entry.output.logicalGeometry.y() < 0) return {};
        result.after.append(entry.output);
    }
    result.changed = fit.proposal.after != fit.proposal.before;
    return result;
}

inline bool matches(const Plan &plan, const RetainedKScreenReadback::Snapshot &readback)
{
    return readback.outputs == plan.after;
}

// Build one KScreen apply containing the selected mode/scale and all dependent
// positions. A custom mode may be registered beforehand, but the active layout
// must still equal the original fresh snapshot when this command is built.
inline std::optional<QStringList> arguments(const Plan &plan, const VirtualResize::Snapshot &state,
    const VirtualResize::Mode &mode)
{
    const auto selected = std::find_if(plan.before.outputs.cbegin(), plan.before.outputs.cend(), [&plan](const auto &output) {
        return output.backendKey == plan.selected;
    });
    if (selected == plan.before.outputs.cend() || state.name != plan.selected
        || state.position != selected->logicalGeometry.topLeft() || state.current.pixels != selected->nativePixels
        || !VirtualResize::sameScale(state.scale, selected->scale)) return {};
    QStringList result;
    if (plan.resizeChanged) {
        if (mode.pixels != plan.pixels || std::none_of(state.modes.cbegin(), state.modes.cend(), [&mode](const auto &candidate) {
            return candidate.id == mode.id && candidate.pixels == mode.pixels && candidate.refresh == mode.refresh;
        })) return {};
        result = VirtualResize::select(state, mode, plan.scale);
    }
    if (!plan.placements.isEmpty()) {
        const auto positions = RetainedKScreenReadback::positionArguments(plan.before, plan.placements);
        if (!positions) return {};
        result.append(*positions);
    }
    return result;
}

// A failed KScreen apply is not necessarily atomic. Reconcile only a layout
// made entirely of the before/after values of this transaction; any third
// party change makes restoration unsafe and must be left alone.
inline std::optional<QStringList> recoveryArguments(const Plan &plan,
    const RetainedKScreenReadback::Snapshot &current, const VirtualResize::Snapshot &selectedState,
    const VirtualResize::Snapshot &originalState, const VirtualResize::Mode &appliedMode)
{
    if (current.outputs.size() != plan.before.outputs.size() || current.outputs.size() != plan.after.size()
        || selectedState.name != plan.selected || originalState.name != plan.selected
        || selectedState.id != originalState.id) return {};
    QStringList result;
    QVector<RetainedKScreenReadback::Placement> restorePositions;
    for (qsizetype i = 0; i < current.outputs.size(); ++i) {
        const auto &now = current.outputs[i];
        const auto &before = plan.before.outputs[i];
        const auto &after = plan.after[i];
        if (now.backendKey != before.backendKey || after.backendKey != before.backendKey
            || now.name != before.name || now.owner != before.owner || now.enabled != before.enabled
            || now.primary != before.primary || now.physical != before.physical
            || (now.nativePixels != before.nativePixels && now.nativePixels != after.nativePixels)
            || (!VirtualResize::sameScale(now.scale, before.scale)
                && !VirtualResize::sameScale(now.scale, after.scale))) return {};
        const auto origin = now.logicalGeometry.topLeft();
        if (origin != before.logicalGeometry.topLeft() && origin != after.logicalGeometry.topLeft()) return {};
        if (now.logicalGeometry.size() != RemoteMonitorGeometry::logicalRect(origin, now.nativePixels, now.scale).size()) return {};
        if (origin != before.logicalGeometry.topLeft()) restorePositions.append({now.backendKey, before.logicalGeometry.topLeft()});
    }
    const auto selected = std::find_if(current.outputs.cbegin(), current.outputs.cend(), [&plan](const auto &output) {
        return output.backendKey == plan.selected;
    });
    if (selected == current.outputs.cend() || selectedState.position != selected->logicalGeometry.topLeft()
        || selectedState.current.pixels != selected->nativePixels
        || !VirtualResize::sameScale(selectedState.scale, selected->scale)
        || (selectedState.current.id != originalState.current.id && selectedState.current.id != appliedMode.id)
        || std::none_of(selectedState.modes.cbegin(), selectedState.modes.cend(), [&originalState](const auto &mode) {
            return mode.id == originalState.current.id && mode.pixels == originalState.current.pixels
                && mode.refresh == originalState.current.refresh;
        })) return {};
    if (selectedState.current.id != originalState.current.id
        || !VirtualResize::sameScale(selectedState.scale, originalState.scale)) {
        result = VirtualResize::select(selectedState, originalState.current, originalState.scale);
    }
    if (!restorePositions.isEmpty()) {
        const auto positions = RetainedKScreenReadback::positionArguments(current, restorePositions);
        if (!positions) return {};
        result.append(*positions);
    }
    return result;
}
}
