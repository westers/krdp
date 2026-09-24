// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RetainedKScreenReadback.h"
#include "RetainedMultiPrimaryPlan.h"

#include <QSet>

namespace KRdp::ConsoleCreatorLease
{
// A release is allowed to retire only exact KWin outputs for which this
// worker still holds a creator. Snapshot the live compositor immediately
// before dropping those creators so independent edits made earlier in the
// controller's lifetime remain the user's edits.
struct State {
    RetainedKScreenReadback::Snapshot before;
    RetainedMultiPrimaryPlan::Priorities priorities;
    QSet<QString> owned;
};

inline std::optional<State> start(const QByteArray &json, const QString &sessionId,
    const QSet<QString> &owned)
{
    if (owned.isEmpty()) return {};
    const auto before = RetainedKScreenReadback::parse(json, sessionId);
    const auto priorities = before ? RetainedMultiPrimaryPlan::priorities(json, *before) : std::nullopt;
    if (!before || !priorities || owned.size() >= before->outputs.size()) return {};
    QSet<QString> found;
    for (const auto &output : before->outputs) {
        if (!owned.contains(output.backendKey)) continue;
        // Current Console Add creates secondary monitors. Primary transfer
        // onto a temporary monitor must acquire an explicit restoration rule
        // before it can be released as a Console lease.
        if (output.primary) return {};
        found.insert(output.backendKey);
    }
    if (found != owned) return {};
    return State{*before, *priorities, owned};
}

inline bool matchesReleased(const State &lease, const QByteArray &json, const QString &sessionId)
{
    const auto after = RetainedKScreenReadback::parse(json, sessionId);
    const auto priorities = after ? RetainedMultiPrimaryPlan::priorities(json, *after) : std::nullopt;
    if (!after || !priorities || after->outputs.size() + lease.owned.size() != lease.before.outputs.size()) return false;
    QVector<QPair<int, QString>> beforeOrder;
    QVector<QPair<int, QString>> afterOrder;
    qsizetype survivor = 0;
    for (const auto &old : lease.before.outputs) {
        if (lease.owned.contains(old.backendKey)) continue;
        if (survivor >= after->outputs.size() || after->outputs[survivor] != old
            || !lease.priorities.contains(old.backendKey)
            || !priorities->contains(old.backendKey)) return false;
        beforeOrder.append({lease.priorities.value(old.backendKey), old.backendKey});
        afterOrder.append({priorities->value(old.backendKey), old.backendKey});
        ++survivor;
    }
    if (survivor != after->outputs.size()) return false;
    std::sort(beforeOrder.begin(), beforeOrder.end());
    std::sort(afterOrder.begin(), afterOrder.end());
    for (qsizetype i = 0; i < beforeOrder.size(); ++i)
        if (beforeOrder[i].second != afterOrder[i].second) return false;
    return true;
}
}
