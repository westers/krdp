// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "LayoutControl.h"

#include <algorithm>
#include <utility>

#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>
#include <QVector>

namespace KRdp
{
namespace LayoutControl
{
namespace
{
constexpr qint64 MaxFrameBytes = 64 * 1024;

QJsonObject sizeToJson(const QSize &size)
{
    return QJsonObject{
        {QStringLiteral("w"), size.width()},
        {QStringLiteral("h"), size.height()},
    };
}

std::optional<QSize> sizeFromJson(const QJsonValue &value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto object = value.toObject();
    if (!object.contains(QStringLiteral("w")) || !object.contains(QStringLiteral("h"))) {
        return std::nullopt;
    }
    return QSize(object.value(QStringLiteral("w")).toInt(), object.value(QStringLiteral("h")).toInt());
}

QJsonObject pointToJson(const QPoint &point)
{
    return QJsonObject{
        {QStringLiteral("x"), point.x()},
        {QStringLiteral("y"), point.y()},
    };
}

std::optional<QPoint> pointFromJson(const QJsonValue &value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto object = value.toObject();
    if (!object.contains(QStringLiteral("x")) || !object.contains(QStringLiteral("y"))) {
        return std::nullopt;
    }
    return QPoint(object.value(QStringLiteral("x")).toInt(), object.value(QStringLiteral("y")).toInt());
}

QString kindToString(Kind kind)
{
    return kind == Kind::Real ? QStringLiteral("real") : QStringLiteral("virtual");
}

std::optional<Kind> kindFromString(const QString &string)
{
    if (string == QStringLiteral("real")) {
        return Kind::Real;
    }
    if (string == QStringLiteral("virtual")) {
        return Kind::Virtual;
    }
    return std::nullopt;
}

QJsonObject monitorToJson(const HostMonitor &monitor)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), monitor.id);
    object.insert(QStringLiteral("name"), monitor.name);
    object.insert(QStringLiteral("kind"), kindToString(monitor.kind));
    object.insert(QStringLiteral("size"), sizeToJson(monitor.size));
    object.insert(QStringLiteral("position"), pointToJson(monitor.position));
    object.insert(QStringLiteral("scale"), monitor.scale);
    object.insert(QStringLiteral("primary"), monitor.primary);
    object.insert(QStringLiteral("lit"), monitor.lit);
    object.insert(QStringLiteral("standIn"), monitor.standIn);
    if (monitor.standIn && monitor.standInSize) {
        object.insert(QStringLiteral("standInSize"), sizeToJson(*monitor.standInSize));
    }
    if (monitor.standIn && monitor.standInScale) {
        object.insert(QStringLiteral("standInScale"), *monitor.standInScale);
    }
    object.insert(QStringLiteral("owner"), monitor.owner.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(monitor.owner));
    return object;
}

std::optional<HostMonitor> monitorFromJson(const QJsonValue &value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto object = value.toObject();
    if (!object.contains(QStringLiteral("id")) || !object.contains(QStringLiteral("kind"))) {
        return std::nullopt;
    }
    const auto kind = kindFromString(object.value(QStringLiteral("kind")).toString());
    const auto size = sizeFromJson(object.value(QStringLiteral("size")));
    const auto position = pointFromJson(object.value(QStringLiteral("position")));
    if (!kind || !size || !position) {
        return std::nullopt;
    }

    HostMonitor monitor;
    monitor.id = object.value(QStringLiteral("id")).toString();
    monitor.name = object.value(QStringLiteral("name")).toString();
    monitor.kind = *kind;
    monitor.size = *size;
    monitor.position = *position;
    monitor.scale = object.value(QStringLiteral("scale")).toDouble(1.0);
    monitor.primary = object.value(QStringLiteral("primary")).toBool(false);
    monitor.lit = object.value(QStringLiteral("lit")).toBool(true);
    monitor.standIn = object.value(QStringLiteral("standIn")).toBool(false);
    if (object.contains(QStringLiteral("standInSize"))) {
        monitor.standInSize = sizeFromJson(object.value(QStringLiteral("standInSize")));
    }
    if (object.contains(QStringLiteral("standInScale"))) {
        monitor.standInScale = object.value(QStringLiteral("standInScale")).toDouble();
    }
    const auto owner = object.value(QStringLiteral("owner"));
    monitor.owner = owner.isString() ? owner.toString() : QString();
    return monitor;
}

QJsonObject capsToJson(const Caps &caps)
{
    return QJsonObject{
        {QStringLiteral("maxOutputPx"), caps.maxOutputPx},
        {QStringLiteral("maxUnionPx"), caps.maxUnionPx},
        {QStringLiteral("cursorMetadata"), caps.cursorMetadata},
    };
}

