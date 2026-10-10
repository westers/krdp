// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "LayoutControl.h"

#include <QRegularExpression>
#include "RemoteMonitorGeometry.h"

#include <algorithm>
#include <cmath>
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
        const QSize size = monitor.kind == Kind::Real && monitor.standIn && monitor.standInSize ? *monitor.standInSize : monitor.size;
        const qreal scale = monitor.kind == Kind::Real && monitor.standIn && monitor.standInScale ? *monitor.standInScale : monitor.scale;
        if (monitor.size.isEmpty() || size.isEmpty() || !std::isfinite(scale) || scale <= 0.0
            || size.width() / scale > caps.maxUnionPx || size.height() / scale > caps.maxUnionPx) {
            return Error{QStringLiteral("invalid"), QStringLiteral("monitor %1 has invalid size or scale").arg(monitor.id)};
        }
        if (size.width() > caps.maxOutputPx || size.height() > caps.maxOutputPx) {
            return Error{QStringLiteral("invalid"), QStringLiteral("monitor %1 exceeds the %2px output limit").arg(monitor.id).arg(caps.maxOutputPx)};
        }
    }

    QVector<VideoMonitor> asVideoMonitors;
    QVector<RemoteMonitorGeometry::Output> wireOutputs;
    asVideoMonitors.reserve(monitors.size());
    wireOutputs.reserve(monitors.size());
    for (const auto &monitor : monitors) {
        const QSize size = monitor.kind == Kind::Real && monitor.standIn && monitor.standInSize ? *monitor.standInSize : monitor.size;
        const qreal scale = monitor.kind == Kind::Real && monitor.standIn && monitor.standInScale ? *monitor.standInScale : monitor.scale;
        asVideoMonitors.push_back(VideoMonitor{.geometry = RemoteMonitorGeometry::logicalRect(monitor.position, size, scale), .primary = monitor.primary});
        wireOutputs.push_back(RemoteMonitorGeometry::Output{monitor.position, size, scale, monitor.primary});
    }
    if (!ClientDisplay::disjoint(asVideoMonitors)) {
        return Error{QStringLiteral("invalid"), QStringLiteral("monitors overlap")};
    }
    QRect wireUnion;
    for (const auto &entry : RemoteMonitorGeometry::projectToWire(wireOutputs)) {
        wireUnion |= entry.geometry;
    }
    if (wireUnion.width() > caps.maxUnionPx || wireUnion.height() > caps.maxUnionPx) {
        return Error{QStringLiteral("invalid"), QStringLiteral("layout union exceeds the %1px limit").arg(caps.maxUnionPx)};
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

RequestId takeRequestId(QJsonObject &record)
{
    RequestId result;
    const auto found = record.constFind(QStringLiteral("requestId"));
    if (found == record.constEnd()) {
        return result;
    }
    static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    const QJsonValue value = *found;
    record.remove(QStringLiteral("requestId"));
    const QJsonValue id = record.value(QStringLiteral("id"));
    if (!value.isString() || !token.match(value.toString()).hasMatch() || (!id.isUndefined() && id != value)) {
        result.invalid = true;
        return result;
    }
    result.value = value.toString();
    return result;
}

QJsonObject withRequestId(QJsonObject reply, const QString &requestId)
{
    if (!requestId.isEmpty()) {
        reply.insert(QStringLiteral("requestId"), requestId);
    }
    return reply;
}

QJsonObject invalidRequestIdRecord()
{
    return errorRecord({QStringLiteral("invalid"), QStringLiteral("invalid requestId")});
}

QJsonObject capabilitiesRecord(const ChannelCapabilities &capabilities)
{
    QJsonObject record{
        {QStringLiteral("type"), QStringLiteral("capabilities")},
        {QStringLiteral("v"), ProtocolVersion},
        {QStringLiteral("protocol"), ChannelProtocol},
        {QStringLiteral("host"), capabilities.host},
        {QStringLiteral("layout"), QJsonObject{{QStringLiteral("query"), capabilities.layoutQuery}, {QStringLiteral("apply"), capabilities.layoutApply}}},
        {QStringLiteral("virtualSessions"),
         QJsonObject{{QStringLiteral("list"), capabilities.virtualList},
                     {QStringLiteral("create"), capabilities.virtualCreate},
                     {QStringLiteral("selectedCreate"), capabilities.virtualSelectedCreate}}},
        {QStringLiteral("topology"),
         QJsonObject{{QStringLiteral("query"), capabilities.topologyQuery},
                     {QStringLiteral("preview"), capabilities.topologyPreview},
                     {QStringLiteral("apply"), capabilities.topologyApply}}},
    };
    if (capabilities.pointerCaptureSync) record.insert(QStringLiteral("pointerCaptureSync"), true);
    if (const auto &devices = capabilities.devices) {
        QJsonObject camera{{QStringLiteral("toggle"), devices->cameraToggle}, {QStringLiteral("reselect"), devices->cameraReselect}};
        if (!devices->cameraUnavailableReason.isEmpty())
            camera.insert(QStringLiteral("unavailableReason"), devices->cameraUnavailableReason.left(1024));
        QJsonObject group{
                          {QStringLiteral("playback"),
                           QJsonObject{{QStringLiteral("toggle"), devices->playbackToggle}, {QStringLiteral("silenceHost"), devices->playbackSilenceHost}}},
                          {QStringLiteral("microphone"), QJsonObject{{QStringLiteral("toggle"), devices->microphoneToggle}}},
                          {QStringLiteral("camera"), camera},
                      };
        if (devices->availabilityPush)
            group.insert(QStringLiteral("availability"),
                         QJsonObject{{QStringLiteral("push"), true},
                                     {QStringLiteral("camera"), devices->cameraSessionAvailable},
                                     {QStringLiteral("microphone"), devices->microphoneSessionAvailable}});
        record.insert(QStringLiteral("devices"), group);
    }
    if (const auto &video = capabilities.video) {
        QJsonArray codecs;
        for (const auto &offer : video->codecs) {
            codecs.append(QJsonObject{{QStringLiteral("name"), offer.name}, {QStringLiteral("hw"), offer.hardware}, {QStringLiteral("sw"), offer.software}});
        }
        QJsonObject group{{QStringLiteral("codecs"), codecs}, {QStringLiteral("softwareEncoding"), video->softwareEncoding}};
        if (video->preferences > 0) {
            group.insert(QStringLiteral("preferences"), video->preferences);
        }
        if (!video->softwareAvc.isEmpty() || !video->softwareHevc.isEmpty() || !video->softwareAv1.isEmpty()) {
            group.insert(QStringLiteral("software"),
                         QJsonObject{{QStringLiteral("avc"), video->softwareAvc}, {QStringLiteral("hevc"), video->softwareHevc}, {QStringLiteral("av1"), video->softwareAv1}});
        }
        if (!video->encoders.isEmpty()) {
            QJsonArray encoders;
            for (const auto &encoder : video->encoders.first(std::min<qsizetype>(video->encoders.size(), VideoCapabilities::MaxEncoders))) {
                QJsonObject entry{{QStringLiteral("codec"), encoder.codec}, {QStringLiteral("backend"), encoder.backend}, {QStringLiteral("hw"), encoder.hardware}};
                if (!encoder.device.isEmpty()) entry.insert(QStringLiteral("device"), encoder.device.left(16));
                if (!encoder.name.isEmpty()) entry.insert(QStringLiteral("name"), encoder.name.left(48));
                encoders.append(entry);
            }
            group.insert(QStringLiteral("encoders"), encoders);
        }
        record.insert(QStringLiteral("video"), group);
    }
    if (const auto &stats = capabilities.stats) {
        record.insert(QStringLiteral("stats"),
                      QJsonObject{{QStringLiteral("maxRateHz"), stats->maxRateHz}, {QStringLiteral("events"), stats->events}, {QStringLiteral("tcp"), stats->tcp}});
    }
    if (const auto &screens = capabilities.consoleScreens) {
        record.insert(QStringLiteral("console"),
                      QJsonObject{{QStringLiteral("screens"),
                                   QJsonObject{{QStringLiteral("replace"), screens->replace},
                                                {QStringLiteral("restore"), screens->restore},
                                                {QStringLiteral("request"), screens->request}}}});
        auto group = record.value(QStringLiteral("console")).toObject();
        auto inner = group.value(QStringLiteral("screens")).toObject();
        if (screens->mapped) inner.insert(QStringLiteral("mapped"), true);
        if (screens->maxScreens > 0) inner.insert(QStringLiteral("maxScreens"), screens->maxScreens);
        if (screens->view) inner.insert(QStringLiteral("view"), true);
        group.insert(QStringLiteral("screens"), inner);
        record.insert(QStringLiteral("console"), group);
    }
    return record;
}

QJsonObject consoleScreensRecord(bool active, bool canRestore, const QString &reason, const ConsoleScreensDetail &detail)
{
    QJsonObject record{
        {QStringLiteral("type"), QStringLiteral("console-screens")},
        {QStringLiteral("v"), ProtocolVersion},
        {QStringLiteral("active"), active},
        {QStringLiteral("canRestore"), canRestore},
    };
    static const QStringList known{QStringLiteral("connect"),    QStringLiteral("deskInput"), QStringLiteral("restoreRequest"), QStringLiteral("workerExit"),
                                   QStringLiteral("disconnect"), QStringLiteral("failed"),    QStringLiteral("lockRestore"),     QStringLiteral("tooManyScreens"), QStringLiteral("hostScreensChanged")};
    if (known.contains(reason)) {
        record.insert(QStringLiteral("reason"), reason);
    }
    if (!detail.layout.isEmpty()) {
        record.insert(QStringLiteral("layout"), detail.layout);
    }
    if (!detail.screens.isEmpty()) {
        QJsonArray screens;
        for (const auto &screen : detail.screens) {
            QJsonObject entry{{QStringLiteral("output"), screen.output},
                              {QStringLiteral("monitor"), screen.clientMonitor},
                              {QStringLiteral("width"), screen.width},
                              {QStringLiteral("height"), screen.height},
                              {QStringLiteral("scale"), screen.scale},
                              {QStringLiteral("primary"), screen.primary},
                              {QStringLiteral("default"), screen.isDefault},
                              {QStringLiteral("clamped"), screen.clamped}};
            if (screen.surface >= 0) {
                entry.insert(QStringLiteral("surface"), screen.surface);
            }
            if (!screen.hostOutput.isEmpty()) {
                entry.insert(QStringLiteral("host"), screen.hostOutput);
            }
            screens.append(entry);
        }
        record.insert(QStringLiteral("screens"), screens);
    }
    if (!detail.unmappedMonitors.isEmpty()) {
        record.insert(QStringLiteral("unmappedMonitors"), QJsonArray::fromStringList(detail.unmappedMonitors));
    }
    if (!detail.unknownHosts.isEmpty()) {
        record.insert(QStringLiteral("unknownHosts"), QJsonArray::fromStringList(detail.unknownHosts));
    }
    if (!detail.message.isEmpty()) {
        record.insert(QStringLiteral("message"), detail.message.left(300));
    }
    return record;
}

std::optional<QVector<VideoMonitor>> parseConsoleScreensRequest(const QJsonObject &record)
{
    static const QSet<QString> keys{QStringLiteral("type"), QStringLiteral("v"), QStringLiteral("replace"), QStringLiteral("monitors")};
    static const QSet<QString> monitorKeys{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("primary")};
    for (auto it = record.begin(); it != record.end(); ++it) {
        if (!keys.contains(it.key())) return std::nullopt;
    }
    if (record.value(QStringLiteral("type")) != QJsonValue(QStringLiteral("console-screens-request")) || record.value(QStringLiteral("v")) != QJsonValue(1)
        || record.value(QStringLiteral("replace")) != QJsonValue(true) || !record.value(QStringLiteral("monitors")).isArray()) {
        return std::nullopt;
    }
    const auto array = record.value(QStringLiteral("monitors")).toArray();
    if (array.isEmpty() || array.size() > ClientDisplay::MaxMonitors) return std::nullopt;
    const auto integer = [](const QJsonValue &value, int minimum, int maximum, int *out) {
        if (!value.isDouble()) return false;
        const double number = value.toDouble();
        if (number != std::floor(number) || number < minimum || number > maximum) return false;
        *out = int(number);
        return true;
    };
    QVector<VideoMonitor> monitors;
    qsizetype primaries = 0;
    for (const auto &entry : array) {
        if (!entry.isObject()) return std::nullopt;
        const auto object = entry.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!monitorKeys.contains(it.key())) return std::nullopt;
        }
        int x = 0, y = 0, width = 0, height = 0;
        if (!integer(object.value(QStringLiteral("x")), -ClientDisplay::MaxCoordinate, ClientDisplay::MaxCoordinate, &x)
            || !integer(object.value(QStringLiteral("y")), -ClientDisplay::MaxCoordinate, ClientDisplay::MaxCoordinate, &y)
            || !integer(object.value(QStringLiteral("width")), ClientDisplay::MinDimension, ClientDisplay::MaxDimension, &width)
            || !integer(object.value(QStringLiteral("height")), ClientDisplay::MinDimension, ClientDisplay::MaxDimension, &height)) {
            return std::nullopt;
        }
        const auto primary = object.value(QStringLiteral("primary"));
        if (!primary.isUndefined() && !primary.isBool()) return std::nullopt;
        monitors.append(VideoMonitor{QRect(x, y, width, height), primary.toBool(false)});
        primaries += monitors.last().primary ? 1 : 0;
    }
    // One monitor is the primary by definition; several need exactly one, no overlap and a union RDP can carry.
    if (monitors.size() == 1) {
        monitors[0].primary = true;
        return monitors;
    }
    QRect unionRect;
    for (const auto &monitor : std::as_const(monitors)) unionRect |= monitor.geometry;
    if (primaries != 1 || !ClientDisplay::disjoint(monitors) || !ClientDisplay::usableDesktop(unionRect.size())) return std::nullopt;
    return monitors;
}

