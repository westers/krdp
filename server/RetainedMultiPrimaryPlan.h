// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RetainedKScreenReadback.h"
#include "RemoteTopologyDraft.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>

namespace KRdp::RetainedMultiPrimaryPlan
{
using Priorities = QMap<QString, int>;
struct Plan {
    RetainedKScreenReadback::Snapshot before;
    QVector<RemoteTopologyCatalog::Output> after;
    Priorities original;
    Priorities requested;
    bool changed = false;
};

// Read priority ordering separately from the authoritative catalog: the
// public protocol promises only which output is primary, but recovery needs
// the entire old ordering. This JSON must be the same fresh KScreen readback
// that supplied `snapshot`, never a previous query or a client value.
inline std::optional<Priorities> priorities(const QByteArray &json, const RetainedKScreenReadback::Snapshot &snapshot)
{
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isObject()) return {};
    const auto values = document.object().value(QStringLiteral("outputs")).toArray();
    if (values.size() != snapshot.outputs.size()) return {};
    Priorities result;
    QSet<int> used;
    for (const auto &value : values) {
        if (!value.isObject()) return {};
        const auto object = value.toObject();
        const auto name = object.value(QStringLiteral("name")).toString();
        const auto priority = object.value(QStringLiteral("priority"));
        if (!RetainedKScreenReadback::outputName(name) || result.contains(name)
            || !RetainedKScreenReadback::integer(priority, 1, 16)
            || used.contains(priority.toInt())) return {};
        result.insert(name, priority.toInt());
        used.insert(priority.toInt());
    }
    for (const auto &output : snapshot.outputs) {
        if (!result.contains(output.backendKey) || output.primary != (result.value(output.backendKey) == 1)) return {};
    }
    return result;
}

inline std::optional<Plan> make(const RetainedKScreenReadback::Snapshot &before, const QString &owner,
    const QString &selected, const Priorities &original)
{
    if (owner.isEmpty() || !RetainedKScreenReadback::outputName(selected)
        || before.outputs.size() < 2 || before.outputs.size() > 16 || original.size() != before.outputs.size()) return {};
    RemoteTopologyCatalog::Snapshot snapshot;
    snapshot.generation = QStringLiteral("worker-primary-preflight");
    snapshot.revision = 1;
    QSet<int> seenPriorities;
    for (const auto &output : before.outputs) {
        if (output.physical || output.owner != owner || !original.contains(output.backendKey)
            || original.value(output.backendKey) < 1 || original.value(output.backendKey) > 16
            || seenPriorities.contains(original.value(output.backendKey))
            || output.primary != (original.value(output.backendKey) == 1)) return {};
        seenPriorities.insert(original.value(output.backendKey));
        snapshot.outputs.append({output.backendKey, output});
    }
    RemoteTopologyDraft::Request request;
    request.generation = snapshot.generation;
    request.expectedRevision = snapshot.revision;
    request.owner = owner;
    RemoteTopologyDraft::Operation operation;
    operation.kind = RemoteTopologyDraft::Operation::Kind::SetPrimary;
    operation.id = selected;
    request.operations.append(operation);
    const RemoteTopologyDraft::Capabilities caps{.changePrimary = true, .maxOutputs = 16,
        .maxOutputDimension = 4096, .maxAtlasDimension = 8192};
    const auto preview = RemoteTopologyDraft::preview(snapshot, caps, request);
    if (!preview.valid()) return {};
    Plan plan;
    plan.before = before;
    plan.original = original;
    for (const auto &entry : preview.after) plan.after.append(entry.output);
    plan.changed = preview.before != preview.after;
    // Preserve existing relative order of the non-selected outputs.
    QVector<QPair<int, QString>> ordered;
    for (auto it = original.cbegin(); it != original.cend(); ++it)
        if (it.key() != selected) ordered.append({it.value(), it.key()});
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return a.first == b.first ? a.second < b.second : a.first < b.first;
    });
    plan.requested.insert(selected, 1);
    for (qsizetype i = 0; i < ordered.size(); ++i) plan.requested.insert(ordered[i].second, int(i) + 2);
    return plan;
}

inline QStringList arguments(const Priorities &priorities)
{
    QStringList result;
    for (auto it = priorities.cbegin(); it != priorities.cend(); ++it)
        result.append(QStringLiteral("output.%1.priority.%2").arg(it.key()).arg(it.value()));
    return result;
}

inline bool matches(const Plan &plan, const RetainedKScreenReadback::Snapshot &readback)
{
    return readback.outputs == plan.after;
}

// Only restore a failed priority apply when nothing outside this transaction
// changed. A fresh partial priority map may contain old or requested values,
// but all other output fields must still match the original snapshot.
inline std::optional<QStringList> recoveryArguments(const Plan &plan,
    const RetainedKScreenReadback::Snapshot &current, const Priorities &currentPriorities)
{
    if (current.outputs.size() != plan.before.outputs.size()
        || currentPriorities.size() != plan.original.size()) return {};
    for (qsizetype i = 0; i < current.outputs.size(); ++i) {
        auto now = current.outputs[i];
        const auto &before = plan.before.outputs[i];
        if (now.backendKey != before.backendKey || !currentPriorities.contains(now.backendKey)) return {};
        const int value = currentPriorities.value(now.backendKey);
        if (value != plan.original.value(now.backendKey) && value != plan.requested.value(now.backendKey)) return {};
        now.primary = before.primary;
        if (now != before) return {};
    }
    return currentPriorities == plan.original ? QStringList{} : arguments(plan.original);
}
}
