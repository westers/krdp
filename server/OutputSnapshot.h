// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <utility>

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

#include "ClientDisplayInfo.h"
#include "LayoutControl.h"

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
    /** KWin's scale factor for the output (1, 1.25, ...); 1.0 when kscreen does not say. */
    qreal scale = 1.0;

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
            .scale = object.value(QLatin1String("scale")).toDouble(1.0),
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
 * The virtual layout that mirrors \a outputs (a PhysicalOutputGuard snapshot)
 * instead of a client's own advertised monitors (`VirtualMonitorLayout=physical`,
 * OPT-041 S5): the enabled outputs, ordered by priority ascending (KWin's
 * primary-first convention - the lowest-priority one becomes primary here
 * too), translated so the union's top-left is (0,0). Empty monitors and an
 * invalid desktopSize when nothing is enabled.
 *
 * This only reshapes the data; it does not enforce KRdp::ClientDisplay's size
 * rules (a physical output wider than MaxDimension, or a union wider than
 * MaxDesktopDimension, is not exotic hardware and gets no special treatment
 * here) - the caller must still run the result through
 * KRdp::ClientDisplay::sanitize() before using it, exactly as it does for a
 * client's own advertised layout.
 */
inline ClientDisplay::Info toClientDisplayInfo(const QVector<Output> &outputs)
{
    QVector<Output> enabled;
    for (const auto &output : outputs) {
        if (output.enabled) {
            enabled.push_back(output);
        }
    }
    if (enabled.isEmpty()) {
        return {};
    }
    std::sort(enabled.begin(), enabled.end(), [](const Output &a, const Output &b) {
        return a.priority < b.priority;
    });

    QRect unionRect;
    for (const auto &output : std::as_const(enabled)) {
        unionRect |= QRect(output.position, output.size);
    }

    ClientDisplay::Info info;
    info.desktopSize = unionRect.size();
    info.monitors.reserve(enabled.size());
    for (qsizetype i = 0; i < enabled.size(); ++i) {
        info.monitors.push_back(VideoMonitor{QRect(enabled.at(i).position, enabled.at(i).size).translated(-unionRect.topLeft()), i == 0});
    }
    return info;
}

/**
 * The host layout's real monitors as a `KRDPCTL` client sees them (OPT-044):
 * one HostMonitor per output of \a outputs, in list order, with `id` and
 * `name` the connector name, native pixel size and KWin position, KWin's
 * scale, `lit` = enabled. Exactly one is primary: the enabled output with the
 * lowest positive kscreen priority (1 is KWin's primary; a disabled output
 * reports 0), else the first output when nothing qualifies. Virtual outputs
 * are the caller's business: pass physicalOnly() to describe the real ones.
 */
