// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "ClientDisplayInfo.h"

#include <optional>

namespace KRdp
{
// Console's temporary outputs are a capture/layout preference. They do not
// select a retained Virtual desktop or confer permission to mutate a seat.
struct ConsoleVirtualOutputPolicy {
    enum class Policy : quint8 { Replace, Extend };
    enum class Layout : quint8 { Client, Single, Physical };
    bool enabled = false;
    Policy policy = Policy::Replace;
    Layout layout = Layout::Client;
    QSize fallback = QSize(1920, 1080);
    ClientDisplay::Info client{QSize(1920, 1080), {}};

    bool operator==(const ConsoleVirtualOutputPolicy &) const = default;

    static bool boundedMonitor(const VideoMonitor &monitor)
    {
        // Check before QRect union/translation and abs(int), including INT_MIN.
        return ClientDisplay::usable(monitor.geometry.size())
            && qint64(monitor.geometry.x()) >= -ClientDisplay::MaxCoordinate
            && qint64(monitor.geometry.x()) <= ClientDisplay::MaxCoordinate
            && qint64(monitor.geometry.y()) >= -ClientDisplay::MaxCoordinate
            && qint64(monitor.geometry.y()) <= ClientDisplay::MaxCoordinate;
    }

    bool isValid() const
    {
        return quint8(policy) <= quint8(Policy::Extend) && quint8(layout) <= quint8(Layout::Physical)
            && ClientDisplay::usable(fallback) && !(fallback.width() % 2) && !(fallback.height() % 2)
            && client.monitors.size() <= ClientDisplay::MaxMonitors
            && std::all_of(client.monitors.cbegin(), client.monitors.cend(), boundedMonitor)
            && client == ClientDisplay::sanitize(client, fallback);
    }

    // Native RDP monitor information is advisory. Convert it to a bounded,
    // canonical request before transport; malformed lists use the single-size
    // fallback instead of changing only part of an output arrangement.
    ClientDisplay::Info normalize(ClientDisplay::Info info) const
    {
        if (info.monitors.size() > ClientDisplay::MaxMonitors
            || !std::all_of(info.monitors.cbegin(), info.monitors.cend(), boundedMonitor))
            info.monitors.clear();
        return ClientDisplay::sanitize(std::move(info), fallback);
    }

    // Per-user permission for Console "Replace" (OPT-060), carried by VirtualMonitorPolicy:
    // `off` never lets a connection turn the host's screens off; `replace` (the default, "When the
    // connection asks") lets a connection that sends its own RDP monitor block do so; `extend`
    // keeps the screens on (advanced). The old MonitorMode=virtual opt-in no longer matters.
    enum class Permission : quint8 { Off, Ask };
    enum class Gate : quint8 { Replace, NoMonitorBlock, PermissionOff, NotPhysicalUser, AlreadyAttempted };

    static Permission permissionOf(const QString &virtualMonitorPolicy)
    {
        return virtualMonitorPolicy == QStringLiteral("off") ? Permission::Off : Permission::Ask;
    }
    // The layout policy the plan uses; `off` never reaches a plan, so it reads as the default.
    static QString planPolicy(const QString &virtualMonitorPolicy)
    {
        return virtualMonitorPolicy == QStringLiteral("off") ? QStringLiteral("replace") : virtualMonitorPolicy;
    }

    // The standard monitor block (TS_UD_CS_MONITOR) is the per-connection
    // request. It is read before sanitising: a one-monitor block is still a block.
    static bool monitorBlockSent(const ClientDisplay::Info &rawInfo) { return !rawInfo.monitors.isEmpty(); }

    // Every refusal ends in normal Console capture; only `Replace` creates outputs.
    static Gate gate(Permission permission, bool monitorBlock, bool physicalUser, bool alreadyAttempted)
    {
        if (!physicalUser) return Gate::NotPhysicalUser;
        if (permission == Permission::Off) return Gate::PermissionOff;
        if (!monitorBlock) return Gate::NoMonitorBlock;
        if (alreadyAttempted) return Gate::AlreadyAttempted;
        return Gate::Replace;
    }

    static std::optional<ConsoleVirtualOutputPolicy> parse(bool enabled, const QString &policy,
        const QString &layout, const QSize &fallback, const ClientDisplay::Info &client)
    {
        ConsoleVirtualOutputPolicy result;
        result.enabled = enabled;
        result.fallback = fallback;
        if (!ClientDisplay::usable(fallback) || fallback.width() % 2 || fallback.height() % 2) return {};
        if (policy == QStringLiteral("replace")) result.policy = Policy::Replace;
        else if (policy == QStringLiteral("extend")) result.policy = Policy::Extend;
        else return {};
        if (layout == QStringLiteral("client")) result.layout = Layout::Client;
        else if (layout == QStringLiteral("single")) result.layout = Layout::Single;
        else if (layout == QStringLiteral("physical")) result.layout = Layout::Physical;
        else return {};
        result.client = result.normalize(client);
        return result.isValid() ? std::optional(result) : std::nullopt;
    }
};
}
