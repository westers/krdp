// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RemoteTopologyDraft.h"
#include "RetainedKScreenReadback.h"
#include "RetainedMultiPrimaryPlan.h"
#include "VirtualResize.h"

#include <QMap>
#include <QSet>

namespace KRdp::RetainedMultiMixedPlan
{
using Operation = RemoteTopologyDraft::Operation;
struct Plan {
    RetainedKScreenReadback::Snapshot before;
    QVector<RemoteTopologyCatalog::Output> after;
    QVector<RetainedKScreenReadback::Placement> placements;
    QMap<QString, QPair<QSize, double>> resizes;
    RetainedMultiPrimaryPlan::Priorities originalPriorities;
    RetainedMultiPrimaryPlan::Priorities requestedPriorities;
    bool changed = false;
};

// This is a pure, existing-output worker preflight. Broker authorization,
// generation/revision binding, capture restart and conditional recovery remain
// the responsibility of the transaction caller. Add/Remove need a separate
// creator-lifecycle phase and are intentionally rejected here.
inline std::optional<Plan> make(const RetainedKScreenReadback::Snapshot &before, const QString &owner,
    const RetainedMultiPrimaryPlan::Priorities &priorities, const QVector<Operation> &operations)
{
    if (owner.isEmpty() || before.outputs.size() < 2 || before.outputs.size() > 16
        || operations.size() < 2 || operations.size() > 16 || priorities.size() != before.outputs.size()) return {};
    RemoteTopologyCatalog::Snapshot snapshot;
    snapshot.generation = QStringLiteral("mixed-worker-preflight");
    snapshot.revision = 1;
    QSet<int> usedPriorities;
    for (const auto &output : before.outputs) {
        if (output.physical || output.owner != owner || !priorities.contains(output.backendKey)
            || priorities.value(output.backendKey) < 1 || priorities.value(output.backendKey) > 16
            || usedPriorities.contains(priorities.value(output.backendKey))
            || output.primary != (priorities.value(output.backendKey) == 1)) return {};
        usedPriorities.insert(priorities.value(output.backendKey));
        snapshot.outputs.append({output.backendKey, output});
    }
    RemoteTopologyDraft::Request request;
    request.generation = snapshot.generation;
    request.expectedRevision = snapshot.revision;
    request.owner = owner;
    request.operations = operations;
    const RemoteTopologyDraft::Capabilities caps{.moveVirtual = true, .resizeVirtual = true,
        .changePrimary = true, .maxOutputs = 16, .maxOutputDimension = 4096, .maxAtlasDimension = 8192};
    QSet<QString> moves;
    QSet<QString> resizes;
    QString primary;
    Plan plan;
    plan.before = before;
    plan.originalPriorities = priorities;
    for (const auto &operation : operations) {
        if (!RetainedKScreenReadback::outputName(operation.id)) return {};
        const auto old = std::find_if(before.outputs.cbegin(), before.outputs.cend(), [&operation](const auto &output) {
            return output.backendKey == operation.id;
        });
        if (old == before.outputs.cend()) return {};
        switch (operation.kind) {
        case Operation::Kind::Move:
            if (moves.contains(operation.id) || operation.position.x() < 0 || operation.position.y() < 0) return {};
            moves.insert(operation.id);
            if (operation.position != old->logicalGeometry.topLeft())
                plan.placements.append({operation.id, operation.position});
            break;
        case Operation::Kind::Resize:
            if (resizes.contains(operation.id) || !VirtualResize::validRequest(operation.pixels, operation.scale)) return {};
            resizes.insert(operation.id);
            if (operation.pixels != old->nativePixels || !VirtualResize::sameScale(operation.scale, old->scale))
                plan.resizes.insert(operation.id, {operation.pixels, operation.scale});
            break;
        case Operation::Kind::SetPrimary:
            if (!primary.isEmpty()) return {};
            primary = operation.id;
            break;
        case Operation::Kind::AddVirtual:
        case Operation::Kind::Remove:
            return {};
        }
    }
    const auto preview = RemoteTopologyDraft::preview(snapshot, caps, request);
    if (!preview.valid()) return {};
    if (!plan.placements.isEmpty() && !RetainedKScreenReadback::positionArguments(before, plan.placements)) return {};
    for (const auto &entry : preview.after) {
        if (entry.output.logicalGeometry.x() < 0 || entry.output.logicalGeometry.y() < 0) return {};
        plan.after.append(entry.output);
    }
    plan.requestedPriorities = priorities;
    if (!primary.isEmpty()) {
        const auto selected = RetainedMultiPrimaryPlan::make(before, owner, primary, priorities);
        if (!selected) return {};
        if (selected->changed) plan.requestedPriorities = selected->requested;
    }
    plan.changed = preview.before != preview.after;
    return plan;
}

inline bool matches(const Plan &plan, const RetainedKScreenReadback::Snapshot &readback,
    const RetainedMultiPrimaryPlan::Priorities &priorities)
{
    return readback.outputs == plan.after && priorities == plan.requestedPriorities;
}

// The caller must obtain each target's fresh mode inventory from the same
// KScreen readback, register missing modes without changing the active layout,
// then pass exact selected modes here. Nothing is executed by this helper.
inline std::optional<QStringList> arguments(const Plan &plan,
    const QMap<QString, VirtualResize::Snapshot> &states,
    const QMap<QString, VirtualResize::Mode> &selectedModes)
{
    if (states.size() != plan.resizes.size() || selectedModes.size() != plan.resizes.size()) return {};
    QStringList args;
    for (auto it = plan.resizes.cbegin(); it != plan.resizes.cend(); ++it) {
        if (!states.contains(it.key()) || !selectedModes.contains(it.key())) return {};
        const auto &state = states.value(it.key());
        const auto &mode = selectedModes.value(it.key());
        const auto old = std::find_if(plan.before.outputs.cbegin(), plan.before.outputs.cend(), [&it](const auto &output) {
            return output.backendKey == it.key();
        });
        if (old == plan.before.outputs.cend() || state.name != it.key()
            || state.position != old->logicalGeometry.topLeft() || state.current.pixels != old->nativePixels
            || !VirtualResize::sameScale(state.scale, old->scale) || mode.pixels != it.value().first
            || std::none_of(state.modes.cbegin(), state.modes.cend(), [&mode](const auto &candidate) {
                return candidate.id == mode.id && candidate.pixels == mode.pixels && candidate.refresh == mode.refresh;
            })) return {};
        args.append(VirtualResize::select(state, mode, it.value().second));
    }
    if (!plan.placements.isEmpty()) {
        const auto positions = RetainedKScreenReadback::positionArguments(plan.before, plan.placements);
        if (!positions) return {};
        args.append(*positions);
    }
    if (plan.requestedPriorities != plan.originalPriorities)
        args.append(RetainedMultiPrimaryPlan::arguments(plan.requestedPriorities));
    if (plan.changed && args.isEmpty()) return {};
    if (!plan.changed && !args.isEmpty()) return {};
    return args;
}

// A failed multi-field KScreen command may have applied only some arguments.
// Restore only when every observed field is one of this plan's before/after
// values and every original mode is still present. The worker must independently
// verify the entire original catalog and mode IDs after running these args.
inline std::optional<QStringList> recoveryArguments(const Plan &plan,
    const RetainedKScreenReadback::Snapshot &current,
    const RetainedMultiPrimaryPlan::Priorities &currentPriorities,
    const QMap<QString, VirtualResize::Snapshot> &currentStates,
    const QMap<QString, VirtualResize::Snapshot> &originalStates,
    const QMap<QString, VirtualResize::Mode> &appliedModes)
{
    if (current.outputs.size() != plan.before.outputs.size() || current.outputs.size() != plan.after.size()
        || currentPriorities.size() != plan.originalPriorities.size()
        || currentStates.size() != plan.resizes.size() || originalStates.size() != plan.resizes.size()
        || appliedModes.size() != plan.resizes.size()) return {};
    QStringList args;
    QVector<RetainedKScreenReadback::Placement> restorePositions;
    for (qsizetype i = 0; i < current.outputs.size(); ++i) {
        const auto &now = current.outputs[i];
        const auto &before = plan.before.outputs[i];
        const auto &after = plan.after[i];
        if (now.backendKey != before.backendKey || after.backendKey != before.backendKey
            || now.name != before.name || now.owner != before.owner || now.enabled != before.enabled
            || now.physical != before.physical || !currentPriorities.contains(now.backendKey)
            || (now.nativePixels != before.nativePixels && now.nativePixels != after.nativePixels)
            || (!VirtualResize::sameScale(now.scale, before.scale)
                && !VirtualResize::sameScale(now.scale, after.scale))
            || (now.primary != before.primary && now.primary != after.primary)
            || (currentPriorities.value(now.backendKey) != plan.originalPriorities.value(now.backendKey)
                && currentPriorities.value(now.backendKey) != plan.requestedPriorities.value(now.backendKey))
            || now.primary != (currentPriorities.value(now.backendKey) == 1)) return {};
        const auto position = now.logicalGeometry.topLeft();
        if ((position != before.logicalGeometry.topLeft() && position != after.logicalGeometry.topLeft())
            || now.logicalGeometry.size() != RemoteMonitorGeometry::logicalSize(now.nativePixels, now.scale)) return {};
        if (position != before.logicalGeometry.topLeft())
            restorePositions.append({now.backendKey, before.logicalGeometry.topLeft()});
    }
    for (auto it = plan.resizes.cbegin(); it != plan.resizes.cend(); ++it) {
        if (!currentStates.contains(it.key()) || !originalStates.contains(it.key())
            || !appliedModes.contains(it.key())) return {};
        const auto &now = currentStates.value(it.key());
        const auto &original = originalStates.value(it.key());
        const auto &applied = appliedModes.value(it.key());
        const auto output = std::find_if(current.outputs.cbegin(), current.outputs.cend(), [&it](const auto &candidate) {
            return candidate.backendKey == it.key();
        });
        if (output == current.outputs.cend() || now.name != it.key() || original.name != it.key()
            || now.id != original.id || now.position != output->logicalGeometry.topLeft()
            || now.current.pixels != output->nativePixels || !VirtualResize::sameScale(now.scale, output->scale)
            || (now.current.id != original.current.id && now.current.id != applied.id)
            || std::none_of(now.modes.cbegin(), now.modes.cend(), [&original](const auto &candidate) {
                return candidate.id == original.current.id && candidate.pixels == original.current.pixels
                    && candidate.refresh == original.current.refresh;
            })) return {};
        if (now.current.id != original.current.id || !VirtualResize::sameScale(now.scale, original.scale))
            args.append(VirtualResize::select(now, original.current, original.scale));
    }
    if (!restorePositions.isEmpty()) {
        const auto positions = RetainedKScreenReadback::positionArguments(current, restorePositions);
        if (!positions) return {};
        args.append(*positions);
    }
    if (currentPriorities != plan.originalPriorities)
        args.append(RetainedMultiPrimaryPlan::arguments(plan.originalPriorities));
    return args;
}
}
