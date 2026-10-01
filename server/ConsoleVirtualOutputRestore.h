// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "OutputRestoreJournal.h"
#include "RetainedKScreenReadback.h"

#include <cmath>
#include <unistd.h>

namespace KRdp::ConsoleVirtualOutputRestore
{
inline std::optional<OutputRestoreJournal::Entry> snapshot(const QByteArray &json, const QString &sessionId,
    const QSet<QString> &excluded = {})
{
    if (sessionId.isEmpty() || json.isEmpty() || json.size() > 1024 * 1024) return {};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    const auto values = document.object().value(QStringLiteral("outputs"));
    if (error.error != QJsonParseError::NoError || !document.isObject() || !values.isArray()
        || values.toArray().isEmpty() || values.toArray().size() > 16) return {};
    for (const auto &value : values.toArray()) {
        if (!value.isObject()) return {};
        const auto object = value.toObject();
        if (!object.value(QStringLiteral("connected")).isBool()) return {};
        if (!object.value(QStringLiteral("connected")).toBool()) continue;
        const auto position = object.value(QStringLiteral("pos")).toObject();
        if (!object.value(QStringLiteral("enabled")).isBool() || !object.value(QStringLiteral("scale")).isDouble()
            || !RetainedKScreenReadback::integer(position.value(QStringLiteral("x")), -32767, 32767)
            || !RetainedKScreenReadback::integer(position.value(QStringLiteral("y")), -32767, 32767)
            || !RetainedKScreenReadback::integer(object.value(QStringLiteral("priority")), 0, 16)
            || !object.value(QStringLiteral("currentModeId")).isString()) return {};
    }
    bool ok = false;
    const auto current = OutputRestoreJournal::parseCurrent(json, &ok);
    if (!ok || current.isEmpty() || current.size() > 16) return {};
    OutputRestoreJournal::Entry result{QString::fromLatin1(OutputRestoreJournal::ConsoleVirtualOwner), qint64(getpid()), sessionId, {}};
    QSet<QString> names;
    int enabled = 0;
    for (const auto &output : current) {
        if (!RetainedKScreenReadback::outputName(output.name) || names.contains(output.name)
            || output.mode.size() > 128 || !std::isfinite(output.scale) || output.scale < 1 || output.scale > 4
            || output.position.x() < -32767 || output.position.x() > 32767
            || output.position.y() < -32767 || output.position.y() > 32767
            || output.priority < 0 || output.priority > 16) return {};
        names.insert(output.name);
        if (excluded.contains(output.name)) continue;
        OutputRestoreJournal::Fields fields;
        fields.enabled = output.enabled;
        if (output.enabled) {
            if (output.mode.isEmpty() || output.priority == 0) return {};
            fields.mode = output.mode;
            fields.scale = output.scale;
            fields.position = output.position;
            fields.priority = output.priority;
            ++enabled;
        }
        result.outputs.append({output.name, fields, {}});
    }
    // Never retire the only enabled outputs while restoration is unverified.
    return enabled ? std::optional(result) : std::nullopt;
}

inline bool matches(const OutputRestoreJournal::Entry &expected, const QByteArray &json,
    const QSet<QString> &retired = {})
{
    if (!snapshot(json, expected.session)) return false;
    bool ok = false;
    const auto current = OutputRestoreJournal::parseCurrent(json, &ok);
    if (!ok || current.size() != expected.outputs.size()) return false;
    QSet<QString> names;
    for (const auto &output : current) {
        if (names.contains(output.name) || retired.contains(output.name)) return false;
        names.insert(output.name);
    }
    const auto plan = OutputRestoreJournal::plan(expected, current);
    return plan.missing.isEmpty() && plan.arguments.isEmpty();
}
}
