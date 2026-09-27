// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QJsonObject>
#include <QString>

#include <DeviceControl.h>
#include <LayoutControl.h>
#include <RdpConnection.h>

namespace KRdp
{
/**
 * krdpserver's side of the KRDPCTL `device` record (DEVICES-DESIGN.md §3/§4):
 * SessionController's glue, kept here so the tests drive the same code.
 */
namespace PhysicalDeviceControl
{
/** On a logged-in desktop everything can be switched. */
constexpr LayoutControl::DeviceCapabilities Capabilities{
    .playbackToggle = true,
    .playbackSilenceHost = true,
    .microphoneToggle = true,
    .cameraToggle = true,
    .cameraReselect = true,
};

/**
 * Handle one `device` record (its `requestId`, already taken off, is \a requestId).
 * Returns the `error` record to send now (echoing \a requestId), or nullopt
 * when the request went to \a connection, which answers it through
 * RdpConnection::deviceState (send that with stateRecord()).
 */
inline std::optional<QJsonObject> request(RdpConnection &connection, const QJsonObject &record, const QString &requestId)
{
    const auto parsed = DeviceControl::parseRequest(record);
    if (const auto *error = std::get_if<LayoutControl::Error>(&parsed)) {
        return LayoutControl::withRequestId(LayoutControl::errorRecord(*error), requestId);
    }
    const auto device = std::get<DeviceControl::Request>(parsed);
    if (const auto refused = DeviceControl::checkSupported(device, Capabilities)) {
        return LayoutControl::withRequestId(LayoutControl::errorRecord(*refused), requestId);
    }
    connection.requestDevice(device.device, device.action, device.silenceHost, requestId);
    return std::nullopt;
}

/** The record a RdpConnection::deviceState emission becomes on the wire. */
inline QJsonObject stateRecord(MediaDevice device, const DeviceStatus &status, const QString &requestId)
{
    return LayoutControl::withRequestId(DeviceControl::stateRecord(device, status), requestId);
}

/**
 * StandardClientMedia (DEVICES-DESIGN.md §1) is for clients that do not speak
 * KRDPCTL: once a client sent a `device` record, or any other record this
 * host knows, it asks for each device itself, and a device it did not ask
 * for stays off.
 */
constexpr bool wantsStandardConsent(bool deviceRecordSeen, bool spokeKrdpctl)
{
    return !deviceRecordSeen && !spokeKrdpctl;
}
}
}