QJsonObject consoleScreensRequestRecord(const QVector<VideoMonitor> &monitors)
{
    QJsonArray array;
    for (const auto &monitor : monitors) {
        array.append(QJsonObject{{QStringLiteral("x"), monitor.geometry.x()}, {QStringLiteral("y"), monitor.geometry.y()},
                                 {QStringLiteral("width"), monitor.geometry.width()}, {QStringLiteral("height"), monitor.geometry.height()},
                                 {QStringLiteral("primary"), monitor.primary}});
    }
    return {{QStringLiteral("type"), QStringLiteral("console-screens-request")}, {QStringLiteral("v"), ProtocolVersion},
            {QStringLiteral("replace"), true}, {QStringLiteral("monitors"), array}};
}

namespace
{
bool printableName(const QString &name, int maximum)
{
    return !name.isEmpty() && name.size() <= maximum
        && std::none_of(name.cbegin(), name.cend(), [](const QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
}
bool wholeNumber(const QJsonValue &value, int minimum, int maximum, int *out)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (number != std::floor(number) || number < minimum || number > maximum) return false;
    *out = int(number);
    return true;
}
}

std::optional<ConsoleScreensMappedRequest> parseConsoleScreensMappedRequest(const QJsonObject &record, QString *refusal)
{
    const auto fail = [refusal](const char *code) -> std::optional<ConsoleScreensMappedRequest> {
        if (refusal) *refusal = QLatin1String(code);
        return std::nullopt;
    };
    static const QSet<QString> keys{QStringLiteral("type"), QStringLiteral("v"), QStringLiteral("replace"), QStringLiteral("layout"),
                                    QStringLiteral("monitors"), QStringLiteral("mapping")};
    static const QSet<QString> monitorKeys{QStringLiteral("id"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"),
                                           QStringLiteral("height"), QStringLiteral("scale"), QStringLiteral("primary")};
    static const QSet<QString> entryKeys{QStringLiteral("hostOutput"), QStringLiteral("clientMonitor"), QStringLiteral("host"), QStringLiteral("monitor")};
    for (auto it = record.begin(); it != record.end(); ++it) {
        if (!keys.contains(it.key())) return fail("invalid");
    }
    if (record.value(QStringLiteral("type")) != QJsonValue(QStringLiteral("console-screens-request")) || record.value(QStringLiteral("v")) != QJsonValue(2)
        || record.value(QStringLiteral("replace")) != QJsonValue(true) || record.value(QStringLiteral("layout")) != QJsonValue(QStringLiteral("mapped"))
        || !record.value(QStringLiteral("monitors")).isArray()) {
        return fail("invalid");
    }
    const auto monitorArray = record.value(QStringLiteral("monitors")).toArray();
    if (monitorArray.isEmpty() || monitorArray.size() > ClientDisplay::MaxMonitors) return fail("invalid");
    ConsoleScreensMappedRequest request;
    QSet<QString> ids;
    qsizetype primaries = 0;
    for (const auto &entry : monitorArray) {
        if (!entry.isObject()) return fail("invalid");
        const auto object = entry.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!monitorKeys.contains(it.key())) return fail("invalid");
        }
        MappedClientMonitor monitor;
        const auto id = object.value(QStringLiteral("id"));
        int x = 0, y = 0, width = 0, height = 0;
        if (!id.isString() || !printableName(id.toString(), 64) || ids.contains(id.toString())
            || !wholeNumber(object.value(QStringLiteral("x")), -ClientDisplay::MaxCoordinate, ClientDisplay::MaxCoordinate, &x)
            || !wholeNumber(object.value(QStringLiteral("y")), -ClientDisplay::MaxCoordinate, ClientDisplay::MaxCoordinate, &y)
            || !wholeNumber(object.value(QStringLiteral("width")), ClientDisplay::MinDimension, 16384, &width)
            || !wholeNumber(object.value(QStringLiteral("height")), ClientDisplay::MinDimension, 16384, &height)) {
            return fail("invalid");
        }
        const auto scale = object.value(QStringLiteral("scale"));
        if (!scale.isUndefined()) {
            if (!scale.isDouble()) return fail("invalid");
            const double hundredths = scale.toDouble() * 100.0;
            const double rounded = std::round(hundredths);
            if (!std::isfinite(hundredths) || rounded < 100 || rounded > 400 || std::abs(hundredths - rounded) > 1e-6 || (int(rounded) % 5)) return fail("invalid");
            monitor.scalePercent = int(rounded);
        }
        const auto primary = object.value(QStringLiteral("primary"));
        if (!primary.isUndefined() && !primary.isBool()) return fail("invalid");
        monitor.id = id.toString();
        monitor.geometry = QRect(x, y, width, height);
        monitor.primary = primary.toBool(false);
        ids.insert(monitor.id);
        primaries += monitor.primary ? 1 : 0;
        request.monitors.append(monitor);
    }
    if (request.monitors.size() == 1) {
        request.monitors[0].primary = true;
    } else {
        QVector<VideoMonitor> rectangles;
        for (const auto &monitor : std::as_const(request.monitors)) rectangles.append({monitor.geometry, monitor.primary});
        if (primaries != 1 || !ClientDisplay::disjoint(rectangles)) return fail("invalid");
    }
    const auto mapping = record.value(QStringLiteral("mapping"));
    if (!mapping.isUndefined()) {
        if (!mapping.isArray() || mapping.toArray().size() > ClientDisplay::MaxMonitors) return fail("invalid");
        QSet<QString> hosts;
        for (const auto &entry : mapping.toArray()) {
            if (!entry.isObject()) return fail("invalid");
            const auto object = entry.toObject();
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (!entryKeys.contains(it.key())) return fail("invalid");
            }
            // The canonical spelling is host/monitor (spec 3.2, what the client sends); hostOutput/clientMonitor is accepted too, never both.
            if ((object.contains(QStringLiteral("hostOutput")) && object.contains(QStringLiteral("host")))
                || (object.contains(QStringLiteral("clientMonitor")) && object.contains(QStringLiteral("monitor")))) return fail("invalid");
            const auto host = object.contains(QStringLiteral("hostOutput")) ? object.value(QStringLiteral("hostOutput")) : object.value(QStringLiteral("host"));
            const auto monitor = object.contains(QStringLiteral("clientMonitor")) ? object.value(QStringLiteral("clientMonitor")) : object.value(QStringLiteral("monitor"));
            if (!host.isString() || !monitor.isString() || !printableName(host.toString(), 128) || hosts.contains(host.toString())) return fail("invalid");
            if (!ids.contains(monitor.toString())) return fail("unknownMonitor");
            hosts.insert(host.toString());
            request.mapping.append({host.toString(), monitor.toString()});
        }
    }
    return request;
}