inline QList<LayoutControl::HostMonitor> hostMonitorsFrom(const QVector<Output> &outputs)
{
    qsizetype primaryIndex = -1;
    for (qsizetype i = 0; i < outputs.size(); ++i) {
        const auto &output = outputs.at(i);
        if (output.enabled && output.priority > 0 && (primaryIndex < 0 || output.priority < outputs.at(primaryIndex).priority)) {
            primaryIndex = i;
        }
    }
    if (primaryIndex < 0 && !outputs.isEmpty()) {
        primaryIndex = 0;
    }

    QList<LayoutControl::HostMonitor> monitors;
    monitors.reserve(outputs.size());
    for (qsizetype i = 0; i < outputs.size(); ++i) {
        const auto &output = outputs.at(i);
        LayoutControl::HostMonitor monitor;
        monitor.id = output.name;
        monitor.name = output.name;
        monitor.kind = LayoutControl::Kind::Real;
        monitor.size = output.size;
        monitor.position = output.position;
        monitor.scale = output.scale > 0.0 ? output.scale : 1.0;
        monitor.primary = i == primaryIndex;
        monitor.lit = output.enabled;
        monitors.push_back(monitor);
    }
    return monitors;
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

/**
 * One output of the arrangement the layout executor asks KWin for in a single
 * kscreen-doctor invocation (OPT-044): a physical output enabled at its place
 * or disabled, a virtual output at its place. Enabled entries take priorities
 * 1..N in list order, so the caller lists the primary first.
 */
struct Arrangement {
    /** KWin output name (connector, or a virtual output's name with its prefix). */
    QString name;
    bool enabled = true;
    /** Where an enabled output goes; ignored for a disabled one. */
    QPoint position;

    bool operator==(const Arrangement &other) const = default;
};

/** Only the enables, positions and priorities of \a entries (the second step of the two-step form). */
inline QStringList arrangementEnableArgs(const QList<Arrangement> &entries)
{
    QStringList args;
    int priority = 1;
    for (const auto &entry : entries) {
        if (!entry.enabled) {
            continue;
        }
        args << QStringLiteral("output.%1.enable").arg(entry.name);
        args << QStringLiteral("output.%1.position.%2,%3").arg(entry.name).arg(entry.position.x()).arg(entry.position.y());
        args << QStringLiteral("output.%1.priority.%2").arg(entry.name).arg(priority++);
    }
    return args;
}

/** Only the disables of \a entries (the first step of the two-step form). */
inline QStringList arrangementDisableArgs(const QList<Arrangement> &entries)
{
    QStringList args;
    for (const auto &entry : entries) {
        if (!entry.enabled) {
            args << QStringLiteral("output.%1.disable").arg(entry.name);
        }
    }
    return args;
}

/**
 * The whole arrangement in one invocation: enables, positions and priorities
 * first, then the disables, so KWin never sees a configuration without an
 * enabled output (same shape as replaceArgs()).
 */
inline QStringList arrangementArgs(const QList<Arrangement> &entries)
{
    return arrangementEnableArgs(entries) + arrangementDisableArgs(entries);
}

/** Every entry of \a entries is present in \a current by name, whatever its state. */
inline bool arrangementPresent(const QList<Arrangement> &entries, const QVector<Output> &current)
{
    return std::all_of(entries.cbegin(), entries.cend(), [&current](const Arrangement &entry) {
        return std::any_of(current.cbegin(), current.cend(), [&entry](const Output &candidate) {
            return candidate.name == entry.name;
        });
    });
}

/**
 * Every entry is present with the wanted enabled state, and every enabled
 * one at its position. Priorities are not compared: KWin renumbers them as
 * it sees fit, and nothing here depends on them.
 */
inline bool arrangementMatches(const QList<Arrangement> &entries, const QVector<Output> &current)
{
    return std::all_of(entries.cbegin(), entries.cend(), [&current](const Arrangement &entry) {
        const auto it = std::find_if(current.cbegin(), current.cend(), [&entry](const Output &candidate) {
            return candidate.name == entry.name;
        });
        if (it == current.cend() || it->enabled != entry.enabled) {
            return false;
        }
        return !entry.enabled || it->position == entry.position;
    });
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

/** Every physical output of \a snapshot is present in \a current by name, whatever its state. */
inline bool allPresent(const QVector<Output> &snapshot, const QVector<Output> &current)
{
    return std::all_of(snapshot.cbegin(), snapshot.cend(), [&current](const Output &wanted) {
        return isVirtual(wanted.name) || std::any_of(current.cbegin(), current.cend(), [&wanted](const Output &candidate) {
                   return candidate.name == wanted.name;
               });
    });
}

/** The entries of \a snapshot (as snapshotted) whose output is present in \a current by name. */
inline QVector<Output> presentSubset(const QVector<Output> &snapshot, const QVector<Output> &current)
{
    QVector<Output> present;
    for (const auto &wanted : snapshot) {
        if (std::any_of(current.cbegin(), current.cend(), [&wanted](const Output &candidate) {
                return candidate.name == wanted.name;
            })) {
            present.push_back(wanted);
        }
    }
    return present;
}

/** The entries of \a snapshot whose output is absent from \a current: unplugged, or being re-added. */
inline QVector<Output> missingSubset(const QVector<Output> &snapshot, const QVector<Output> &current)
{
    QVector<Output> missing;
    for (const auto &wanted : snapshot) {
        if (std::none_of(current.cbegin(), current.cend(), [&wanted](const Output &candidate) {
                return candidate.name == wanted.name;
            })) {
            missing.push_back(wanted);
        }
    }
    return missing;
}

/** The output names, for logs. */
inline QStringList names(const QVector<Output> &outputs)
{
    QStringList list;
    list.reserve(outputs.size());
    for (const auto &output : outputs) {
        list.push_back(output.name);
    }
    return list;
}

/**
 * Every physical output of \a snapshot is present in \a current AND disabled:
 * what a replace has to read back before it may call itself applied. An
 * output the compositor is re-adding is absent, not disabled, and a read-back
 * with no physical output at all is that churn, not a replace.
 */
inline bool allPresentAndDisabled(const QVector<Output> &snapshot, const QVector<Output> &current)
{
    return std::all_of(snapshot.cbegin(), snapshot.cend(), [&current](const Output &wanted) {
        if (isVirtual(wanted.name)) {
            return true;
        }
        const auto it = std::find_if(current.cbegin(), current.cend(), [&wanted](const Output &candidate) {
            return candidate.name == wanted.name;
        });
        return it != current.cend() && !it->enabled;
    });
}

inline QJsonArray toJsonArray(const QVector<Output> &outputs)
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
            {QLatin1String("scale"), output.scale},
        });
    }
    return array;
}

