// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RemoteTopologyDraft.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

#include <QMap>
#include <QQueue>
#include <QSet>

namespace KRdp::RemoteTopologyFit
{
// A saved, explicitly managed edge. Unrelated outputs (including deliberate
// gaps) have no relation and are never repacked. A child has one parent.
struct Relation {
    enum class Edge { Left, Right, Above, Below };
    QString parent;
    QString child;
    Edge edge = Edge::Right;
    int offset = 0; // logical alignment from parent's top/left edge
};

struct Plan {
    RemoteTopologyDraft::Request request;
    RemoteTopologyDraft::Preview proposal;
    QString error;
    bool valid() const { return error.isEmpty(); }
};

inline std::optional<QPoint> childPosition(const QRect &parent, const QSize &child, const Relation &relation)
{
    const qint64 x = relation.edge == Relation::Edge::Right ? qint64(parent.x()) + parent.width()
        : relation.edge == Relation::Edge::Left ? qint64(parent.x()) - child.width()
        : qint64(parent.x()) + relation.offset;
    const qint64 y = relation.edge == Relation::Edge::Below ? qint64(parent.y()) + parent.height()
        : relation.edge == Relation::Edge::Above ? qint64(parent.y()) - child.height()
        : qint64(parent.y()) + relation.offset;
    if (x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max()
        || y < std::numeric_limits<int>::min() || y > std::numeric_limits<int>::max()) return {};
    return QPoint(int(x), int(y));
}

inline Plan plan(const RemoteTopologyDraft::Snapshot &snapshot, const RemoteTopologyDraft::Capabilities &caps,
    RemoteTopologyDraft::Request base, const QString &selected, const QSize &pixels, qreal scale,
    const QVector<Relation> &relations)
{
    Plan result{std::move(base), {}, {}};
    const auto fail = [&result](const QString &reason) {
        result.request.operations.clear();
        result.error = reason;
        return result;
    };
    if (result.request.generation != snapshot.generation) return fail(QStringLiteral("stale-generation"));
    if (result.request.expectedRevision != snapshot.revision) return fail(QStringLiteral("stale-revision"));
    if (result.request.owner.isEmpty()) return fail(QStringLiteral("not-owner"));
    if (!result.request.operations.isEmpty()) return fail(QStringLiteral("invalid"));
    if (snapshot.outputs.size() > 16 || relations.size() > 16 || caps.maxOutputDimension <= 0
        || caps.maxOutputDimension > 16384 || caps.maxAtlasDimension <= 0 || caps.maxAtlasDimension > 65536
        || pixels.isEmpty() || pixels.width() > caps.maxOutputDimension
        || pixels.height() > caps.maxOutputDimension || !std::isfinite(scale) || scale <= 0.0
        || double(pixels.width()) / scale > caps.maxAtlasDimension
        || double(pixels.height()) / scale > caps.maxAtlasDimension) return fail(QStringLiteral("limit"));

    QMap<QString, RemoteTopologyDraft::Entry> entries;
    QMap<QString, QRect> geometry;
    for (const auto &entry : snapshot.outputs) {
        if (entry.id.isEmpty() || entries.contains(entry.id)) return fail(QStringLiteral("invalid"));
        entries.insert(entry.id, entry);
        geometry.insert(entry.id, entry.output.logicalGeometry);
    }
    if (!entries.contains(selected)) return fail(QStringLiteral("stale-output"));
    const auto &anchor = entries[selected].output;
    if (anchor.physical && (!caps.changePhysical || !result.request.allowPhysicalChange))
        return fail(QStringLiteral("physical-change-requires-confirmation"));
    if (!anchor.physical && (anchor.owner != result.request.owner || !caps.resizeVirtual))
        return fail(anchor.owner != result.request.owner ? QStringLiteral("not-owner") : QStringLiteral("unsupported"));

    QMap<QString, Relation> byChild;
    QMap<QString, QVector<QString>> children;
    for (const auto &relation : relations) {
        if (!entries.contains(relation.parent) || !entries.contains(relation.child)) return fail(QStringLiteral("stale-output"));
        if (relation.parent == relation.child || byChild.contains(relation.child)) return fail(QStringLiteral("invalid-relation"));
        const auto &child = entries[relation.child].output;
        if (child.physical || child.owner != result.request.owner) return fail(QStringLiteral("not-owner"));
        byChild.insert(relation.child, relation);
        children[relation.parent].append(relation.child);
    }
    // Check the entire saved graph, not just descendants of this Fit. A later
    // reflow must not inherit a cycle hidden in a disconnected branch.
    for (auto it = byChild.cbegin(); it != byChild.cend(); ++it) {
        QSet<QString> seen;
        QString current = it.key();
        while (byChild.contains(current)) {
            if (seen.contains(current)) return fail(QStringLiteral("cycle"));
            seen.insert(current);
            current = byChild[current].parent;
        }
    }
    for (auto it = byChild.cbegin(); it != byChild.cend(); ++it) {
        const auto &relation = it.value();
        const auto expected = childPosition(geometry[relation.parent], geometry[relation.child].size(), relation);
        if (!expected || *expected != geometry[relation.child].topLeft()) return fail(QStringLiteral("stale-relation"));
    }
    for (auto it = children.begin(); it != children.end(); ++it) std::sort(it.value().begin(), it.value().end());

    const QSize newLogical = RemoteMonitorGeometry::logicalSize(pixels, scale);
    if (!newLogical.isValid()) return fail(QStringLiteral("limit"));
    if (anchor.nativePixels != pixels || anchor.scale != scale) {
        result.request.operations.append({RemoteTopologyDraft::Operation::Kind::Resize, selected,
            {}, pixels, scale});
    }
    geometry[selected].setSize(newLogical); // Keep the selected output's origin fixed.

    QQueue<QString> queue;
    queue.enqueue(selected);
    while (!queue.isEmpty()) {
        const QString parent = queue.dequeue();
        for (const auto &child : children.value(parent)) {
            const auto position = childPosition(geometry[parent], geometry[child].size(), byChild[child]);
            if (!position || position->x() < -caps.maxAtlasDimension || position->y() < -caps.maxAtlasDimension
                || qint64(position->x()) + geometry[child].width() > caps.maxAtlasDimension
                || qint64(position->y()) + geometry[child].height() > caps.maxAtlasDimension)
                return fail(QStringLiteral("limit"));
            if (*position != geometry[child].topLeft()) {
                result.request.operations.append({RemoteTopologyDraft::Operation::Kind::Move, child,
                    *position, {}, 1.0});
                geometry[child].moveTopLeft(*position);
            }
            queue.enqueue(child);
        }
    }
    // The selected top-left is intentionally fixed. A left/above relation to
    // its parent may therefore become unsatisfiable after growth/shrink; do
    // not silently introduce a gap or move the chosen anchor to repair it.
    for (auto it = byChild.cbegin(); it != byChild.cend(); ++it) {
        const auto &relation = it.value();
        const auto expected = childPosition(geometry[relation.parent], geometry[relation.child].size(), relation);
        if (!expected || *expected != geometry[relation.child].topLeft()) return fail(QStringLiteral("relation-conflict"));
    }
    result.proposal = RemoteTopologyDraft::preview(snapshot, caps, result.request);
    if (!result.proposal.valid()) result.error = result.proposal.error;
    return result;
}
}
