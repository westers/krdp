// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "DeviceControl.h"

#include <QSet>

namespace KRdp::DeviceControl
{
namespace
{
LayoutControl::Error invalid(const QString &message)
{
    return {QStringLiteral("invalid"), message};
}

LayoutControl::Error unsupported(const QString &message)
{
    return {QStringLiteral("unsupported"), message};
}
}

QJsonObject availabilityRecord(bool camera, bool microphone)
{
    const auto entry = [](bool available) {
        QJsonObject object{{QStringLiteral("available"), available}};
        if (!available) object.insert(QStringLiteral("reason"), NeedsSession);
        return object;
    };
    return {{QStringLiteral("type"), QStringLiteral("device-availability")},
            {QStringLiteral("v"), 1},
            {QStringLiteral("camera"), entry(camera)},
            {QStringLiteral("microphone"), entry(microphone)}};
}

QString deviceName(MediaDevice device)
{
    switch (device) {
    case MediaDevice::Playback:
        return QStringLiteral("playback");
    case MediaDevice::Microphone:
        return QStringLiteral("microphone");
    case MediaDevice::Camera:
        return QStringLiteral("camera");
    }
    return {};
}

std::optional<MediaDevice> deviceFromName(const QString &name)
{
    for (const auto device : {MediaDevice::Playback, MediaDevice::Microphone, MediaDevice::Camera}) {
        if (name == deviceName(device)) {
            return device;
        }
    }
    return std::nullopt;
}

QString stateName(DeviceStatus::State state)
{
    switch (state) {
    case DeviceStatus::State::Off:
        return QStringLiteral("off");
    case DeviceStatus::State::Starting:
        return QStringLiteral("starting");
    case DeviceStatus::State::On:
        return QStringLiteral("on");
    case DeviceStatus::State::Error:
        return QStringLiteral("error");
    }
    return {};
}

std::variant<Request, LayoutControl::Error> parseRequest(const QJsonObject &record)
{
    const QJsonValue version = record.value(QStringLiteral("v"));
    if (!version.isDouble() || version.toDouble() != 1) {
        return unsupported(QStringLiteral("device record version %1 is not supported; this host speaks 1").arg(version.toVariant().toString()));
    }
    static const QSet<QString> known{QStringLiteral("type"), QStringLiteral("v"), QStringLiteral("device"), QStringLiteral("action"), QStringLiteral("silenceHost")};
    for (auto it = record.constBegin(); it != record.constEnd(); ++it) {
        if (!known.contains(it.key())) {
            return invalid(QStringLiteral("device record has an unknown field \"%1\"").arg(it.key()));
        }
    }
    const QJsonValue deviceValue = record.value(QStringLiteral("device"));
    const auto device = deviceValue.isString() ? deviceFromName(deviceValue.toString()) : std::nullopt;
    if (!device) {
        return invalid(QStringLiteral("device must be playback, microphone or camera"));
    }
    const QJsonValue actionValue = record.value(QStringLiteral("action"));
    const QString action = actionValue.isString() ? actionValue.toString() : QString();
    Request request;
    request.device = *device;
    if (action == QLatin1String("on")) {
        request.action = Action::On;
    } else if (action == QLatin1String("off")) {
        request.action = Action::Off;
    } else if (action == QLatin1String("reselect")) {
        request.action = Action::Reselect;
    } else if (action == QLatin1String("query")) {
        request.action = Action::Query;
    } else {
        return invalid(QStringLiteral("device action must be on, off, reselect or query"));
    }
    const QJsonValue silenceHost = record.value(QStringLiteral("silenceHost"));
    if (!silenceHost.isUndefined()) {
        if (request.device != MediaDevice::Playback || request.action != Action::On || !silenceHost.isBool()) {
            return invalid(QStringLiteral("silenceHost is a boolean, and only valid with a playback \"on\""));
        }
        request.silenceHost = silenceHost.toBool();
    }
    return request;
}

std::optional<LayoutControl::Error> checkSupported(const Request &request, const std::optional<LayoutControl::DeviceCapabilities> &capabilities)
{
    if (request.action == Action::Query) {
        return std::nullopt;
    }
    const QString name = deviceName(request.device);
    if (!capabilities) {
        return unsupported(QStringLiteral("this host cannot switch devices during a session"));
    }
    bool toggle = false;
    switch (request.device) {
    case MediaDevice::Playback:
        toggle = capabilities->playbackToggle;
        break;
    case MediaDevice::Microphone:
        toggle = capabilities->microphoneToggle;
        break;
    case MediaDevice::Camera:
        toggle = capabilities->cameraToggle;
        break;
    }
    if (!toggle) {
        // Off must remain possible if a bridge disappears during a session.
        if (request.device == MediaDevice::Camera && request.action == Action::Off) return std::nullopt;
        if (request.device == MediaDevice::Camera && !capabilities->cameraUnavailableReason.isEmpty())
            return unsupported(capabilities->cameraUnavailableReason);
        return unsupported(QStringLiteral("this host cannot switch the %1 during a session").arg(name));
    }
    if (request.action == Action::Reselect && (request.device != MediaDevice::Camera || !capabilities->cameraReselect)) {
        return unsupported(QStringLiteral("this host cannot re-select the %1").arg(name));
    }
    if (request.silenceHost && !capabilities->playbackSilenceHost) {
        return unsupported(QStringLiteral("this host cannot silence its speakers"));
    }
    return std::nullopt;
}

QJsonObject stateRecord(MediaDevice device, const DeviceStatus &status)
{
    QJsonObject record{
        {QStringLiteral("type"), QStringLiteral("device")},
        {QStringLiteral("v"), LayoutControl::ProtocolVersion},
        {QStringLiteral("device"), deviceName(device)},
        {QStringLiteral("state"), stateName(status.state)},
        {QStringLiteral("inUse"), status.inUse},
    };
    if (!status.code.isEmpty()) {
        record.insert(QStringLiteral("code"), status.code);
    }
    if (!status.message.isEmpty()) {
        record.insert(QStringLiteral("message"), status.message.left(1024));
    }
    return record;
}
}
