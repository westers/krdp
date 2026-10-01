// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "ConsoleVirtualOutputReadback.h"
#include "ConsoleVirtualOutputRestore.h"
#include "VirtualResize.h"

#include <QMap>

namespace KRdp::ConsoleVirtualOutputMutation
{
inline std::optional<QVector<OutputRestoreJournal::Current>> foreign(const QByteArray &json,
    const QString &session, const ConsoleVirtualOutputPlan::Plan &owned)
{
    if (!ConsoleVirtualOutputRestore::snapshot(json, session)
        || !ConsoleVirtualOutputReadback::parse(json, session, owned)) return {};
    bool ok = false;
    auto outputs = OutputRestoreJournal::parseCurrent(json, &ok);
    if (!ok) return {};
    outputs.erase(std::remove_if(outputs.begin(), outputs.end(), [&owned](const auto &output) {
        return std::any_of(owned.outputs.cbegin(), owned.outputs.cend(), [&output](const auto &entry) { return entry.name == output.name; });
    }), outputs.end());
    std::sort(outputs.begin(), outputs.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
    return outputs;
}

inline bool preservesForeign(const QByteArray &before, const QByteArray &after,
    const QString &session, const ConsoleVirtualOutputPlan::Plan &owned)
{
    const auto original = foreign(before, session, owned), current = foreign(after, session, owned);
    if (!original || !current || original->size() != current->size()) return false;
    for (qsizetype i = 0; i < original->size(); ++i) {
        const auto &a = (*original)[i], &b = (*current)[i];
        if (a.name != b.name || a.enabled != b.enabled || a.mode != b.mode || a.position != b.position
            || a.priority != b.priority || !VirtualResize::sameScale(a.scale, b.scale)) return false;
    }
    const auto geometry = [&owned](const QByteArray &json) {
        QMap<QString, QJsonObject> result;
        const auto inventory = QJsonDocument::fromJson(json).object().value(QStringLiteral("outputs")).toArray();
        for (const auto &value : inventory) {
            const auto output = value.toObject();
            const auto name = output.value(QStringLiteral("name")).toString();
            if (!output.value(QStringLiteral("connected")).toBool()
                || std::any_of(owned.outputs.cbegin(), owned.outputs.cend(), [&name](const auto &entry) { return entry.name == name; })) continue;
            result.insert(name, {{QStringLiteral("id"), output.value(QStringLiteral("id"))},
                {QStringLiteral("size"), output.value(QStringLiteral("size"))},
                {QStringLiteral("rotation"), output.value(QStringLiteral("rotation"))},
                {QStringLiteral("replicationSource"), output.value(QStringLiteral("replicationSource"))}});
        }
        return result;
    };
    return geometry(before) == geometry(after);
}

// An owned projection cannot authorize mutations to physical/foreign outputs.
// Preflight the proposed owned geometry against the *complete fresh* inventory
// as well, so a move/reflow cannot overlap an uncaptured survivor.
inline bool allowed(const QByteArray &json, const QString &session,
    const ConsoleVirtualOutputPlan::Plan &owned, const QVector<RemoteTopologyCatalog::Output> &after)
{
    const auto before = ConsoleVirtualOutputReadback::parse(json, session, owned);
    if (!before || !foreign(json, session, owned) || after.size() != before->outputs.size()) return false;
    const auto complete = OutputSnapshot::parse(json);
    const auto inventory = QJsonDocument::fromJson(json).object().value(QStringLiteral("outputs")).toArray();
    for (const auto &value : inventory) {
        const auto output = value.toObject();
        if (!output.value(QStringLiteral("connected")).toBool() || !output.value(QStringLiteral("enabled")).toBool()) continue;
        const auto size = output.value(QStringLiteral("size")).toObject();
        if (!RetainedKScreenReadback::integer(size.value(QStringLiteral("width")), 1, 16384)
            || !RetainedKScreenReadback::integer(size.value(QStringLiteral("height")), 1, 16384)) return false;
    }
    QSet<QString> names;
    QVector<QRect> placed;
    QRect workspace;
    for (const auto &output : after) {
        const auto original = std::find_if(before->outputs.cbegin(), before->outputs.cend(), [&output](const auto &entry) {
            return entry.backendKey == output.backendKey;
        });
        if (original == before->outputs.cend() || names.contains(output.backendKey) || output.physical || !output.enabled
            || output.owner != session || output.primary != original->primary
            || !VirtualResize::validRequest(output.nativePixels, output.scale)
            || output.logicalGeometry != RemoteMonitorGeometry::logicalRect(output.logicalGeometry.topLeft(), output.nativePixels, output.scale)
            || output.logicalGeometry.x() < 0 || output.logicalGeometry.y() < 0
            || qint64(output.logicalGeometry.x()) + output.logicalGeometry.width() > 32768
            || qint64(output.logicalGeometry.y()) + output.logicalGeometry.height() > 32768) return false;
        names.insert(output.backendKey);
        for (const auto &other : placed) if (other.intersects(output.logicalGeometry)) return false;
        for (const auto &survivor : complete) {
            const bool ours = std::any_of(owned.outputs.cbegin(), owned.outputs.cend(), [&survivor](const auto &entry) { return entry.name == survivor.name; });
            if (!ours && survivor.enabled && output.logicalGeometry.intersects(
                RemoteMonitorGeometry::logicalRect(survivor.position, survivor.size, survivor.scale))) return false;
        }
        placed.append(output.logicalGeometry); workspace |= output.logicalGeometry;
    }
    return workspace.width() <= 8192 && workspace.height() <= 8192;
}
}