QJsonObject consoleScreensMappedRequestRecord(const ConsoleScreensMappedRequest &request)
{
    QJsonArray monitors;
    for (const auto &monitor : request.monitors) {
        monitors.append(QJsonObject{{QStringLiteral("id"), monitor.id}, {QStringLiteral("x"), monitor.geometry.x()}, {QStringLiteral("y"), monitor.geometry.y()},
                                    {QStringLiteral("width"), monitor.geometry.width()}, {QStringLiteral("height"), monitor.geometry.height()},
                                    {QStringLiteral("scale"), monitor.scalePercent / 100.0}, {QStringLiteral("primary"), monitor.primary}});
    }
    QJsonObject record{{QStringLiteral("type"), QStringLiteral("console-screens-request")}, {QStringLiteral("v"), 2}, {QStringLiteral("replace"), true},
                       {QStringLiteral("layout"), QStringLiteral("mapped")}, {QStringLiteral("monitors"), monitors}};
    if (!request.mapping.isEmpty()) {
        QJsonArray mapping;
        for (const auto &entry : request.mapping)
            mapping.append(QJsonObject{{QStringLiteral("host"), entry.hostOutput}, {QStringLiteral("monitor"), entry.clientMonitor}});
        record.insert(QStringLiteral("mapping"), mapping);
    }
    return record;
}

