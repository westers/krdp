// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <cmath>

#include <QList>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QStringList>

#include "LayoutControl.h"
#include "OutputSnapshot.h"

namespace KRdp
{
/**
 * The pure half of HostLayoutExecutor (OPT-044): given the layout an apply
 * results in and the virtual outputs that already exist, which KWin outputs
 * the host needs, where each goes, what has to be created and what removed.
 * No KWin, no processes; unit-tested from autotests/OutputSnapshotTest.cpp.
 */
namespace LayoutArrangement
{
/** One output the executor created and holds. */
struct VirtualOutput {
    /** The HostMonitor it stands for: a real monitor's id for a stand-in, "virtual-<n>" otherwise. */
    QString monitorId;
    /** KWin's name for it, prefix included ("Virtual-krdp-si-DP-1-1920x1080-s100"). */
    QString name;
    /** The connection whose apply created it. */
    QString owner;
    QSize size;
    qreal scale = 1.0;
    /** Where the arrangement put it (KWin logical position). */
    QPoint position;
    bool standIn = false;

    bool operator==(const VirtualOutput &) const = default;
};

inline QString scaleTag(qreal scale)
{
    return QString::number(qRound(scale * 100.0));
}

/**
 * The name requested from KWin (which prefixes "Virtual-"). Size and scale
 * are part of it on purpose: KWin remembers an arrangement (scale included)
 * per output identity, and a same-named output at another size would have
 * that replayed onto it. No dots: kscreen-doctor splits `output.<name>.<setting>`
 * on them.
 */
inline QString standInName(const QString &realId, const QSize &size, qreal scale)
{
    return QStringLiteral("krdp-si-%1-%2x%3-s%4").arg(realId).arg(size.width()).arg(size.height()).arg(scaleTag(scale));
}

inline QString virtualName(const QString &virtualId, const QSize &size, qreal scale)
{
    QString number = virtualId;
    number.remove(QStringLiteral("virtual-"));
    return QStringLiteral("krdp-v%1-%2x%3-s%4").arg(number).arg(size.width()).arg(size.height()).arg(scaleTag(scale));
}

/** The size/scale a real monitor presents: its own when merely dark, the stand-in's when stood in. */
inline QSize presentedSize(const LayoutControl::HostMonitor &monitor)
{
    return monitor.standIn && monitor.standInSize ? *monitor.standInSize : monitor.size;
}

inline qreal presentedScale(const LayoutControl::HostMonitor &monitor)
{
    return monitor.standIn && monitor.standInScale ? *monitor.standInScale : monitor.scale;
}

/** Pixel width to KWin logical width (positions are logical, sizes are pixels). */
inline int logicalWidth(const QSize &size, qreal scale)
{
    return int(std::ceil(size.width() / (scale > 0.0 ? scale : 1.0)));
}

struct Derived {
    /**
     * The one kscreen-doctor call: enabled entries first, primary first (so
     * it takes priority 1), disabled physical outputs, then the outputs
     * being removed, parked beyond the enabled union so nothing overlaps
     * for the moment they still exist.
     */
    QList<OutputSnapshot::Arrangement> arrangement;
    /** Every virtual output the layout needs, existing or not. */
    QList<VirtualOutput> wanted;
    /** The subset of `wanted` that does not exist yet. */
    QList<VirtualOutput> creating;
    /** Names of existing outputs the layout no longer needs. */
    QStringList removing;
    /** KWin output names the built sessions will capture (every enabled entry). */
    QStringList neededScreens;
    /** Where the first removed output is parked: right of the enabled entries. */
    QPoint parkAnchor;
    /** A physical output changes state (a disable, or an enable of a currently dark one is only known to the caller). */
    bool physicalDisabled = false;

