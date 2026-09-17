// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>

#include <QByteArray>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace KRdp
{
/**
 * Pure model of KWin's output arrangement as `kscreen-doctor -j` reports it,
 * and the `kscreen-doctor` argument lists that change it (OPT-041). No
 * process is run here; PhysicalOutputGuard does that.
 */
namespace OutputSnapshot
{
/** KWin names screencast-created outputs "Virtual-<requested name>". */
const QLatin1String VirtualPrefix("Virtual-");

struct Output {
    QString name;
    bool enabled = false;
    QPoint position;
    int priority = 0;
    QSize size;

    bool operator==(const Output &other) const = default;
};

struct Placement {
    /** KWin output name, prefix included. */
    QString name;
    QPoint position;
};

inline bool isVirtual(const QString &name)
{
    return name.startsWith(VirtualPrefix);
}

/** Connected outputs from `kscreen-doctor -j`. Empty + \a error set on malformed input. */
inline QVector<Output> parse(const QByteArray &kscreenJson, QString *error = nullptr)
{
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(kscreenJson, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        if (error) {
            *error = QStringLiteral("kscreen-doctor output is not a JSON object: %1").arg(parseError.errorString());
        }
        return {};
    }
    const auto outputsValue = doc.object().value(QLatin1String("outputs"));
    if (!outputsValue.isArray()) {
        if (error) {
            *error = QStringLiteral("kscreen-doctor output has no \"outputs\" array");
        }
        return {};
    }

    QVector<Output> outputs;
    for (const auto &entry : outputsValue.toArray()) {
        const auto object = entry.toObject();
        if (!object.value(QLatin1String("connected")).toBool(false)) {
            continue;
        }
        const auto pos = object.value(QLatin1String("pos")).toObject();
        const auto size = object.value(QLatin1String("size")).toObject();
        outputs.push_back(Output{
            .name = object.value(QLatin1String("name")).toString(),
            .enabled = object.value(QLatin1String("enabled")).toBool(false),
            .position = QPoint(pos.value(QLatin1String("x")).toInt(), pos.value(QLatin1String("y")).toInt()),
            .priority = object.value(QLatin1String("priority")).toInt(),
            .size = QSize(size.value(QLatin1String("width")).toInt(), size.value(QLatin1String("height")).toInt()),
        });
    }
    if (error) {
        error->clear();
    }
    return outputs;
}

inline QVector<Output> physicalOnly(const QVector<Output> &outputs)
{
    QVector<Output> physical;
    for (const auto &output : outputs) {
        if (!isVirtual(output.name)) {
            physical.push_back(output);
        }
    }
    return physical;
}

/** Bounding rect of the enabled outputs; invalid when none is enabled. */
inline QRect enabledUnion(const QVector<Output> &outputs)
{
    QRect rect;
    for (const auto &output : outputs) {
        if (output.enabled) {
            rect |= QRect(output.position, output.size);
        }
    }
    return rect;
}

/**
 * Give the virtual outputs their places and priorities (primary first at
 * \a firstPriority, the rest in list order after it).
 */
inline QStringList positionArgs(const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName, int firstPriority)
{
    QStringList args;
    int priority = firstPriority;
    auto appendPlacement = [&](const Placement &placement) {
        args << QStringLiteral("output.%1.priority.%2").arg(placement.name).arg(priority++);
        args << QStringLiteral("output.%1.position.%2,%3").arg(placement.name).arg(placement.position.x()).arg(placement.position.y());
    };
    for (const auto &placement : virtualOutputs) {
        if (placement.name == primaryVirtualName) {
            appendPlacement(placement);
        }
    }
    for (const auto &placement : virtualOutputs) {
        if (placement.name != primaryVirtualName) {
            appendPlacement(placement);
        }
    }
    return args;
}

/**
 * `replace` policy in one kscreen-doctor invocation: the virtual outputs take
 * the top priorities and their positions, then every physical output that is
 * currently enabled is disabled. One invocation so KWin never sees a
 * configuration without an enabled output.
 */
inline QStringList replaceArgs(const QVector<Output> &physical, const QVector<Placement> &virtualOutputs, const QString &primaryVirtualName)
{
    QStringList args = positionArgs(virtualOutputs, primaryVirtualName, 1);
    for (const auto &output : physical) {
        if (output.enabled) {
            args << QStringLiteral("output.%1.disable").arg(output.name);
        }
    }
    return args;
}

/** Put the physical outputs back exactly as the snapshot had them. */
inline QStringList restoreArgs(const QVector<Output> &physical)
{
    QStringList args;
    for (const auto &output : physical) {
        if (!output.enabled) {
            args << QStringLiteral("output.%1.disable").arg(output.name);
            continue;
        }
        args << QStringLiteral("output.%1.enable").arg(output.name);
        args << QStringLiteral("output.%1.position.%2,%3").arg(output.name).arg(output.position.x()).arg(output.position.y());
        args << QStringLiteral("output.%1.priority.%2").arg(output.name).arg(output.priority);
    }
    return args;
}

/** Every physical output of \a snapshot is present in \a current with the same enabled/position/priority (size and virtual outputs ignored). */
inline bool matches(const QVector<Output> &snapshot, const QVector<Output> &current)
{
    for (const auto &wanted : snapshot) {
        if (isVirtual(wanted.name)) {
            continue;
        }
        const auto it = std::find_if(current.cbegin(), current.cend(), [&wanted](const Output &candidate) {
            return candidate.name == wanted.name;
        });
        if (it == current.cend() || it->enabled != wanted.enabled || (wanted.enabled && (it->position != wanted.position || it->priority != wanted.priority))) {
            return false;
        }
    }
    return true;
}

inline QByteArray toJson(const QVector<Output> &outputs)
{
    QJsonArray array;
    for (const auto &output : outputs) {
        array.push_back(QJsonObject{
            {QLatin1String("name"), output.name},
            {QLatin1String("enabled"), output.enabled},
            {QLatin1String("x"), output.position.x()},
            {QLatin1String("y"), output.position.y()},
            {QLatin1String("priority"), output.priority},
            {QLatin1String("width"), output.size.width()},
            {QLatin1String("height"), output.size.height()},
        });
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

inline QVector<Output> fromJson(const QByteArray &json)
{
    const auto doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) {
        return {};
    }
    QVector<Output> outputs;
    for (const auto &entry : doc.array()) {
        const auto object = entry.toObject();
        outputs.push_back(Output{
            .name = object.value(QLatin1String("name")).toString(),
            .enabled = object.value(QLatin1String("enabled")).toBool(false),
            .position = QPoint(object.value(QLatin1String("x")).toInt(), object.value(QLatin1String("y")).toInt()),
            .priority = object.value(QLatin1String("priority")).toInt(),
            .size = QSize(object.value(QLatin1String("width")).toInt(), object.value(QLatin1String("height")).toInt()),
        });
    }
    return outputs;
}

inline QDebug operator<<(QDebug dbg, const Output &output)
{
    QDebugStateSaver saver(dbg);
    dbg.nospace() << "Output(" << output.name << (output.enabled ? " enabled" : " disabled") << " at " << output.position.x() << ',' << output.position.y()
                  << " priority " << output.priority << ')';
    return dbg;
}
}
}