std::optional<ConsoleScreensView> parseConsoleScreensView(const QJsonObject &record)
{
    static const QSet<QString> keys{QStringLiteral("type"), QStringLiteral("v"), QStringLiteral("visible")};
    for (auto it = record.begin(); it != record.end(); ++it) {
        if (!keys.contains(it.key())) return std::nullopt;
    }
    if (record.value(QStringLiteral("type")) != QJsonValue(QStringLiteral("console-screens-view")) || record.value(QStringLiteral("v")) != QJsonValue(1)
        || !record.value(QStringLiteral("visible")).isArray()) {
        return std::nullopt;
    }
    const auto array = record.value(QStringLiteral("visible")).toArray();
    if (array.size() > ClientDisplay::MaxMonitors) return std::nullopt;
    ConsoleScreensView view;
    for (const auto &entry : array) {
        int index = 0;
        if (entry.isString() && printableName(entry.toString(), 128)) {
            view.names.append(entry.toString());
        } else if (wholeNumber(entry, 0, ClientDisplay::MaxMonitors - 1, &index)) {
            view.indices.append(index);
        } else {
            return std::nullopt;
        }
    }
    return view;
}

QJsonObject consoleScreensViewRecord(const QStringList &names)
{
    return {{QStringLiteral("type"), QStringLiteral("console-screens-view")}, {QStringLiteral("v"), 1},
            {QStringLiteral("visible"), QJsonArray::fromStringList(names)}};
}

