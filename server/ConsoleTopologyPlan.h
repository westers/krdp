// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RemoteTopologyDraft.h"
#include "RetainedMultiPrimaryPlan.h"
#include "ConsoleResize.h"

#include <QMap>
#include <QSet>

namespace KRdp::ConsoleTopologyPlan
{
using Priorities = RetainedMultiPrimaryPlan::Priorities;
using Snapshot = RemoteTopologyCatalog::Snapshot;
using Operation = RemoteTopologyDraft::Operation;

// Physical mode inventory is read in the selected Console worker. The
// retained/virtual resize planner cannot authorize a physical compositor.
struct Mode {
    QString id;
    QSize pixels;
    int refresh = 0; // mHz
    bool operator==(const Mode &) const = default;
};
struct OutputState {
    QString name;
    QPoint position;
    double scale = 1;
    Mode current;
    QList<Mode> modes;
};

inline bool sameScale(double a, double b)
{
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) < 0.000001;
}

inline bool validPhysicalRequest(QSize pixels, double scale)
{
    return pixels.width() >= 320 && pixels.height() >= 200
        && pixels.width() <= 4096 && pixels.height() <= 4096
        && std::isfinite(scale) && scale >= 1 && scale <= 4;
}

struct Plan {
    Snapshot before;
    QVector<RemoteTopologyCatalog::Entry> after;
    Priorities beforePriorities;
    Priorities afterPriorities;
    QMap<QString, QPoint> positions;
    QMap<QString, QPair<QSize, double>> modes;
    bool changed = false;
};

// Pure preflight for the existing physical outputs of one captured console.
// It does not grant authority, execute KScreen, or advertise write support.
// The caller must bind the request to the authenticated controller and the
// exact captured generation/revision, then independently verify every output
// after any mutation. New virtual outputs have a separate creator lifetime.
inline std::optional<Plan> make(const Snapshot &before, const Priorities &priorities,
    const RemoteTopologyDraft::Request &request, const RemoteTopologyDraft::Capabilities &limits)
{
    if (before.revision == 0 || before.outputs.isEmpty() || before.outputs.size() > 16
        || priorities.size() != before.outputs.size() || request.operations.isEmpty()
        || request.operations.size() > 16) return {};
    QSet<QString> names;
    QSet<int> priorityValues;
    for (const auto &entry : before.outputs) {
        const auto &output = entry.output;
        if (entry.id.isEmpty() || !output.physical || !output.owner.isEmpty() || !output.enabled
            || !RetainedKScreenReadback::outputName(output.backendKey) || names.contains(output.backendKey)
            || !priorities.contains(output.backendKey) || priorities.value(output.backendKey) < 1
            || priorities.value(output.backendKey) > 16
            || priorityValues.contains(priorities.value(output.backendKey))
            || output.primary != (priorities.value(output.backendKey) == 1)) return {};
        names.insert(output.backendKey);
        priorityValues.insert(priorities.value(output.backendKey));
    }
    if (priorityValues.size() != before.outputs.size() || !priorityValues.contains(1)) return {};
    auto caps = limits;
    caps.addVirtual = false;
    caps.removeVirtual = false;
    caps.moveVirtual = false;
    caps.resizeVirtual = false;
    caps.changePhysical = true;
    const auto preview = RemoteTopologyDraft::preview(before, caps, request);
    if (!preview.valid() || preview.after.size() != before.outputs.size()) return {};

    Plan plan{before, preview.after, priorities, priorities, {}, {}, preview.before != preview.after};
    QSet<QString> moved;
    QSet<QString> resized;
    QString selectedPrimary;
    for (const auto &operation : request.operations) {
        const auto old = std::find_if(before.outputs.cbegin(), before.outputs.cend(), [&operation](const auto &entry) {
            return entry.id == operation.id;
        });
        if (old == before.outputs.cend()) return {};
        const QString backend = old->output.backendKey;
        switch (operation.kind) {
        case Operation::Kind::Move:
            if (moved.contains(backend)) return {};
            moved.insert(backend);
            if (operation.position != old->output.logicalGeometry.topLeft())
                plan.positions.insert(backend, operation.position);
            break;
        case Operation::Kind::Resize:
            if (resized.contains(backend) || !validPhysicalRequest(operation.pixels, operation.scale)) return {};
            resized.insert(backend);
            if (operation.pixels != old->output.nativePixels || !sameScale(operation.scale, old->output.scale))
                plan.modes.insert(backend, {operation.pixels, operation.scale});
            break;
        case Operation::Kind::SetPrimary:
            if (!selectedPrimary.isEmpty()) return {};
            selectedPrimary = backend;
            break;
        case Operation::Kind::AddVirtual:
        case Operation::Kind::Remove:
            return {};
        }
    }
    if (!selectedPrimary.isEmpty() && priorities.value(selectedPrimary) != 1) {
        QVector<QPair<int, QString>> peers;
        for (auto it = priorities.cbegin(); it != priorities.cend(); ++it)
            if (it.key() != selectedPrimary) peers.append({it.value(), it.key()});
        std::sort(peers.begin(), peers.end());
        plan.afterPriorities.clear();
        plan.afterPriorities.insert(selectedPrimary, 1);
        for (qsizetype i = 0; i < peers.size(); ++i) plan.afterPriorities.insert(peers[i].second, int(i) + 2);
    }
    // The public draft is keyed by stable IDs, but execution must address
    // precisely the backend names that were captured in this generation.
    for (qsizetype i = 0; i < before.outputs.size(); ++i) {
        if (plan.after[i].id != before.outputs[i].id
            || plan.after[i].output.backendKey != before.outputs[i].output.backendKey
            || !plan.after[i].output.physical || !plan.after[i].output.owner.isEmpty()) return {};
    }
    return plan;
}

