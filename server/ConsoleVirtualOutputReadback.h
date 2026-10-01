// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "ConsoleVirtualOutputPlan.h"
#include "RetainedKScreenReadback.h"

namespace KRdp::ConsoleVirtualOutputReadback
{
// Capture only creator-owned outputs. Disabled physical outputs and foreign
// outputs remain outside this projection; it never authorizes a full-layout
// mutation. Keep the normal full KScreen parser's stricter contract intact.
inline std::optional<RetainedKScreenReadback::Snapshot> parse(const QByteArray &json,
    const QString &sessionId, const ConsoleVirtualOutputPlan::Plan &plan)
{
    if (json.isEmpty() || json.size() > 1024 * 1024 || plan.outputs.isEmpty() || plan.outputs.size() > 16) return {};
    QJsonParseError error;
    auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    auto root = document.object();
    const auto values = root.value(QStringLiteral("outputs"));
    if (!values.isArray() || values.toArray().isEmpty() || values.toArray().size() > 16) return {};
    QSet<QString> names, found;
    QSet<int> ids;
    QJsonArray selected;
    int secondary = 2;
    for (const auto &value : values.toArray()) {
        if (!value.isObject()) return {};
        auto object = value.toObject();
        const auto name = object.value(QStringLiteral("name")).toString();
        const auto id = object.value(QStringLiteral("id"));
        if (!RetainedKScreenReadback::outputName(name) || names.contains(name)
            || !RetainedKScreenReadback::integer(id, 0, 2147483647) || ids.contains(id.toInt())) return {};
        names.insert(name); ids.insert(id.toInt());
        const auto owned = std::find_if(plan.outputs.cbegin(), plan.outputs.cend(), [&name](const auto &output) { return output.name == name; });
        if (owned == plan.outputs.cend()) continue;
        // Priority is relative to captured outputs, independently of KDE's
        // physical primary. All actual mode/scale/position fields stay intact.
        object.insert(QStringLiteral("priority"), owned->primary ? 1 : secondary++);
        selected.append(object); found.insert(name);
    }
    if (found.size() != plan.outputs.size()) return {};
    root.insert(QStringLiteral("outputs"), selected);
    return RetainedKScreenReadback::parse(QJsonDocument(root).toJson(QJsonDocument::Compact), sessionId);
}
}