QJsonObject codecRecord(const QString &selected, std::optional<bool> hardware, const QString &reason)
{
    QJsonObject record{
        {QStringLiteral("type"), QStringLiteral("codec")},
        {QStringLiteral("v"), ProtocolVersion},
        {QStringLiteral("ok"), true},
        {QStringLiteral("selected"), selected},
    };
    if (hardware) {
        record.insert(QStringLiteral("backend"), *hardware ? QStringLiteral("hardware") : QStringLiteral("software"));
    }
    if (!reason.isEmpty()) {
        record.insert(QStringLiteral("reason"), reason);
    }
    return record;
}

QJsonObject codecRecord(const QString &selected, std::optional<bool> hardware, const QString &reason, const CodecDetail &detail)
{
    QJsonObject record = codecRecord(selected, hardware, reason);
    if (!detail.encoder.isEmpty()) {
        QJsonObject encoder{{QStringLiteral("backend"), detail.encoder.left(24)}};
        if (!detail.device.isEmpty()) encoder.insert(QStringLiteral("device"), detail.device.left(16));
        if (!detail.deviceName.isEmpty()) encoder.insert(QStringLiteral("name"), detail.deviceName.left(48));
        record.insert(QStringLiteral("encoder"), encoder);
    }
    if (!detail.decodePath.isEmpty()) {
        record.insert(QStringLiteral("decodePath"), detail.decodePath);
    }
    record.insert(QStringLiteral("baseline"), detail.baseline);
    if (!detail.skipped.isEmpty()) {
        QJsonArray skipped;
        for (const auto &entry : detail.skipped.first(std::min<qsizetype>(detail.skipped.size(), 3))) {
            skipped.append(QJsonObject{{QStringLiteral("codec"), entry.codec}, {QStringLiteral("why"), QJsonArray::fromStringList(entry.why.mid(0, 6))}});
        }
        record.insert(QStringLiteral("skipped"), skipped);
    }
    return record;
}