Caps capsFromJson(const QJsonValue &value)
{
    Caps caps;
    if (!value.isObject()) {
        return caps;
    }
    const auto object = value.toObject();
    caps.maxOutputPx = object.value(QStringLiteral("maxOutputPx")).toInt(caps.maxOutputPx);
    caps.maxUnionPx = object.value(QStringLiteral("maxUnionPx")).toInt(caps.maxUnionPx);
    caps.cursorMetadata = object.value(QStringLiteral("cursorMetadata")).toBool(caps.cursorMetadata);
    return caps;
}

QJsonObject applyMonitorToJson(const ApplyMonitor &monitor)
{
    QJsonObject object;
    if (monitor.isNew) {
        object.insert(QStringLiteral("new"), true);
    } else {
        object.insert(QStringLiteral("id"), monitor.id);
    }
    if (monitor.lit) {
        object.insert(QStringLiteral("lit"), *monitor.lit);
    }
    if (monitor.size) {
        object.insert(QStringLiteral("size"), sizeToJson(*monitor.size));
    }
    if (monitor.scale) {
        object.insert(QStringLiteral("scale"), *monitor.scale);
    }
    return object;
}

std::optional<ApplyMonitor> applyMonitorFromJson(const QJsonValue &value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto object = value.toObject();

    ApplyMonitor monitor;
    if (object.value(QStringLiteral("new")).toBool(false)) {
        monitor.isNew = true;
    } else if (object.contains(QStringLiteral("id"))) {
        monitor.id = object.value(QStringLiteral("id")).toString();
    } else {
        return std::nullopt;
    }
    if (object.contains(QStringLiteral("lit"))) {
        monitor.lit = object.value(QStringLiteral("lit")).toBool();
    }
    if (object.contains(QStringLiteral("size"))) {
        const auto size = sizeFromJson(object.value(QStringLiteral("size")));
        if (!size) {
            return std::nullopt;
        }
        monitor.size = size;
    }
    if (object.contains(QStringLiteral("scale"))) {
        monitor.scale = object.value(QStringLiteral("scale")).toDouble();
    }
    return monitor;
}

/** The bounding union, in host desktop coordinates, of \a monitors. Invalid (null) when empty. */
QRect unionOf(const QList<HostMonitor> &monitors)
{
    QRect result;
    for (const auto &monitor : monitors) {
        result |= QRect(monitor.position, monitor.size);
    }
    return result;
}

/** The lowest n >= 1 not already used by a "virtual-<n>" id in \a monitors. */
int lowestFreeVirtualNumber(const QList<HostMonitor> &monitors)
{
    QSet<int> used;
    static const QString prefix = QStringLiteral("virtual-");
    for (const auto &monitor : monitors) {
        if (monitor.kind == Kind::Virtual && monitor.id.startsWith(prefix)) {
            bool ok = false;
            const int n = monitor.id.mid(prefix.size()).toInt(&ok);
            if (ok) {
                used.insert(n);
            }
        }
    }
    int n = 1;
    while (used.contains(n)) {
        ++n;
    }
    return n;
}

// Deliberately not ClientDisplay::usable()/usableDesktop(): those also
// enforce a 640px MinDimension floor for RDP clients, which nothing in the
// LayoutControl planner rules asks for (a small stand-in or virtual monitor
// is not itself a protocol violation), so the bound comparisons are
// re-implemented here rather than reusing the floor along with the ceiling.
std::optional<Error> sanitizeResult(const QList<HostMonitor> &monitors, const Caps &caps)
{
    for (const auto &monitor : monitors) {
        if (monitor.size.width() > caps.maxOutputPx || monitor.size.height() > caps.maxOutputPx) {
            return Error{QStringLiteral("invalid"), QStringLiteral("monitor %1 exceeds the %2px output limit").arg(monitor.id).arg(caps.maxOutputPx)};
        }
    }

    const QRect union_ = unionOf(monitors);
    if (union_.width() > caps.maxUnionPx || union_.height() > caps.maxUnionPx) {
        return Error{QStringLiteral("invalid"), QStringLiteral("layout union exceeds the %1px limit").arg(caps.maxUnionPx)};
    }

    QVector<VideoMonitor> asVideoMonitors;
    asVideoMonitors.reserve(monitors.size());
    for (const auto &monitor : monitors) {
        asVideoMonitors.push_back(VideoMonitor{.geometry = QRect(monitor.position, monitor.size), .primary = monitor.primary});
    }
    if (!ClientDisplay::disjoint(asVideoMonitors)) {
        return Error{QStringLiteral("invalid"), QStringLiteral("monitors overlap")};
    }

    const auto primaryCount = std::count_if(monitors.cbegin(), monitors.cend(), [](const HostMonitor &monitor) {
        return monitor.primary;
    });
    if (primaryCount != 1) {
        return Error{QStringLiteral("invalid"), QStringLiteral("layout must have exactly one primary monitor")};
    }

    return std::nullopt;
}

}

