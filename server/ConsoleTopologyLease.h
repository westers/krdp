// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ConsoleTopologyPlan.h"

namespace KRdp::ConsoleTopologyLease
{
// Pure, non-authorizing state for conditional lease release. The original
// complete mode inventory is kept across successive verified transactions;
// only the latest state that this controller actually requested is considered
// owned. A worker still needs a fresh readback and recovery/capture proof at
// release. This type deliberately performs no compositor mutation.
struct State {
    ConsoleTopologyPlan::Plan cumulative;
    ConsoleTopologyPlan::Inventory original;
    QMap<QString, ConsoleTopologyPlan::Mode> selected;
    quint64 latestRevision = 0;
};

inline std::optional<State> start(const ConsoleTopologyPlan::Plan &first,
    const ConsoleTopologyPlan::Inventory &before,
    const QMap<QString, ConsoleTopologyPlan::Mode> &selected)
{
    if (!first.changed || !ConsoleTopologyPlan::arguments(first, before.states, before.priorities, selected)) return {};
    return State{first, before, selected, first.before.revision};
}

inline std::optional<State> advance(const State &lease, const ConsoleTopologyPlan::Plan &next,
    const ConsoleTopologyPlan::Inventory &currentBefore,
    const QMap<QString, ConsoleTopologyPlan::Mode> &nextSelected)
{
    using namespace ConsoleTopologyPlan;
    if (!next.changed || next.before.generation != lease.cumulative.before.generation
        || next.before.outputs != lease.cumulative.after
        || next.beforePriorities != lease.cumulative.afterPriorities
        || next.before.revision <= lease.latestRevision
        || !arguments(next, currentBefore.states, currentBefore.priorities, nextSelected)
        || currentBefore.states.size() != lease.original.states.size()) return {};
    State merged = lease;
    merged.latestRevision = next.before.revision;
    merged.cumulative.after = next.after;
    merged.cumulative.afterPriorities = next.afterPriorities;
    merged.cumulative.positions.clear();
    merged.cumulative.modes.clear();
    merged.selected.clear();
    for (qsizetype i = 0; i < lease.cumulative.before.outputs.size(); ++i) {
        const auto &original = lease.cumulative.before.outputs[i];
        if (i >= next.after.size() || next.after[i].id != original.id
            || next.after[i].output.backendKey != original.output.backendKey) return {};
        const QString name = original.output.backendKey;
        if (!lease.original.states.contains(name) || !currentBefore.states.contains(name)) return {};
        const Mode expectedCurrent = lease.selected.value(name, lease.original.states.value(name).current);
        if (currentBefore.states.value(name).current != expectedCurrent) return {};
        const auto &final = next.after[i].output;
        const auto &baseline = original.output;
        if (final.logicalGeometry.topLeft() != baseline.logicalGeometry.topLeft())
            merged.cumulative.positions.insert(name, final.logicalGeometry.topLeft());
        const Mode finalMode = nextSelected.value(name, expectedCurrent);
        if (finalMode.pixels != final.nativePixels) return {};
        if (finalMode != lease.original.states.value(name).current
            || !sameScale(final.scale, baseline.scale)) {
            merged.cumulative.modes.insert(name, {final.nativePixels, final.scale});
            merged.selected.insert(name, finalMode);
        }
    }
    merged.cumulative.changed = !merged.cumulative.positions.isEmpty() || !merged.cumulative.modes.isEmpty()
        || merged.cumulative.afterPriorities != merged.cumulative.beforePriorities;
    if (merged.cumulative.changed
        && !arguments(merged.cumulative, merged.original.states, merged.original.priorities, merged.selected)) return {};
    return merged;
}
}