inline QVector<Output> fromJsonArray(const QJsonArray &array)
{
    QVector<Output> outputs;
    for (const auto &entry : array) {
        const auto object = entry.toObject();
        outputs.push_back(Output{
            .name = object.value(QLatin1String("name")).toString(),
            .enabled = object.value(QLatin1String("enabled")).toBool(false),
            .position = QPoint(object.value(QLatin1String("x")).toInt(), object.value(QLatin1String("y")).toInt()),
            .priority = object.value(QLatin1String("priority")).toInt(),
            .size = QSize(object.value(QLatin1String("width")).toInt(), object.value(QLatin1String("height")).toInt()),
            // Absent from state files written before the field existed.
            .scale = object.value(QLatin1String("scale")).toDouble(1.0),
        });
    }
    return outputs;
}

inline QByteArray toJson(const QVector<Output> &outputs)
{
    return QJsonDocument(toJsonArray(outputs)).toJson(QJsonDocument::Compact);
}

inline QVector<Output> fromJson(const QByteArray &json)
{
    const auto doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) {
        return {};
    }
    return fromJsonArray(doc.array());
}

/**
 * The crash-recovery state file: the physical outputs plus the PID of the
 * krdpserver that replaced them, so another server start can tell a live
 * session's file from a dead one's.
 */
inline QByteArray toStateJson(const QVector<Output> &outputs, qint64 ownerPid)
{
    return QJsonDocument(QJsonObject{
                             {QLatin1String("pid"), ownerPid},
                             {QLatin1String("outputs"), toJsonArray(outputs)},
                         })
        .toJson(QJsonDocument::Compact);
}

/**
 * Inverse of toStateJson(); also accepts the bare array toJson() writes
 * (owner 0). Empty on malformed input.
 */
inline QVector<Output> fromStateJson(const QByteArray &json, qint64 *ownerPid = nullptr)
{
    if (ownerPid) {
        *ownerPid = 0;
    }
    const auto doc = QJsonDocument::fromJson(json);
    if (doc.isArray()) {
        return fromJsonArray(doc.array());
    }
    if (!doc.isObject()) {
        return {};
    }
    const auto object = doc.object();
    if (ownerPid) {
        *ownerPid = object.value(QLatin1String("pid")).toInteger(0);
    }
    return fromJsonArray(object.value(QLatin1String("outputs")).toArray());
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