QJsonObject toJson(const Layout &layout)
{
    QJsonArray monitors;
    for (const auto &monitor : layout.monitors) {
        monitors.append(monitorToJson(monitor));
    }

    QJsonObject object;
    object.insert(QStringLiteral("monitors"), monitors);
    object.insert(QStringLiteral("owner"), layout.owner.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(layout.owner));
    object.insert(QStringLiteral("you"), layout.you);
    object.insert(QStringLiteral("caps"), capsToJson(layout.caps));
    return object;
}

std::optional<Layout> layoutFromJson(const QJsonObject &object)
{
    if (!object.value(QStringLiteral("monitors")).isArray()) {
        return std::nullopt;
    }

    Layout layout;
    for (const auto &value : object.value(QStringLiteral("monitors")).toArray()) {
        const auto monitor = monitorFromJson(value);
        if (!monitor) {
            return std::nullopt;
        }
        layout.monitors.append(*monitor);
    }
    const auto owner = object.value(QStringLiteral("owner"));
    layout.owner = owner.isString() ? owner.toString() : QString();
    layout.you = object.value(QStringLiteral("you")).toString();
    layout.caps = capsFromJson(object.value(QStringLiteral("caps")));
    return layout;
}

QJsonObject toJson(const ApplyRequest &request)
{
    QJsonObject object;
    object.insert(QStringLiteral("private"), request.privateMode);
    if (request.takeoverLayout) {
        object.insert(QStringLiteral("takeoverLayout"), true);
    }
    QJsonArray monitors;
    for (const auto &monitor : request.monitors) {
        monitors.append(applyMonitorToJson(monitor));
    }
    object.insert(QStringLiteral("monitors"), monitors);
    return object;
}

std::optional<ApplyRequest> applyFromJson(const QJsonObject &object)
{
    if (!object.value(QStringLiteral("monitors")).isArray()) {
        return std::nullopt;
    }

    ApplyRequest request;
    request.privateMode = object.value(QStringLiteral("private")).toBool(false);
    request.takeoverLayout = object.value(QStringLiteral("takeoverLayout")).toBool(false);
    for (const auto &value : object.value(QStringLiteral("monitors")).toArray()) {
        const auto monitor = applyMonitorFromJson(value);
        if (!monitor) {
            return std::nullopt;
        }
        request.monitors.append(*monitor);
    }
    return request;
}

std::optional<ChromaRequest> chromaFromJson(const QJsonObject &object)
{
    ChromaRequest request;
    const std::pair<QString, std::optional<int> *> fields[] = {
        {QStringLiteral("motionGapMs"), &request.motionGapMs},
        {QStringLiteral("restMs"), &request.restMs},
        {QStringLiteral("maxGapMs"), &request.maxGapMs},
    };
    for (const auto &[key, target] : fields) {
        if (!object.contains(key)) {
            continue;
        }
        const auto value = object.value(key);
        if (!value.isDouble()) {
            return std::nullopt;
        }
        *target = value.toInt();
    }
    return request;
}

QJsonObject errorRecord(const Error &error)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("error")},
        {QStringLiteral("code"), error.code},
        {QStringLiteral("message"), error.message},
    };
}

QJsonObject layoutRecord(const Layout &layout)
{
    QJsonObject object = toJson(layout);
    object.insert(QStringLiteral("type"), QStringLiteral("layout"));
    object.insert(QStringLiteral("audioPriority"), true);
    return object;
}

QJsonObject takeoverRecord(const Layout &layout)
{
    QJsonObject object = toJson(layout);
    object.insert(QStringLiteral("type"), QStringLiteral("takeover"));
    object.insert(QStringLiteral("audioPriority"), true);
    return object;
}