QJsonObject sessionEndRecord(const QString &reason, quint32 errorInfo, const QString &message)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("session-end")},
        {QStringLiteral("v"), ProtocolVersion},
        {QStringLiteral("reason"), reason},
        {QStringLiteral("errorInfo"), qint64(errorInfo)},
        {QStringLiteral("message"), message},
    };
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
        if ((entry.scale && (!std::isfinite(*entry.scale) || *entry.scale <= 0.0))
            || (entry.size && entry.size->isEmpty())) {
            return Error{QStringLiteral("invalid"), QStringLiteral("monitor request has invalid size or scale")};
        }
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

    // Grow from the rightmost enabled monitor's LOGICAL edge and its top.
    // A union's top can belong to another row and cannot guarantee a
    // traversable shared edge with the output being extended.
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
        const HostMonitor *rightmost = nullptr;
        int rightEdge = 0;
        for (const auto &candidate : std::as_const(resultMonitors)) {
            if (candidate.kind == Kind::Real && !candidate.lit && !candidate.standIn) {
                continue;
            }
            const QSize size = candidate.kind == Kind::Real && candidate.standIn && candidate.standInSize ? *candidate.standInSize : candidate.size;
            const qreal candidateScale = candidate.kind == Kind::Real && candidate.standIn && candidate.standInScale ? *candidate.standInScale : candidate.scale;
            if (size.isEmpty() || !std::isfinite(candidateScale) || candidateScale <= 0.0
                || size.width() / candidateScale > caps.maxUnionPx || size.height() / candidateScale > caps.maxUnionPx) {
                return Error{QStringLiteral("invalid"), QStringLiteral("monitor %1 has invalid size or scale").arg(candidate.id)};
            }
            const int edge = RemoteMonitorGeometry::logicalRect(candidate.position, size, candidateScale).right() + 1;
            if (!rightmost || edge > rightEdge) {
                rightmost = &candidate;
                rightEdge = edge;
            }
        }
        const QPoint position(rightmost ? rightEdge : 0, rightmost ? rightmost->position.y() : 0);
        const qreal scale = entry.scale.value_or(1.0);

        actions.append(Action{
            .kind = ActionKind::CreateVirtual,
            .id = id,
            .size = *entry.size,
            .position = position,
            .scale = scale,
        });
        HostMonitor created{
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
        };
        resultMonitors.append(created);
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
