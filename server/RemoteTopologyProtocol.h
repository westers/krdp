// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RemoteTopologyCatalog.h"

#include <optional>

#include <QJsonArray>
#include <QJsonObject>

namespace KRdp::RemoteTopologyProtocol
{
inline std::optional<QString> queryId(const QJsonObject &record)
{
    if (record.size() != 3 || record.value(QStringLiteral("type")) != QStringLiteral("topology-query")
        || record.value(QStringLiteral("v")) != 1 || !record.value(QStringLiteral("id")).isString()) return {};
    const auto id = record.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || id.size() > 64) return {};
    for (const auto character : id) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return {};
    }
    return id;
}

inline QJsonObject error(const QString &id, const QString &code)
{
    return {{QStringLiteral("type"), QStringLiteral("topology-error")}, {QStringLiteral("v"), 1},
        {QStringLiteral("id"), id}, {QStringLiteral("code"), code}};
}

// The retained single-output worker has no topology writes yet. Report only
// what its captured output proves; never borrow legacy virtual-resize as an
// advertised general resize capability.
inline QJsonObject retainedReadOnly(const QString &id, const RemoteTopologyCatalog::Snapshot &snapshot)
{
    QJsonArray outputs;
    for (const auto &entry : snapshot.outputs) {
        const auto &output = entry.output;
        outputs.append(QJsonObject{
            {QStringLiteral("id"), entry.id}, {QStringLiteral("name"), output.name},
            {QStringLiteral("kind"), output.physical ? QStringLiteral("physical") : QStringLiteral("virtual")},
            {QStringLiteral("owner"), output.owner}, {QStringLiteral("lifetime"), QStringLiteral("retained")},
            {QStringLiteral("enabled"), output.enabled}, {QStringLiteral("primary"), output.primary},
            {QStringLiteral("pixels"), QJsonObject{{QStringLiteral("width"), output.nativePixels.width()},
                {QStringLiteral("height"), output.nativePixels.height()}}},
            {QStringLiteral("logical"), QJsonObject{{QStringLiteral("x"), output.logicalGeometry.x()},
                {QStringLiteral("y"), output.logicalGeometry.y()},
                {QStringLiteral("width"), output.logicalGeometry.width()},
                {QStringLiteral("height"), output.logicalGeometry.height()}}},
            {QStringLiteral("scale"), output.scale},
        });
    }
    return {{QStringLiteral("type"), QStringLiteral("topology")}, {QStringLiteral("v"), 1},
        {QStringLiteral("id"), id}, {QStringLiteral("generation"), snapshot.generation},
        {QStringLiteral("revision"), double(snapshot.revision)}, {QStringLiteral("outputs"), outputs},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("enumerate"), true}, {QStringLiteral("add"), false},
            {QStringLiteral("remove"), false}, {QStringLiteral("position"), false},
            {QStringLiteral("resize"), false}, {QStringLiteral("scale"), false},
            {QStringLiteral("primary"), false}, {QStringLiteral("multiOutputCapture"), false},
            {QStringLiteral("maxOutputs"), 1}, {QStringLiteral("lifetime"), QStringLiteral("retained")},
        }}};
}
}