QByteArray frame(const QJsonObject &record)
{
    QJsonObject withVersion = record;
    withVersion.insert(QStringLiteral("v"), ProtocolVersion);
    const QByteArray payload = QJsonDocument(withVersion).toJson(QJsonDocument::Compact);

    QByteArray out;
    QDataStream stream(&out, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << static_cast<quint32>(payload.size());
    out.append(payload);
    return out;
}

void Deframer::feed(const QByteArray &data)
{
    if (m_overflowed) {
        return;
    }
    m_buffer.append(data);
}

std::optional<QJsonObject> Deframer::next()
{
    for (;;) {
        if (m_overflowed || m_buffer.size() < 4) {
            return std::nullopt;
        }

        QDataStream stream(m_buffer);
        stream.setByteOrder(QDataStream::BigEndian);
        quint32 length = 0;
        stream >> length;

        if (length > MaxFrameBytes) {
            m_overflowed = true;
            m_buffer.clear();
            return std::nullopt;
        }
        if (m_buffer.size() < 4 + static_cast<qsizetype>(length)) {
            return std::nullopt;
        }

        const QByteArray payload = m_buffer.mid(4, length);
        m_buffer.remove(0, 4 + length);

        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (!document.isObject()) {
            // Framed correctly, so the stream stays in sync; the payload is
            // just not a record. Skip it and look at the next one.
            ++m_invalid;
            continue;
        }
        return document.object();
    }
}

bool Deframer::overflowed() const
{
    return m_overflowed;
}

int Deframer::takeInvalidCount()
{
    return std::exchange(m_invalid, 0);
}

std::variant<Plan, Error> plan(const Layout &current, const ApplyRequest &request, const QString &requester, const Caps &caps)
{
    QHash<QString, const ApplyMonitor *> mentioned;
    for (const auto &entry : request.monitors) {
        if (!entry.isNew) {
            mentioned.insert(entry.id, &entry);
        }
    }

    for (auto it = mentioned.cbegin(); it != mentioned.cend(); ++it) {
        const bool exists = std::any_of(current.monitors.cbegin(), current.monitors.cend(), [&it](const HostMonitor &monitor) {
            return monitor.id == it.key();
        });
        if (!exists) {
            return Error{QStringLiteral("invalid"), QStringLiteral("unknown monitor id: %1").arg(it.key())};
        }
    }

    QList<Action> actions;
    QList<HostMonitor> resultMonitors = current.monitors;

    // Real monitors: stand-in derivation and lit/dark, in current-layout order.
    // Monitors not mentioned at all keep their state entirely (privateMode,
    // below, is the only thing that can still darken them).
    for (auto &monitor : resultMonitors) {
        if (monitor.kind != Kind::Real) {
            continue;
        }

        const auto it = mentioned.constFind(monitor.id);
        if (it == mentioned.cend()) {
            continue;
        }
        const ApplyMonitor &entry = *it.value();

        bool createdStandIn = false;
        std::optional<bool> targetLitOverride = entry.lit;

        if (entry.size && *entry.size != monitor.size) {
            if (entry.size->width() > caps.maxOutputPx || entry.size->height() > caps.maxOutputPx) {
                return Error{QStringLiteral("invalid"), QStringLiteral("stand-in for %1 exceeds the %2px output limit").arg(monitor.id).arg(caps.maxOutputPx)};
            }
            // A scale left out keeps the stand-in's current one (as a virtual
            // monitor's does below); a new stand-in defaults to 1.
            const qreal standInScale = entry.scale.value_or(monitor.standIn && monitor.standInScale ? *monitor.standInScale : 1.0);
            if (monitor.standIn && monitor.standInSize == *entry.size && monitor.standInScale && qFuzzyCompare(*monitor.standInScale, standInScale)) {
                // Already stood in exactly so: a client re-sending its whole
                // mapping (the panel does) changes nothing here. `lit` is
                // implied false by the stand-in, as for a fresh one.
                continue;
            }
            actions.append(Action{
                .kind = ActionKind::CreateStandIn,
                .id = monitor.id,
                .size = *entry.size,
                .position = monitor.position,
                .scale = standInScale,
            });
            monitor.standIn = true;
            monitor.lit = false; // implied by CreateStandIn; no separate DarkenReal
            monitor.standInSize = *entry.size;
            monitor.standInScale = standInScale;
            createdStandIn = true;
        } else if (monitor.standIn) {
            // Either no size was given, or the native size was re-sent: both
            // mean "go back to native" (a size that actually differs from
            // native was handled by the CreateStandIn branch above).
            actions.append(Action{
                .kind = ActionKind::RemoveStandIn,
                .id = monitor.id,
                .size = monitor.size,
                .position = monitor.position,
                .scale = 1.0,
            });
            monitor.standIn = false;
            monitor.standInSize.reset();
            monitor.standInScale.reset();
            targetLitOverride = entry.lit.value_or(true);
        }

        if (!createdStandIn) {
            const bool targetLit = targetLitOverride.value_or(monitor.lit);
            if (targetLit != monitor.lit) {
                actions.append(Action{
                    .kind = targetLit ? ActionKind::LightReal : ActionKind::DarkenReal,
                    .id = monitor.id,
                    .size = monitor.size,
                    .position = monitor.position,
                    .scale = 1.0,
                });
                monitor.lit = targetLit;
            }
        }
    }

    if (request.privateMode) {
        for (auto &monitor : resultMonitors) {
            if (monitor.kind == Kind::Real && monitor.lit) {
                actions.append(Action{
                    .kind = ActionKind::DarkenReal,
                    .id = monitor.id,
                    .size = monitor.size,
                    .position = monitor.position,
                    .scale = 1.0,
                });
                monitor.lit = false;
            }
        }
    }

    // Existing virtual monitors: the requester's own are removed when left
    // out of the apply, or replanned in place (RemoveVirtual + CreateVirtual
    // at the same id/position) when a size/scale change is requested; a
    // no-op mention (same size/scale, or neither given) leaves it untouched.
    // Mentioning another connection's virtual monitor is refused outright.
    for (auto it = resultMonitors.begin(); it != resultMonitors.end();) {
        if (it->kind != Kind::Virtual) {
            ++it;
            continue;
        }

        const auto mentionIt = mentioned.constFind(it->id);
        if (mentionIt == mentioned.cend()) {
            if (it->owner == requester) {
                actions.append(Action{
                    .kind = ActionKind::RemoveVirtual,
                    .id = it->id,
                    .size = it->size,
                    .position = it->position,
                    .scale = it->scale,
                });
                it = resultMonitors.erase(it);
                continue;
            }
            ++it;
            continue;
        }

        if (it->owner != requester) {
            return Error{QStringLiteral("invalid"), QStringLiteral("%1 belongs to another client").arg(it->id)};
        }

        const ApplyMonitor &entry = *mentionIt.value();
        const QSize newSize = entry.size.value_or(it->size);
        const qreal newScale = entry.scale.value_or(it->scale);
        if (newSize != it->size || newScale != it->scale) {
            if (newSize.width() > caps.maxOutputPx || newSize.height() > caps.maxOutputPx) {
                return Error{QStringLiteral("invalid"), QStringLiteral("%1 would exceed the %2px output limit").arg(it->id).arg(caps.maxOutputPx)};
            }
            actions.append(Action{
                .kind = ActionKind::RemoveVirtual,
                .id = it->id,
                .size = it->size,
                .position = it->position,
                .scale = it->scale,
            });
            actions.append(Action{
                .kind = ActionKind::CreateVirtual,
                .id = it->id,
                .size = newSize,
                .position = it->position,
                .scale = newScale,
            });
            it->size = newSize;
            it->scale = newScale;
        }
        ++it;
    }

    // New virtual monitors, placed left-to-right along the union as it stood
    // before this apply, extended by each one already placed in this request.
    QRect placementUnion = unionOf(current.monitors);
    for (const auto &entry : request.monitors) {
        if (!entry.isNew) {
            continue;
        }
        if (!entry.size) {
            return Error{QStringLiteral("invalid"), QStringLiteral("a new monitor needs a size")};
        }
        if (entry.size->width() > caps.maxOutputPx || entry.size->height() > caps.maxOutputPx) {
            return Error{QStringLiteral("invalid"), QStringLiteral("new monitor exceeds the %1px output limit").arg(caps.maxOutputPx)};
        }

        const int n = lowestFreeVirtualNumber(resultMonitors);
        const QString id = QStringLiteral("virtual-%1").arg(n);
        const QPoint position(placementUnion.isValid() ? placementUnion.right() + 1 : 0, 0);
        const qreal scale = entry.scale.value_or(1.0);

        actions.append(Action{
            .kind = ActionKind::CreateVirtual,
            .id = id,
            .size = *entry.size,
            .position = position,
            .scale = scale,
        });
        resultMonitors.append(HostMonitor{
            .id = id,
            .name = id,
            .kind = Kind::Virtual,
            .size = *entry.size,
            .position = position,
            .scale = scale,
            .primary = false,
            .lit = true,
            .standIn = false,
            .standInSize = {},
            .standInScale = {},
            .owner = requester,
        });
        placementUnion |= QRect(position, *entry.size);
    }

    if (const auto error = sanitizeResult(resultMonitors, caps)) {
        return *error;
    }

    Layout resulting;
    resulting.monitors = resultMonitors;
    resulting.owner = requester;
    resulting.you = QStringLiteral("owner");
    resulting.caps = caps;

    return Plan{actions, resulting};
}

}
}
