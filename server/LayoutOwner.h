// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QList>
#include <QString>

#include "LayoutControl.h"

/**
 * Who owns the host layout (slice 2c, OPT-044, design §4 "Owner state machine").
 *
 * The first `KRDPCTL` connection to apply a layout owns it; every other
 * connection's `apply` is refused as `not-owner` unless it asks for
 * `takeoverLayout`, in which case it becomes the owner and the previous owner
 * a viewer. Viewers are connections the controller built sessions for from
 * the owner's layout; they are tracked here only so `you` can be answered.
 * The owner is released when it disconnects or when three heartbeats in a
 * row go unanswered; viewers stay viewers of whatever layout is left.
 *
 * Pure bookkeeping: no timers, no I/O. The controller drives it and turns
 * "released" into the guard restore and the `layout` broadcast.
 */
class LayoutOwner
{
public:
    enum class Role {
        None,
        Owner,
        Viewer,
    };

    /** Missed heartbeats in a row that release the owner (design §3: "3 missed → release"). */
    static constexpr int MaxMissedHeartbeats = 3;

    /** The `you` spelling of a role. */
    static QString roleName(Role role)
    {
        switch (role) {
        case Role::Owner:
            return QStringLiteral("owner");
        case Role::Viewer:
            return QStringLiteral("viewer");
        case Role::None:
            break;
        }
        return QStringLiteral("none");
    }

    bool hasOwner() const
    {
        return !m_owner.isEmpty();
    }

    /** The owning connection id; empty when nobody owns the layout. */
    QString owner() const
    {
        return m_owner;
    }

    QList<QString> viewers() const
    {
        return m_viewers;
    }

    int missedHeartbeats() const
    {
        return m_missed;
    }

    Role roleOf(const QString &id) const
    {
        if (!id.isEmpty() && id == m_owner) {
            return Role::Owner;
        }
        if (m_viewers.contains(id)) {
            return Role::Viewer;
        }
        return Role::None;
    }

    /**
     * Whether \a id may apply a layout right now, without changing anything:
     * nullopt when it may (nobody owns, it owns, or it asked to take over),
     * the `not-owner` error otherwise. Checked before the plan so an invalid
     * request from a non-owner is answered `not-owner`, and so a plan that
     * turns out invalid never moves ownership.
     */
    std::optional<KRdp::LayoutControl::Error> canAcquire(const QString &id, bool takeover) const
    {
        if (!hasOwner() || m_owner == id || takeover) {
            return std::nullopt;
        }
        return KRdp::LayoutControl::Error{
            QStringLiteral("not-owner"),
            QStringLiteral("the layout is owned by %1; send takeoverLayout to take it over").arg(m_owner),
        };
    }

    /**
     * canAcquire(), then make \a id the owner: a previous owner (another
     * connection) becomes a viewer, and \a id stops being one.
     */
    std::optional<KRdp::LayoutControl::Error> tryAcquire(const QString &id, bool takeover)
    {
        if (const auto refused = canAcquire(id, takeover)) {
            return refused;
        }
        if (m_owner != id) {
            // Demote the previous owner after clearing it: addViewer()
            // refuses whoever m_owner names.
            const QString previous = m_owner;
            m_owner.clear();
            addViewer(previous);
            m_viewers.removeAll(id);
            m_owner = id;
            m_missed = 0;
        }
        return std::nullopt;
    }

    /** A connection that streams the owner's layout without owning it. The owner is never a viewer. */
    void addViewer(const QString &id)
    {
        if (id.isEmpty() || id == m_owner || m_viewers.contains(id)) {
            return;
        }
        m_viewers.append(id);
    }

    /**
     * \a id is gone (disconnected) or forfeits the layout. True when it was
     * the owner and the layout is now unowned: the caller restores the desk.
     */
    bool release(const QString &id)
    {
        if (!id.isEmpty() && id == m_owner) {
            m_owner.clear();
            m_missed = 0;
            return true;
        }
        m_viewers.removeAll(id);
        return false;
    }

    /**
     * A heartbeat to \a id went unanswered. Only the owner is heartbeated;
     * the MaxMissedHeartbeats-th miss in a row releases it (true).
     */
    bool heartbeatMissed(const QString &id)
    {
        if (id.isEmpty() || id != m_owner) {
            return false;
        }
        if (++m_missed < MaxMissedHeartbeats) {
            return false;
        }
        return release(id);
    }

    void heartbeatOk(const QString &id)
    {
        if (!id.isEmpty() && id == m_owner) {
            m_missed = 0;
        }
    }

private:
    QString m_owner;
    QList<QString> m_viewers;
    int m_missed = 0;
};