inline bool matches(const Plan &plan, const Snapshot &readback, const Priorities &priorities)
{
    return readback.generation == plan.before.generation
        && readback.outputs == plan.after && priorities == plan.afterPriorities;
}

// Build one shell-free KScreen invocation from the *fresh* per-output mode
// inventory. This prepares an executor; it does not execute or claim that the
// compositor accepted any field. Readback and decoded captures remain required.
inline std::optional<QStringList> arguments(const Plan &plan,
    const QMap<QString, OutputState> &states,
    const Priorities &priorities,
    const QMap<QString, Mode> &selectedModes)
{
    if (states.size() != plan.before.outputs.size() || priorities != plan.beforePriorities
        || selectedModes.size() != plan.modes.size()) return {};
    for (const auto &entry : plan.before.outputs) {
        const auto &old = entry.output;
        if (!states.contains(old.backendKey)) return {};
        const auto &state = states.value(old.backendKey);
        if (state.name != old.backendKey || state.position != old.logicalGeometry.topLeft()
            || state.current.pixels != old.nativePixels || !sameScale(state.scale, old.scale)
            || !ConsoleResize::safeToken(state.current.id)
            || std::none_of(state.modes.cbegin(), state.modes.cend(), [&state](const auto &mode) {
                return mode == state.current;
            })) return {};
    }
    QStringList args;
    for (auto it = plan.modes.cbegin(); it != plan.modes.cend(); ++it) {
        if (!states.contains(it.key()) || !selectedModes.contains(it.key())) return {};
        const auto &state = states.value(it.key());
        const auto &selected = selectedModes.value(it.key());
        const auto old = std::find_if(plan.before.outputs.cbegin(), plan.before.outputs.cend(), [&it](const auto &entry) {
            return entry.output.backendKey == it.key();
        });
        if (old == plan.before.outputs.cend() || selected.pixels != it.value().first
            || !ConsoleResize::safeToken(selected.id)
            || !validPhysicalRequest(selected.pixels, it.value().second)
            || std::none_of(state.modes.cbegin(), state.modes.cend(), [&selected](const auto &candidate) {
                return candidate == selected;
            })) return {};
        const QString prefix = QStringLiteral("output.%1.").arg(it.key());
        args.append(prefix + QStringLiteral("mode.") + selected.id);
        args.append(prefix + QStringLiteral("scale.") + QString::number(it.value().second, 'g', 12));
    }
    if (!plan.positions.isEmpty()) {
        RetainedKScreenReadback::Snapshot before;
        for (const auto &entry : plan.before.outputs) before.outputs.append(entry.output);
        QVector<RetainedKScreenReadback::Placement> placements;
        for (auto it = plan.positions.cbegin(); it != plan.positions.cend(); ++it)
            placements.append({it.key(), it.value()});
        const auto positions = RetainedKScreenReadback::positionArguments(before, placements);
        if (!positions) return {};
        args.append(*positions);
    }
    if (plan.afterPriorities != plan.beforePriorities)
        args.append(RetainedMultiPrimaryPlan::arguments(plan.afterPriorities));
    if (plan.changed == args.isEmpty()) return {};
    return args;
}

// A conditional restore must not overwrite an unrelated KDE edit. This is
// only the field-level gate; the worker must additionally verify original
// mode IDs still exist before producing rollback arguments and must read back
// the complete restored layout and fresh captures afterward.
inline bool recognizedPartial(const Plan &plan, const Snapshot &current, const Priorities &priorities)
{
    if (current.generation != plan.before.generation || current.outputs.size() != plan.before.outputs.size()
        || priorities.size() != plan.beforePriorities.size()) return false;
    for (qsizetype i = 0; i < current.outputs.size(); ++i) {
        const auto &now = current.outputs[i];
        const auto &old = plan.before.outputs[i];
        const auto &target = plan.after[i];
        const auto &a = now.output;
        const auto &b = old.output;
        const auto &c = target.output;
        if (now.id != old.id || target.id != old.id || a.backendKey != b.backendKey
            || a.name != b.name || !a.physical || !a.owner.isEmpty() || !a.enabled
            || !priorities.contains(a.backendKey)
            || (a.nativePixels != b.nativePixels && a.nativePixels != c.nativePixels)
            || (!sameScale(a.scale, b.scale) && !sameScale(a.scale, c.scale))
            || (a.logicalGeometry.topLeft() != b.logicalGeometry.topLeft()
                && a.logicalGeometry.topLeft() != c.logicalGeometry.topLeft())
            || a.logicalGeometry.size() != RemoteMonitorGeometry::logicalSize(a.nativePixels, a.scale)
            || (priorities.value(a.backendKey) != plan.beforePriorities.value(a.backendKey)
                && priorities.value(a.backendKey) != plan.afterPriorities.value(a.backendKey))
            || a.primary != (priorities.value(a.backendKey) == 1)) return false;
    }
    return true;
}
}
