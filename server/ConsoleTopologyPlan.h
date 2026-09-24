// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RemoteTopologyDraft.h"
#include "RetainedMultiPrimaryPlan.h"
#include "ConsoleResize.h"
#include "ConsoleWorkerWire.h"

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
struct Inventory {
    QMap<QString, OutputState> states;
    Priorities priorities;
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

// Translate the authenticated v9 worker record to the same complete draft
// preflight used by the broker. Names are local worker lookup keys here;
// generation-scoped public IDs remain exclusively broker-owned.
inline std::optional<Plan> fromWire(const ConsoleWorkerWire::PhysicalLayout &wire)
{
    if (!wire.requestId || !wire.controlGeneration || wire.catalogGeneration.isEmpty()
        || !wire.expectedRevision || !wire.allowPhysicalChange || wire.before.isEmpty()
        || wire.before.size() > 16 || wire.operations.isEmpty() || wire.operations.size() > 16) return {};
    Snapshot snapshot;
    snapshot.generation = wire.catalogGeneration;
    snapshot.revision = wire.expectedRevision;
    Priorities priorities;
    QSet<QString> names;
    for (const auto &physical : wire.before) {
        if (!RetainedKScreenReadback::outputName(physical.name) || physical.name.startsWith(QStringLiteral("Virtual-"))
            || names.contains(physical.name) || !validPhysicalRequest(physical.pixels, physical.scale)
            || physical.logical != RemoteMonitorGeometry::logicalRect(physical.logical.topLeft(),
                physical.pixels, physical.scale)
            || physical.priority < 1 || physical.priority > 16
            || physical.primary != (physical.priority == 1)) return {};
        names.insert(physical.name);
        priorities.insert(physical.name, physical.priority);
        snapshot.outputs.append({physical.name, {
            .backendKey = physical.name, .name = physical.name, .nativePixels = physical.pixels,
            .logicalGeometry = physical.logical, .scale = physical.scale, .enabled = true,
            .primary = physical.primary, .physical = true, .owner = {},
        }});
    }
    std::sort(snapshot.outputs.begin(), snapshot.outputs.end(), [](const auto &a, const auto &b) {
        return a.output.backendKey < b.output.backendKey;
    });
    RemoteTopologyDraft::Request draft;
    draft.generation = wire.catalogGeneration;
    draft.expectedRevision = wire.expectedRevision;
    draft.owner = QStringLiteral("authenticated-physical-controller");
    draft.allowPhysicalChange = true;
    for (const auto &change : wire.operations) {
        if (!names.contains(change.output)) return {};
        Operation operation;
        operation.id = change.output;
        switch (change.kind) {
        case ConsoleWorkerWire::MixedOperation::Kind::Move:
            operation.kind = Operation::Kind::Move;
            operation.position = change.globalLogical;
            break;
        case ConsoleWorkerWire::MixedOperation::Kind::Resize:
            operation.kind = Operation::Kind::Resize;
            operation.pixels = change.pixels;
            operation.scale = change.scale;
            break;
        case ConsoleWorkerWire::MixedOperation::Kind::Primary:
            operation.kind = Operation::Kind::SetPrimary;
            break;
        }
        draft.operations.append(operation);
    }
    return make(snapshot, priorities, draft, {.changePrimary = true, .maxOutputs = 16,
        .maxOutputDimension = 4096, .maxAtlasDimension = 8192});
}

inline bool matches(const Plan &plan, const Snapshot &readback, const Priorities &priorities)
{
    return readback.generation == plan.before.generation
        && readback.outputs == plan.after && priorities == plan.afterPriorities;
}

// Parse the complete *fresh* physical worker readback, not a broker-cached
// catalog or a single target's mode list. A changed peer invalidates the
// entire candidate command before KScreen sees any mutation.
inline std::optional<Inventory> inventory(const Plan &plan, const QByteArray &json)
{
    const auto readback = RetainedKScreenReadback::parse(json, QStringLiteral("console-mode-inventory"));
    if (!readback || readback->outputs.size() != plan.before.outputs.size()) return {};
    const auto freshPriorities = RetainedMultiPrimaryPlan::priorities(json, *readback);
    if (!freshPriorities || *freshPriorities != plan.beforePriorities) return {};
    for (qsizetype i = 0; i < readback->outputs.size(); ++i) {
        const auto &fresh = readback->outputs[i];
        const auto &old = plan.before.outputs[i].output;
        if (fresh.backendKey != old.backendKey || fresh.name != old.name
            || fresh.nativePixels != old.nativePixels || fresh.logicalGeometry != old.logicalGeometry
            || !sameScale(fresh.scale, old.scale) || fresh.primary != old.primary) return {};
    }
    const auto document = QJsonDocument::fromJson(json);
    Inventory result;
    result.priorities = *freshPriorities;
    for (const auto &value : document.object().value(QStringLiteral("outputs")).toArray()) {
        const auto object = value.toObject();
        OutputState state;
        state.name = object.value(QStringLiteral("name")).toString();
        const auto position = object.value(QStringLiteral("pos")).toObject();
        state.position = QPoint(position.value(QStringLiteral("x")).toInt(), position.value(QStringLiteral("y")).toInt());
        state.scale = object.value(QStringLiteral("scale")).toDouble();
        const QString currentId = object.value(QStringLiteral("currentModeId")).toString();
        QSet<QString> modeIds;
        for (const auto &modeValue : object.value(QStringLiteral("modes")).toArray()) {
            const auto modeObject = modeValue.toObject();
            const auto size = modeObject.value(QStringLiteral("size")).toObject();
            const auto refresh = modeObject.value(QStringLiteral("refreshRate"));
            Mode mode{modeObject.value(QStringLiteral("id")).toString(),
                QSize(size.value(QStringLiteral("width")).toInt(), size.value(QStringLiteral("height")).toInt()), 0};
            if (!ConsoleResize::safeToken(mode.id) || modeIds.contains(mode.id)
                || !refresh.isDouble() || !std::isfinite(refresh.toDouble())
                || refresh.toDouble() < 0.001 || refresh.toDouble() > 1000) return {};
            mode.refresh = qRound(refresh.toDouble() * 1000.0);
            if (mode.refresh < 1) return {};
            modeIds.insert(mode.id);
            state.modes.append(mode);
            if (mode.id == currentId) state.current = mode;
        }
        if (state.current.id.isEmpty() || result.states.contains(state.name)) return {};
        result.states.insert(state.name, state);
    }
    return result.states.size() == plan.before.outputs.size() ? std::optional(result) : std::nullopt;
}

// Preserve the existing refresh rate when available; otherwise choose the
// closest advertised physical mode, with a stable ID tie-break.
inline std::optional<QMap<QString, Mode>> selectedModes(const Plan &plan, const Inventory &fresh)
{
    QMap<QString, Mode> selected;
    for (auto it = plan.modes.cbegin(); it != plan.modes.cend(); ++it) {
        if (!fresh.states.contains(it.key())) return {};
        const auto &state = fresh.states.value(it.key());
        std::optional<Mode> best;
        for (const auto &candidate : state.modes) {
            if (candidate.pixels != it.value().first) continue;
            if (!best || std::abs(candidate.refresh - state.current.refresh) < std::abs(best->refresh - state.current.refresh)
                || (std::abs(candidate.refresh - state.current.refresh) == std::abs(best->refresh - state.current.refresh)
                    && candidate.id < best->id)) best = candidate;
        }
        if (!best) return {};
        selected.insert(it.key(), *best);
    }
    return selected;
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

inline std::optional<QStringList> arguments(const Plan &plan, const QByteArray &freshKScreenJson)
{
    const auto fresh = inventory(plan, freshKScreenJson);
    if (!fresh) return {};
    const auto selected = selectedModes(plan, *fresh);
    return selected ? arguments(plan, fresh->states, fresh->priorities, *selected) : std::nullopt;
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

// Build a compensating command from a *new* compositor readback. This is not
// an unconditional rollback: a field that KDE or another controller changed
// to a third value is left alone. The projected complete layout must still
// have distinct priorities and non-overlapping logical output rectangles.
// The worker must verify the resulting KScreen state and fresh captures before
// reporting recovery; an empty list means there is nothing of ours to undo.
inline std::optional<QStringList> recoveryArguments(const Plan &plan, const Inventory &before,
    const QMap<QString, Mode> &selected, const QByteArray &freshKScreenJson)
{
    if (!arguments(plan, before.states, before.priorities, selected)) return {};
    const auto parsed = RetainedKScreenReadback::parse(freshKScreenJson, QStringLiteral("console-recovery"));
    if (!parsed || parsed->outputs.size() != plan.before.outputs.size()) return {};
    const auto currentPriorities = RetainedMultiPrimaryPlan::priorities(freshKScreenJson, *parsed);
    if (!currentPriorities || currentPriorities->size() != plan.before.outputs.size()) return {};
    Plan observedPlan = plan;
    observedPlan.before.outputs.clear();
    for (const auto &output : parsed->outputs) {
        if (!before.states.contains(output.backendKey)) return {}; // hotplug or replacement
        auto physical = output;
        physical.physical = true;
        physical.owner.clear();
        const auto old = std::find_if(plan.before.outputs.cbegin(), plan.before.outputs.cend(),
            [&physical](const auto &entry) { return entry.output.backendKey == physical.backendKey; });
        if (old == plan.before.outputs.cend() || old->output.name != physical.name) return {};
        observedPlan.before.outputs.append({old->id, physical});
    }
    observedPlan.beforePriorities = *currentPriorities;
    const auto current = inventory(observedPlan, freshKScreenJson);
    if (!current || current->states.size() != before.states.size()) return {};

    auto projected = current->states;
    auto projectedPriorities = current->priorities;
    QStringList args;
    for (auto it = before.states.cbegin(); it != before.states.cend(); ++it) {
        const QString &name = it.key();
        if (!current->states.contains(name)) return {};
        const auto &old = it.value();
        const auto &now = current->states.value(name);
        auto &next = projected[name];
        const QString prefix = QStringLiteral("output.%1.").arg(name);
        if (selected.contains(name)) {
            const auto &target = selected.value(name);
            // Equal pixels with a different mode ID can be an independent
            // refresh-rate edit. Never infer ownership from size alone.
            if (now.current == target && now.current != old.current) {
                if (std::none_of(now.modes.cbegin(), now.modes.cend(), [&old](const Mode &mode) {
                        return mode == old.current;
                    })) return {};
                args.append(prefix + QStringLiteral("mode.") + old.current.id);
                next.current = old.current;
            }
            const double targetScale = plan.modes.value(name).second;
            if (sameScale(now.scale, targetScale) && !sameScale(targetScale, old.scale)) {
                args.append(prefix + QStringLiteral("scale.") + QString::number(old.scale, 'g', 12));
                next.scale = old.scale;
            }
        }
        if (plan.positions.contains(name) && now.position == plan.positions.value(name)
            && now.position != old.position) {
            args.append(prefix + QStringLiteral("position.%1,%2").arg(old.position.x()).arg(old.position.y()));
            next.position = old.position;
        }
        if (plan.afterPriorities.value(name) != plan.beforePriorities.value(name)
            && current->priorities.value(name) == plan.afterPriorities.value(name))
            projectedPriorities[name] = plan.beforePriorities.value(name);
    }
    if (projectedPriorities != current->priorities) {
        QSet<int> values;
        for (auto it = projectedPriorities.cbegin(); it != projectedPriorities.cend(); ++it) {
            if (it.value() < 1 || it.value() > projectedPriorities.size() || values.contains(it.value())) return {};
            values.insert(it.value());
        }
        if (!values.contains(1)) return {};
        args.append(RetainedMultiPrimaryPlan::arguments(projectedPriorities));
    }
    QVector<QRect> rectangles;
    for (const auto &state : std::as_const(projected)) {
        if (!validPhysicalRequest(state.current.pixels, state.scale)) return {};
        const QRect rect = RemoteMonitorGeometry::logicalRect(state.position, state.current.pixels, state.scale);
        if (rect.isEmpty() || std::any_of(rectangles.cbegin(), rectangles.cend(),
                [&rect](const QRect &peer) { return peer.intersects(rect); })) return {};
        rectangles.append(rect);
    }
    return args;
}
}
