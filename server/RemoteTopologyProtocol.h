// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RemoteTopologyCatalog.h"
#include "RemoteTopologyDraft.h"

#include <cmath>
#include <optional>

#include <QJsonArray>
#include <QJsonObject>

namespace KRdp::RemoteTopologyProtocol
{
inline bool identifier(const QJsonValue &value, int maximum = 64)
{
    if (!value.isString()) return false;
    const auto id = value.toString();
    if (id.isEmpty() || id.size() > maximum) return false;
    for (const auto character : id) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('-') && character != QLatin1Char('_')) return false;
    }
    return true;
}

inline std::optional<QString> queryId(const QJsonObject &record)
{
    if (record.size() != 3 || record.value(QStringLiteral("type")) != QStringLiteral("topology-query")
        || record.value(QStringLiteral("v")) != 1 || !identifier(record.value(QStringLiteral("id")))) return {};
    return record.value(QStringLiteral("id")).toString();
}

struct PreviewRequest {
    QString id;
    RemoteTopologyDraft::Request draft; // owner is always filled from authenticated server state
};

inline std::optional<PreviewRequest> previewRequest(const QJsonObject &record)
{
    constexpr double maxExact = 9007199254740991.0;
    if (record.size() != 8 || record.value(QStringLiteral("type")) != QStringLiteral("topology-preview")
        || record.value(QStringLiteral("v")) != 1 || !identifier(record.value(QStringLiteral("id")))
        || !identifier(record.value(QStringLiteral("generation")), 128)
        || !record.value(QStringLiteral("expectedRevision")).isDouble()
        || !std::isfinite(record.value(QStringLiteral("expectedRevision")).toDouble())
        || record.value(QStringLiteral("expectedRevision")).toDouble() < 1
        || record.value(QStringLiteral("expectedRevision")).toDouble() > maxExact
        || std::floor(record.value(QStringLiteral("expectedRevision")).toDouble()) != record.value(QStringLiteral("expectedRevision")).toDouble()
        || !record.value(QStringLiteral("allowRemoval")).isBool()
        || !record.value(QStringLiteral("allowPhysicalChange")).isBool()
        || !record.value(QStringLiteral("operations")).isArray()) return {};
    const auto operations = record.value(QStringLiteral("operations")).toArray();
    if (operations.isEmpty() || operations.size() > 16) return {};
    PreviewRequest result;
    result.id = record.value(QStringLiteral("id")).toString();
    result.draft.generation = record.value(QStringLiteral("generation")).toString();
    result.draft.expectedRevision = quint64(record.value(QStringLiteral("expectedRevision")).toDouble());
    result.draft.allowRemoval = record.value(QStringLiteral("allowRemoval")).toBool();
    result.draft.allowPhysicalChange = record.value(QStringLiteral("allowPhysicalChange")).toBool();
    for (const auto &value : operations) {
        if (!value.isObject()) return {};
        const auto object = value.toObject();
        const auto kind = object.value(QStringLiteral("op")).toString();
        const auto output = object.value(QStringLiteral("output"));
        const bool temporary = kind == QStringLiteral("add") && output.isString()
            && output.toString().startsWith(QStringLiteral("new:"))
            && identifier(output.toString().mid(4), 124);
        if (!identifier(output, 128) && !temporary) return {};
        RemoteTopologyDraft::Operation operation;
        operation.id = output.toString();
        if (kind == QStringLiteral("add") || kind == QStringLiteral("move") || kind == QStringLiteral("resize")) {
            const auto position = object.value(QStringLiteral("position")).toObject();
            if (kind != QStringLiteral("resize")) {
                if (position.size() != 2) return {};
                const auto x = position.value(QStringLiteral("x"));
                const auto y = position.value(QStringLiteral("y"));
                if (!x.isDouble() || !y.isDouble() || !std::isfinite(x.toDouble()) || !std::isfinite(y.toDouble())
                    || x.toDouble() < -32768 || x.toDouble() > 32768 || y.toDouble() < -32768 || y.toDouble() > 32768
                    || std::floor(x.toDouble()) != x.toDouble() || std::floor(y.toDouble()) != y.toDouble()) return {};
                operation.position = QPoint(x.toInt(), y.toInt());
            }
            if (kind == QStringLiteral("move")) {
                if (object.size() != 3) return {};
                operation.kind = RemoteTopologyDraft::Operation::Kind::Move;
            } else {
                const auto pixels = object.value(QStringLiteral("pixels")).toObject();
                const auto width = pixels.value(QStringLiteral("width"));
                const auto height = pixels.value(QStringLiteral("height"));
                const auto scale = object.value(QStringLiteral("scale"));
                if (object.size() != (kind == QStringLiteral("add") ? 5 : 4)
                    || (kind == QStringLiteral("resize") && object.contains(QStringLiteral("position")))
                    || pixels.size() != 2 || !width.isDouble() || !height.isDouble()
                    || !std::isfinite(width.toDouble()) || !std::isfinite(height.toDouble())
                    || width.toDouble() < 1 || width.toDouble() > 16384
                    || height.toDouble() < 1 || height.toDouble() > 16384
                    || std::floor(width.toDouble()) != width.toDouble() || std::floor(height.toDouble()) != height.toDouble()
                    || !scale.isDouble() || !std::isfinite(scale.toDouble()) || scale.toDouble() < 1 || scale.toDouble() > 4) return {};
                operation.pixels = QSize(width.toInt(), height.toInt());
                operation.scale = scale.toDouble();
                operation.kind = kind == QStringLiteral("add") ? RemoteTopologyDraft::Operation::Kind::AddVirtual
                    : RemoteTopologyDraft::Operation::Kind::Resize;
                if (kind == QStringLiteral("add") && !operation.id.startsWith(QStringLiteral("new:"))) return {};
            }
        } else if (kind == QStringLiteral("remove") || kind == QStringLiteral("primary")) {
            if (object.size() != 2) return {};
            operation.kind = kind == QStringLiteral("remove") ? RemoteTopologyDraft::Operation::Kind::Remove
                : RemoteTopologyDraft::Operation::Kind::SetPrimary;
        } else return {};
        result.draft.operations.append(operation);
    }
    return result;
}