    bool operator==(const Derived &) const = default;
};

/**
 * Derive everything execute() needs from \a resulting (the plan's layout),
 * \a existing (the executor's table) and \a requester (owner of the
 * stand-ins it creates).
 */
inline Derived derive(const LayoutControl::Layout &resulting, const QList<VirtualOutput> &existing, const QString &requester)
{
    Derived out;

    QList<LayoutControl::HostMonitor> ordered = resulting.monitors;
    std::stable_partition(ordered.begin(), ordered.end(), [](const LayoutControl::HostMonitor &monitor) {
        return monitor.primary;
    });

    // Enabled entries, and the right edge / top of their union for the park.
    int rightEdge = 0;
    int top = 0;
    bool anyEnabled = false;
    auto noteEnabled = [&](const QPoint &position, const QSize &size, qreal scale) {
        const int right = position.x() + logicalWidth(size, scale);
        if (!anyEnabled) {
            rightEdge = right;
            top = position.y();
            anyEnabled = true;
        } else {
            rightEdge = std::max(rightEdge, right);
            top = std::min(top, position.y());
        }
    };

    QList<OutputSnapshot::Arrangement> disabled;
    for (const auto &monitor : std::as_const(ordered)) {
        if (monitor.kind == LayoutControl::Kind::Real) {
            if (monitor.lit) {
                out.arrangement.push_back({monitor.id, true, monitor.position});
                out.neededScreens.push_back(monitor.id);
                noteEnabled(monitor.position, monitor.size, monitor.scale);
                continue;
            }
            disabled.push_back({monitor.id, false, QPoint()});
            out.physicalDisabled = true;
            VirtualOutput standIn;
            standIn.monitorId = monitor.id;
            standIn.size = presentedSize(monitor);
            standIn.scale = presentedScale(monitor);
            standIn.name = OutputSnapshot::VirtualPrefix + standInName(monitor.id, standIn.size, standIn.scale);
            standIn.owner = requester;
            standIn.position = monitor.position;
            standIn.standIn = monitor.standIn;
            out.wanted.push_back(standIn);
        } else {
            VirtualOutput extra;
            extra.monitorId = monitor.id;
            extra.size = monitor.size;
            extra.scale = monitor.scale;
            extra.name = OutputSnapshot::VirtualPrefix + virtualName(monitor.id, extra.size, extra.scale);
            extra.owner = monitor.owner;
            extra.position = monitor.position;
            out.wanted.push_back(extra);
        }
        const auto &output = out.wanted.last();
        out.arrangement.push_back({output.name, true, output.position});
        out.neededScreens.push_back(output.name);
        noteEnabled(output.position, output.size, output.scale);
    }
    out.arrangement += disabled;

    // What exists already stays; what does not is created; what is no
    // longer wanted is parked right of everything that stays enabled - by
    // the same call, so a stand-in never sits under a real output coming
    // back on, and never on top of an extra that stays - and removed once
    // the arrangement has applied.
    out.parkAnchor = QPoint(rightEdge, top);
    QPoint park = out.parkAnchor;
    for (const auto &output : existing) {
        const bool stillWanted = std::any_of(out.wanted.cbegin(), out.wanted.cend(), [&output](const VirtualOutput &wanted) {
            return wanted.name == output.name;
        });
        if (!stillWanted) {
            out.removing.push_back(output.name);
            out.arrangement.push_back({output.name, true, park});
            park.rx() += logicalWidth(output.size, output.scale);
        }
    }
    for (const auto &output : std::as_const(out.wanted)) {
        const bool exists = std::any_of(existing.cbegin(), existing.cend(), [&output](const VirtualOutput &candidate) {
            return candidate.name == output.name;
        });
        if (!exists) {
            out.creating.push_back(output);
        }
    }
    return out;
}

/**
 * The KWin output that carries \a monitorId in \a layout: the connector for
 * a lit real monitor, its stand-in's name for a dark or stood-in one, the
 * virtual output's name for a virtual monitor. Empty when unknown, or when
 * the output it needs is not in \a outputs.
 */
inline QString outputNameFor(const LayoutControl::Layout &layout, const QList<VirtualOutput> &outputs, const QString &monitorId)
{
    const auto it = std::find_if(layout.monitors.cbegin(), layout.monitors.cend(), [&monitorId](const LayoutControl::HostMonitor &monitor) {
        return monitor.id == monitorId;
    });
    if (it == layout.monitors.cend()) {
        return {};
    }
    if (it->kind == LayoutControl::Kind::Real && it->lit) {
        return it->id;
    }
    const auto output = std::find_if(outputs.cbegin(), outputs.cend(), [&monitorId](const VirtualOutput &candidate) {
        return candidate.monitorId == monitorId;
    });
    return output == outputs.cend() ? QString() : output->name;
}
}
}
