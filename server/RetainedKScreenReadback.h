// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "RemoteTopologyCatalog.h"
#include "RemoteMonitorGeometry.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace KRdp::RetainedKScreenReadback
{
struct Snapshot {
    QVector<RemoteTopologyCatalog::Output> outputs;
    int maxActiveOutputs = 0;
};

inline bool integer(const QJsonValue &value, int minimum, int maximum)
{
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= minimum
        && value.toDouble() <= maximum && std::floor(value.toDouble()) == value.toDouble();
}

inline bool outputName(const QString &name)
{
    if (name.isEmpty() || name.size() > 128) return false;
    for (const auto c : name) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('_')) return false;
    }
    return true;
}

// This is a parser for a fresh `kscreen-doctor -j` from inside the verified
// private compositor, NOT proof that an arbitrary XDG_RUNTIME_DIR is retained
// or that every output has a working encoder. It intentionally supports only
// enabled, unrotated, non-mirrored outputs until those cases have capture proof.
inline std::optional<Snapshot> parse(const QByteArray &json, const QString &authenticatedOwner)
{
    if (authenticatedOwner.isEmpty() || json.isEmpty() || json.size() > 1024 * 1024) return {};
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return {};
    const auto root = document.object();
    const auto values = root.value(QStringLiteral("outputs"));
    const auto screen = root.value(QStringLiteral("screen")).toObject();
    if (!values.isArray() || values.toArray().isEmpty() || values.toArray().size() > 16
        || !integer(screen.value(QStringLiteral("maxActiveOutputsCount")), 1, 16)
        || screen.value(QStringLiteral("maxActiveOutputsCount")).toInt() < values.toArray().size()) return {};

    Snapshot result;
    result.maxActiveOutputs = screen.value(QStringLiteral("maxActiveOutputsCount")).toInt();
    QSet<QString> names;
    QSet<int> ids;
    QSet<int> priorities;
    int primary = 0;
    for (const auto &value : values.toArray()) {
        if (!value.isObject()) return {};
        const auto object = value.toObject();
        const QString name = object.value(QStringLiteral("name")).toString();
        const QString currentId = object.value(QStringLiteral("currentModeId")).toString();
        const auto position = object.value(QStringLiteral("pos")).toObject();
        const auto scaleValue = object.value(QStringLiteral("scale"));
        const auto modes = object.value(QStringLiteral("modes"));
        const auto priorityValue = object.value(QStringLiteral("priority"));
        const auto outputId = object.value(QStringLiteral("id"));
        if (!outputName(name) || names.contains(name) || !integer(outputId, 0, 2147483647)
            || ids.contains(outputId.toInt()) || currentId.isEmpty() || currentId.size() > 128
            || object.value(QStringLiteral("connected")) != QJsonValue(true)
            || object.value(QStringLiteral("enabled")) != QJsonValue(true)
            || !integer(object.value(QStringLiteral("rotation")), 1, 1)
            || !integer(object.value(QStringLiteral("replicationSource")), 0, 0)
            || !integer(priorityValue, 1, 16) || priorities.contains(priorityValue.toInt())
            || !integer(position.value(QStringLiteral("x")), -32768, 32768)
            || !integer(position.value(QStringLiteral("y")), -32768, 32768)
            || !scaleValue.isDouble() || !std::isfinite(scaleValue.toDouble())
            || scaleValue.toDouble() < 1.0 || scaleValue.toDouble() > 4.0
            || !modes.isArray() || modes.toArray().isEmpty() || modes.toArray().size() > 512) return {};
        QSize pixels;
        int currentMatches = 0;
        QSet<QString> modeIds;
        for (const auto &modeValue : modes.toArray()) {
            if (!modeValue.isObject()) return {};
            const auto mode = modeValue.toObject();
            const auto modeId = mode.value(QStringLiteral("id")).toString();
            const auto size = mode.value(QStringLiteral("size")).toObject();
            if (modeId.isEmpty() || modeId.size() > 128 || modeIds.contains(modeId)
                || !integer(size.value(QStringLiteral("width")), 1, 16384)
                || !integer(size.value(QStringLiteral("height")), 1, 16384)) return {};
            modeIds.insert(modeId);
            if (modeId == currentId) {
                pixels = QSize(size.value(QStringLiteral("width")).toInt(), size.value(QStringLiteral("height")).toInt());
                ++currentMatches;
            }
        }
        const auto size = object.value(QStringLiteral("size")).toObject();
        if (currentMatches != 1 || !integer(size.value(QStringLiteral("width")), 1, 16384)
            || !integer(size.value(QStringLiteral("height")), 1, 16384)
            || pixels != QSize(size.value(QStringLiteral("width")).toInt(), size.value(QStringLiteral("height")).toInt())) return {};
        names.insert(name);
        ids.insert(outputId.toInt());
        priorities.insert(priorityValue.toInt());
        if (priorityValue.toInt() == 1) ++primary;
        const QPoint origin(position.value(QStringLiteral("x")).toInt(), position.value(QStringLiteral("y")).toInt());
        result.outputs.append({.backendKey = name, .name = name, .nativePixels = pixels,
            .logicalGeometry = RemoteMonitorGeometry::logicalRect(origin, pixels, scaleValue.toDouble()),
            .scale = scaleValue.toDouble(), .enabled = true, .primary = priorityValue.toInt() == 1,
            .physical = false, .owner = authenticatedOwner});
    }
    if (primary != 1) return {};
    std::sort(result.outputs.begin(), result.outputs.end(), [](const auto &a, const auto &b) { return a.backendKey < b.backendKey; });
    return result;
}

struct Placement {
    QString outputName;
    QPoint logicalPosition;
};

// Arguments only; callers must run kscreen-doctor without a shell and require
// fresh readback plus independently verified encoded frames before success.
inline std::optional<QStringList> positionArguments(const Snapshot &before, const QVector<Placement> &positions)
{
    if (positions.isEmpty() || positions.size() > before.outputs.size()) return {};
    QSet<QString> names;
    QStringList args;
    for (const auto &position : positions) {
        if (!outputName(position.outputName) || names.contains(position.outputName)
            || position.logicalPosition.x() < -32768 || position.logicalPosition.x() > 32768
            || position.logicalPosition.y() < -32768 || position.logicalPosition.y() > 32768
            || std::none_of(before.outputs.cbegin(), before.outputs.cend(), [&position](const auto &output) {
                return output.backendKey == position.outputName;
            })) return {};
        names.insert(position.outputName);
        args.append(QStringLiteral("output.%1.position.%2,%3").arg(position.outputName)
            .arg(position.logicalPosition.x()).arg(position.logicalPosition.y()));
    }
    std::sort(args.begin(), args.end());
    return args;
}
}
