// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>
#include <variant>

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QPoint>
#include <QSize>
#include <QString>

#include "ClientDisplayInfo.h"
#include "krdp_export.h"

namespace KRdp
{
/**
 * The `KRDPCTL` static virtual channel protocol: records, wire framing and the
 * pure planner that turns "current host layout + a client's apply request"
 * into layout actions (slice 2c, OPT-044). No networking, no KWin: the
 * connection/controller layer feeds this from and to the real world.
 */
namespace LayoutControl
{
/** A host monitor is either a real output or one the client asked to be created. */
enum class Kind {
    Real,
    Virtual,
};

/**
 * One monitor of the host layout, as shown to `KRDPCTL` clients.
 *
 * A real monitor's `size` and `position` are its native, unchanging geometry
 * (MonitorMode never changes a real output's mode); `standIn`/`lit` are the
 * only fields planning ever changes on it. A virtual monitor's `size`,
 * `position` and `scale` are exactly what was requested/placed for it.
 */
struct HostMonitor {
    /** Real: the connector name ("DP-1"). Virtual: server-assigned ("virtual-<n>"). */
    QString id;
    QString name;
    Kind kind;
    QSize size;
    QPoint position;
    qreal scale = 1.0;
    bool primary = false;
    /** Real monitors only: whether the physical screen is on at the desk. */
    bool lit = true;
    /** Real monitors only: whether it is currently replaced by a Fit stand-in. */
    bool standIn = false;
    /** Virtual monitors only: the connection id that created it; empty for none/real. */
    QString owner;

    bool operator==(const HostMonitor &) const = default;
};

/** Server-advertised limits; defaults match ClientDisplay's sanitising rules. */
struct Caps {
    int maxOutputPx = ClientDisplay::MaxDimension;
    int maxUnionPx = ClientDisplay::MaxDesktopDimension;
    bool cursorMetadata = true;

    bool operator==(const Caps &) const = default;
};

/** The `layout` record body (also used, with `type: takeover`, for `takeover`). */
struct Layout {
    QList<HostMonitor> monitors;
    /** The owning connection id; empty when the layout has no owner. */
    QString owner;
    /** "owner" | "viewer" | "none", from the recipient's point of view. */
    QString you;
    Caps caps;

    bool operator==(const Layout &) const = default;
};

/** One monitor entry inside an `apply` request. */
struct ApplyMonitor {
    /** Empty when isNew is true (the server assigns the id). */
    QString id;
    bool isNew = false;
    std::optional<bool> lit;
    std::optional<QSize> size;
    std::optional<qreal> scale;

    bool operator==(const ApplyMonitor &) const = default;
};

/** The `apply` record body. */
struct ApplyRequest {
    bool privateMode = false;
    bool takeoverLayout = false;
    QList<ApplyMonitor> monitors;

    bool operator==(const ApplyRequest &) const = default;
};

/** The `error` record body. `code` is one of "not-owner" | "invalid" | "unsupported". */
struct Error {
    QString code;
    QString message;

    bool operator==(const Error &) const = default;
};

KRDP_EXPORT QJsonObject toJson(const Layout &layout);
KRDP_EXPORT std::optional<Layout> layoutFromJson(const QJsonObject &object);
KRDP_EXPORT QJsonObject toJson(const ApplyRequest &request);
KRDP_EXPORT std::optional<ApplyRequest> applyFromJson(const QJsonObject &object);

/** Wraps \a error / \a layout as a full `{"type": ...}` record body, ready for frame(). */
KRDP_EXPORT QJsonObject errorRecord(const Error &error);
KRDP_EXPORT QJsonObject layoutRecord(const Layout &layout);
KRDP_EXPORT QJsonObject takeoverRecord(const Layout &layout);

/** \a record plus `"v": 1`, as a 4-byte big-endian length prefix followed by UTF-8 JSON. */
KRDP_EXPORT QByteArray frame(const QJsonObject &record);

/**
 * Reassembles frame()'d records out of a byte stream that may arrive split
 * across reads, or with several records in one read.
 *
 * A declared length over 64 KiB is treated as a protocol violation rather
 * than an allocation to attempt: overflowed() latches true and no further
 * records are produced.
 */
class KRDP_EXPORT Deframer
{
public:
    void feed(const QByteArray &data);
    /** The next complete record, if the buffer holds one. */
    std::optional<QJsonObject> next();
    bool overflowed() const;

private:
    QByteArray m_buffer;
    bool m_overflowed = false;
};

enum class ActionKind {
    LightReal,
    DarkenReal,
    CreateStandIn,
    RemoveStandIn,
    CreateVirtual,
    RemoveVirtual,
};

/** One instruction for the executor. Fields not meaningful for `kind` are left default. */
struct Action {
    ActionKind kind;
    QString id;
    QSize size;
    QPoint position;
    qreal scale = 1.0;

    bool operator==(const Action &) const = default;
};

struct Plan {
    QList<Action> actions;
    Layout resulting;

    bool operator==(const Plan &) const = default;
};

/**
 * Turns \a current + \a request from \a requester into the actions needed to
 * reach the new layout, sanitising the result against \a caps.
 *
 * Ownership ("not-owner") is decided above this call, not here: plan() only
 * ever fails with Error{"invalid", ...} when the requested layout would
 * violate a sanitising rule (an output or the union too large, overlapping
 * monitors, not exactly one primary) or names a monitor id that does not
 * exist.
 */
KRDP_EXPORT std::variant<Plan, Error> plan(const Layout &current, const ApplyRequest &request, const QString &requester, const Caps &caps);

}
}
