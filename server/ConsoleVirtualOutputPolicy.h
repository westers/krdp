// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "ClientDisplayInfo.h"

#include <QRect>
#include <QSet>
#include <optional>

namespace KRdp
{
// Console's temporary outputs are a capture/layout preference. They do not
// select a retained Virtual desktop or confer permission to mutate a seat.
struct ConsoleVirtualOutputPolicy {
    enum class Policy : quint8 { Replace, Extend };
    // Mapped (OPT-060 M-1): one output per HOST screen, sized and scaled like the client monitor it is
    // mapped to. Selected only by the own client's explicit request; stock clients keep Client.
    enum class Layout : quint8 { Client, Single, Physical, Mapped };

    // Hard cap of replaced host screens (Steve, 2026-10-06; default AND maximum). Above it nothing is replaced.
    static constexpr int MaxMappedScreens = 4;
    static constexpr int MaxMappedMonitors = ClientDisplay::MaxMonitors;
    static constexpr int MaxMappedName = 128;

    // A client monitor of a Mapped request. `geometry` is in PHYSICAL pixels; only its size and (for ordering)
    // its position matter. Scale is a percentage (100..400, steps of 5) so the wire has no floating point.
    struct MappedMonitor {
        QString id;
        QRect geometry;
        int scalePercent = 100;
        bool primary = false;
        bool operator==(const MappedMonitor &) const = default;
    };
    // Host screen (KWin connector name) -> client monitor id.
    struct MapEntry {
        QString host;
        QString monitor;
        bool operator==(const MapEntry &) const = default;
    };
    QVector<MappedMonitor> mappedMonitors; // Layout::Mapped only; empty otherwise.
    QVector<MapEntry> mapping;             // Layout::Mapped only; may be partial or empty (default mapping).
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

    static bool plainName(const QString &name, int maximum)
    {
        return !name.isEmpty() && name.size() <= maximum
            && std::none_of(name.cbegin(), name.cend(), [](const QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
    }

    bool validMapped() const
    {
        if (layout != Layout::Mapped) return mappedMonitors.isEmpty() && mapping.isEmpty();
        if (mappedMonitors.isEmpty() || mappedMonitors.size() > MaxMappedMonitors || mapping.size() > MaxMappedMonitors) return false;
        QSet<QString> ids;
        int primaries = 0;
        for (const auto &monitor : mappedMonitors) {
            const auto &g = monitor.geometry;
            if (!plainName(monitor.id, 64) || ids.contains(monitor.id)
                || g.width() < ClientDisplay::MinDimension || g.height() < ClientDisplay::MinDimension
                || g.width() > 16384 || g.height() > 16384
                || qint64(g.x()) < -ClientDisplay::MaxCoordinate || qint64(g.x()) > ClientDisplay::MaxCoordinate
                || qint64(g.y()) < -ClientDisplay::MaxCoordinate || qint64(g.y()) > ClientDisplay::MaxCoordinate
                || monitor.scalePercent < 100 || monitor.scalePercent > 400 || monitor.scalePercent % 5) return false;
            ids.insert(monitor.id);
            primaries += monitor.primary;
        }
        if (primaries != 1) return false;
        QSet<QString> hosts;
        for (const auto &entry : mapping) {
            if (!plainName(entry.host, MaxMappedName) || hosts.contains(entry.host) || !ids.contains(entry.monitor)) return false;
            hosts.insert(entry.host);
        }
        return true;
    }

    bool isValid() const
    {
        return quint8(policy) <= quint8(Policy::Extend) && quint8(layout) <= quint8(Layout::Mapped) && validMapped()
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

    // The per-connection request is one of two equal sources (OPT-060 D0):
    // 1. the standard monitor block (TS_UD_CS_MONITOR), read before sanitising. FreeRDP's client writes it
    //    only when it has MORE THAN ONE monitor, so a one-monitor client sends no block at all (an earlier
    //    version of this comment claimed "a one-monitor block is still a block"; no stock client sends one);
    // 2. the own client's explicit `console-screens-request` (KRDPCTL, contract (h)), which exists for exactly
    //    that gap. The gate treats both alike.
    static bool monitorBlockSent(const ClientDisplay::Info &rawInfo) { return !rawInfo.monitors.isEmpty(); }

    // What the connection asked for: the standard block when it sent one (standard RDP wins), else the
    // explicit request, else the plain connect info (no request: normal capture).
    static ClientDisplay::Info effectiveRequest(ClientDisplay::Info block, const std::optional<QVector<VideoMonitor>> &request)
    {
        if (monitorBlockSent(block) || !request || request->isEmpty()) return block;
        QRect unionRect;
        for (const auto &monitor : *request) unionRect |= monitor.geometry;
        block.monitors = *request;
        block.desktopSize = unionRect.size();
        return block;
    }

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