inline QJsonObject error(const QString &id, const QString &code)
{
    return {{QStringLiteral("type"), QStringLiteral("topology-error")}, {QStringLiteral("v"), 1},
        {QStringLiteral("id"), id}, {QStringLiteral("code"), code}};
}

struct CommitRequest {
    QString id;
    QString token;
    QString generation;
    quint64 expectedRevision = 0;
};

inline std::optional<CommitRequest> commitRequest(const QJsonObject &record)
{
    const auto revision = record.value(QStringLiteral("expectedRevision"));
    constexpr double maxExact = 9007199254740991.0;
    if (record.size() != 6 || record.value(QStringLiteral("type")) != QStringLiteral("topology-commit")
        || record.value(QStringLiteral("v")) != 1 || !identifier(record.value(QStringLiteral("id")))
        || !identifier(record.value(QStringLiteral("token")), 64)
        || !identifier(record.value(QStringLiteral("generation")), 128)
        || !revision.isDouble() || !std::isfinite(revision.toDouble()) || revision.toDouble() < 1
        || revision.toDouble() > maxExact || std::floor(revision.toDouble()) != revision.toDouble()) return {};
    return CommitRequest{record.value(QStringLiteral("id")).toString(), record.value(QStringLiteral("token")).toString(),
        record.value(QStringLiteral("generation")).toString(), quint64(revision.toDouble())};
}

inline QJsonArray outputArray(const QVector<RemoteTopologyCatalog::Entry> &entries, const QString &lifetime = QStringLiteral("retained"))
{
    QJsonArray outputs;
    for (const auto &entry : entries) {
        const auto &output = entry.output;
        outputs.append(QJsonObject{
            {QStringLiteral("id"), entry.id}, {QStringLiteral("name"), output.name},
            {QStringLiteral("kind"), output.physical ? QStringLiteral("physical") : QStringLiteral("virtual")},
            {QStringLiteral("owner"), output.owner}, {QStringLiteral("lifetime"), lifetime},
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
    return outputs;
}

inline QJsonObject previewReply(const QString &id, const QString &token, const RemoteTopologyDraft::Preview &draft,
    const RemoteTopologyCatalog::Snapshot &snapshot)
{
    return {{QStringLiteral("type"), QStringLiteral("topology-preview")}, {QStringLiteral("v"), 1},
        {QStringLiteral("id"), id}, {QStringLiteral("token"), token},
        {QStringLiteral("generation"), snapshot.generation}, {QStringLiteral("expectedRevision"), double(snapshot.revision)},
        {QStringLiteral("before"), outputArray(draft.before)}, {QStringLiteral("after"), outputArray(draft.after)},
        {QStringLiteral("warnings"), QJsonArray{}}};
}

// Retained position/Add/owned Remove use revisioned preview/commit and fresh
// compositor/capture readback. Legacy single-output resize is not a general
// topology capability.
inline QJsonObject retainedReadOnly(const QString &id, const RemoteTopologyCatalog::Snapshot &snapshot)
{
    const bool removable = snapshot.outputs.size() > 2
        && std::any_of(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [](const auto &entry) {
            return !entry.output.physical && entry.output.backendKey.startsWith(QStringLiteral("Virtual-krdp-added-"));
        });
    return {{QStringLiteral("type"), QStringLiteral("topology")}, {QStringLiteral("v"), 1},
        {QStringLiteral("id"), id}, {QStringLiteral("generation"), snapshot.generation},
        {QStringLiteral("revision"), double(snapshot.revision)}, {QStringLiteral("outputs"), outputArray(snapshot.outputs)},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("enumerate"), true},
            {QStringLiteral("add"), snapshot.outputs.size() > 1 && snapshot.outputs.size() < 16},
            {QStringLiteral("remove"), removable}, {QStringLiteral("position"), snapshot.outputs.size() > 1},
            {QStringLiteral("resize"), false}, {QStringLiteral("scale"), false},
            {QStringLiteral("primary"), false}, {QStringLiteral("multiOutputCapture"), snapshot.outputs.size() > 1},
            {QStringLiteral("maxOutputs"), 16}, {QStringLiteral("positionMin"), 0},
            {QStringLiteral("positionMax"), 32768}, {QStringLiteral("lifetime"), QStringLiteral("retained")},
        }}};
}

inline QJsonObject consoleReadOnly(const QString &id, const RemoteTopologyCatalog::Snapshot &snapshot)
{
    return {{QStringLiteral("type"), QStringLiteral("topology")}, {QStringLiteral("v"), 1},
        {QStringLiteral("id"), id}, {QStringLiteral("generation"), snapshot.generation},
        {QStringLiteral("revision"), double(snapshot.revision)},
        {QStringLiteral("outputs"), outputArray(snapshot.outputs, QStringLiteral("lease"))},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("enumerate"), true}, {QStringLiteral("add"), false},
            {QStringLiteral("remove"), false}, {QStringLiteral("position"), false},
            {QStringLiteral("resize"), false}, {QStringLiteral("scale"), false},
            {QStringLiteral("primary"), false}, {QStringLiteral("multiOutputCapture"), false},
            {QStringLiteral("maxOutputs"), 16}, {QStringLiteral("lifetime"), QStringLiteral("lease")},
        }}};
}
}
