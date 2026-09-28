// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>
#include <variant>

#include <QJsonObject>
#include <QMetaType>
#include <QString>

#include "LayoutControl.h"
#include "krdp_export.h"

namespace KRdp
{
/** A device the client can share with the host (DEVICES-DESIGN.md). */
enum class MediaDevice {
    Playback, ///< host audio to the client (RDPSND)
    Microphone, ///< client microphone to the host (AUDIN)
    Camera, ///< client camera to the host (RDPECAM)
};

/** A device's state, as the KRDPCTL `device` record reports it. */
struct DeviceStatus {
    enum class State {
        Off,
        Starting,
        On,
        Error,
    };
    State state = State::Off;
    /** Microphone/camera: an application on the host is capturing from the device. */
    bool inUse = false;
    /** With Error (or a revocation's Off): declined | unavailable | revoked | busy | timeout. */
    QString code;
    QString message;
    bool operator==(const DeviceStatus &) const = default;
};

/**
 * The KRDPCTL `device` record (KRDPCTL-V2-CONTRACT.md §d): the client asks the
 * host to switch one device on, off, re-select it (camera) or report it; the
 * host answers, and later pushes changes, with a `device` state record.
 * Pure: every KRDPCTL endpoint (krdpserver, the console and virtual brokers)
 * parses and answers through these.
 */
namespace DeviceControl
{
enum class Action {
    On,
    Off,
    Reselect,
    Query,
};

struct Request {
    MediaDevice device = MediaDevice::Playback;
    Action action = Action::Query;
    /** Playback `on` only: play into a session-private sink, so the host's speakers stay silent. */
    bool silenceHost = false;
    bool operator==(const Request &) const = default;
};

/**
 * Parse a `device` record whose `requestId` was already taken off
 * (LayoutControl::takeRequestId()). Strict: exactly the keys `type`, `v`,
 * `device`, `action` and, for a playback `on`, `silenceHost`. A `v` other than
 * 1 is `unsupported`; everything else malformed is `invalid`.
 */
KRDP_EXPORT std::variant<Request, LayoutControl::Error> parseRequest(const QJsonObject &record);

/**
 * Whether a host with \a capabilities (none: no runtime control) can carry out
 * \a request: nullopt if it can, otherwise the `unsupported` error to send.
 * A `query` is always answerable.
 */
KRDP_EXPORT std::optional<LayoutControl::Error> checkSupported(const Request &request, const std::optional<LayoutControl::DeviceCapabilities> &capabilities);

/** The `device` state record (without `requestId`; add it with LayoutControl::withRequestId()). */
KRDP_EXPORT QJsonObject stateRecord(MediaDevice device, const DeviceStatus &status);

KRDP_EXPORT QString deviceName(MediaDevice device);
KRDP_EXPORT std::optional<MediaDevice> deviceFromName(const QString &name);
KRDP_EXPORT QString stateName(DeviceStatus::State state);

/** `code` values of a `device` state record. */
inline const QString Declined = QStringLiteral("declined");
inline const QString Unavailable = QStringLiteral("unavailable");
inline const QString Revoked = QStringLiteral("revoked"); ///< console: the console user or controller changed
/// Virtual broker (AUD-FIX2): this connection no longer holds the virtual desktop (detached,
/// ended, or opened from another device); "revoked" wording about the console does not apply.
inline const QString Detached = QStringLiteral("detached");
inline const QString Busy = QStringLiteral("busy");
inline const QString Timeout = QStringLiteral("timeout");
}
}

Q_DECLARE_METATYPE(KRdp::MediaDevice)
Q_DECLARE_METATYPE(KRdp::DeviceStatus)
