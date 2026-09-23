// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RetainedMultiMixedPlan.h"

#include <QSet>

namespace KRdp::RetainedMultiMixedCreatePlan
{
using Operation = RemoteTopologyDraft::Operation;

struct Plan {
    RetainedKScreenReadback::Snapshot before;
    QString newBackendKey;
    QString temporaryId;
    QSize newPixels;
    double newScale = 1;
    QPoint newPosition;
    QVector<Operation> operations;
    QVector<RemoteTopologyCatalog::Output> after;
};

// Pure first-stage preflight. The caller must still create the output under
// its own KWin creator session, freeze capture/input, obtain a fresh KScreen
// readback, and then use afterCreation() for the single mixed KScreen apply.
// No broker capability follows from this plan alone.
inline std::optional<Plan> make(const RetainedKScreenReadback::Snapshot &before, const QString &owner,
    const RetainedMultiPrimaryPlan::Priorities &priorities, const QString &newBackendKey,
    const QVector<Operation> &operations)
{
    if (owner.isEmpty() || !newBackendKey.startsWith(QStringLiteral("Virtual-krdp-added-"))
        || !RetainedKScreenReadback::outputName(newBackendKey)
        || before.outputs.size() < 2 || before.outputs.size() >= 16 || priorities.size() != before.outputs.size()
        || operations.size() < 2 || operations.size() > 16) return {};
    RemoteTopologyCatalog::Snapshot snapshot;
    snapshot.generation = QStringLiteral("mixed-create-preflight");
    snapshot.revision = 1;
    QSet<int> usedPriorities;
    for (const auto &output : before.outputs) {
        if (output.physical || output.owner != owner || output.backendKey == newBackendKey
            || !priorities.contains(output.backendKey) || priorities.value(output.backendKey) < 1
            || priorities.value(output.backendKey) > 16 || usedPriorities.contains(priorities.value(output.backendKey))
            || output.primary != (priorities.value(output.backendKey) == 1)) return {};
        usedPriorities.insert(priorities.value(output.backendKey));
        snapshot.outputs.append({output.backendKey, output});
    }
    QString temporaryId;
    QSet<QString> operationKeys;
    int primaryCount = 0;
    for (const auto &operation : operations) {
        if (operation.kind == Operation::Kind::Remove) return {};
        if (operation.kind == Operation::Kind::AddVirtual) {
            if (!temporaryId.isEmpty() || !operation.id.startsWith(QStringLiteral("new:"))
                || operation.id.size() <= 4 || !VirtualResize::validRequest(operation.pixels, operation.scale)
                || operation.position.x() < 0 || operation.position.y() < 0) return {};
            temporaryId = operation.id;
        } else if (operation.kind == Operation::Kind::Resize
            && !VirtualResize::validRequest(operation.pixels, operation.scale)) return {};
        else if (operation.kind == Operation::Kind::Move
            && (operation.position.x() < 0 || operation.position.y() < 0)) return {};
        if (operation.kind == Operation::Kind::SetPrimary && ++primaryCount > 1) return {};
        const QString key = QString::number(int(operation.kind)) + QLatin1Char(':') + operation.id;
        if (operationKeys.contains(key)) return {};
        operationKeys.insert(key);
    }
    if (temporaryId.isEmpty()) return {};
    RemoteTopologyDraft::Request request;
    request.generation = snapshot.generation;
    request.expectedRevision = snapshot.revision;
    request.owner = owner;
    request.operations = operations;
    const RemoteTopologyDraft::Capabilities caps{.addVirtual = true, .moveVirtual = true,
        .resizeVirtual = true, .changePrimary = true, .maxOutputs = 16,
        .maxOutputDimension = 4096, .maxAtlasDimension = 8192};
    const auto preview = RemoteTopologyDraft::preview(snapshot, caps, request);
    if (!preview.valid() || preview.after.size() != before.outputs.size() + 1) return {};
    const auto newEntry = std::find_if(preview.after.cbegin(), preview.after.cend(), [&temporaryId](const auto &entry) {
        return entry.id == temporaryId;
    });
    if (newEntry == preview.after.cend()) return {};
    Plan plan;
    plan.before = before;
    plan.newBackendKey = newBackendKey;
    plan.temporaryId = temporaryId;
    plan.newPixels = newEntry->output.nativePixels;
    plan.newScale = newEntry->output.scale;
    plan.newPosition = newEntry->output.logicalGeometry.topLeft();
    plan.operations = operations;
    for (const auto &entry : preview.after) {
        auto output = entry.output;
        if (entry.id == temporaryId) {
            output.backendKey = newBackendKey;
            output.name = newBackendKey;
        }
        plan.after.append(output);
    }
    std::sort(plan.after.begin(), plan.after.end(), [](const auto &a, const auto &b) {
        return a.backendKey < b.backendKey;
    });
    return plan;
}

// KWin may initially place the new output somewhere other than its final
// requested position. That is acceptable only while capture/input are frozen;
// all older outputs and every non-position field of the creator-owned output
// must match independently read-back state exactly.
inline bool matchesCreated(const Plan &plan, const RetainedKScreenReadback::Snapshot &created)
{
    if (created.outputs.size() != plan.before.outputs.size() + 1) return false;
    for (const auto &old : plan.before.outputs) {
        const auto found = std::find_if(created.outputs.cbegin(), created.outputs.cend(), [&old](const auto &output) {
            return output.backendKey == old.backendKey;
        });
        if (found == created.outputs.cend() || *found != old) return false;
    }
    const auto found = std::find_if(created.outputs.cbegin(), created.outputs.cend(), [&plan](const auto &output) {
        return output.backendKey == plan.newBackendKey;
    });
    if (found == created.outputs.cend()) return false;
    const auto &output = *found;
    return output.name == plan.newBackendKey && output.owner == plan.before.outputs.first().owner
        && !output.physical && output.enabled && !output.primary && output.nativePixels == plan.newPixels
        && VirtualResize::sameScale(output.scale, plan.newScale)
        && output.logicalGeometry.size() == RemoteMonitorGeometry::logicalSize(output.nativePixels, output.scale)
        && output.logicalGeometry.x() >= 0 && output.logicalGeometry.y() >= 0;
}

// Convert the one creator operation into a move of the now-existing output.
// The existing mixed planner validates the entire final layout and builds one
// mode/scale/position/primary KScreen argument list against this fresh state.
inline std::optional<RetainedMultiMixedPlan::Plan> afterCreation(const Plan &plan,
    const RetainedKScreenReadback::Snapshot &created,
    const RetainedMultiPrimaryPlan::Priorities &createdPriorities)
{
    if (!matchesCreated(plan, created)) return {};
    QVector<Operation> remaining;
    for (auto operation : plan.operations) {
        if (operation.id == plan.temporaryId) operation.id = plan.newBackendKey;
        if (operation.kind == Operation::Kind::AddVirtual) {
            operation.kind = Operation::Kind::Move;
            operation.position = plan.newPosition;
            operation.pixels = {};
            operation.scale = 1;
        }
        remaining.append(operation);
    }
    const auto mixed = RetainedMultiMixedPlan::make(created, plan.before.outputs.first().owner,
        createdPriorities, remaining);
    if (!mixed || mixed->after != plan.after) return {};
    return mixed;
}
}
