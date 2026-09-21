// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ConsoleSeat.h"

namespace KRdp::ConsoleHandoff
{
/** A specific Wayland session a capture worker may attach to. */
struct Target {
    ConsoleSeat::Adapter adapter = ConsoleSeat::Adapter::None;
    QString sessionId;
    quint32 uid = 0;

    bool valid() const
    {
        return adapter != ConsoleSeat::Adapter::None && !sessionId.isEmpty() && uid != 0;
    }
    bool operator==(const Target &) const = default;
};

inline Target targetFor(const QList<ConsoleSeat::Session> &sessions, const QString &seat = QStringLiteral("seat0"))
{
    const auto adapter = ConsoleSeat::adapterFor(sessions, seat);
    return {adapter, ConsoleSeat::activeSessionId(sessions, adapter, seat), ConsoleSeat::activeSessionUid(sessions, adapter, seat).value_or(0)};
}

/**
 * The only operations a persistent RDP broker performs during a seat change.
 *
 * `revokeInput` is deliberately separated from `startWorker`: when a greeter
 * disappears, input must have no endpoint until the selected user worker says
 * it is ready. `resetGraphics` and `requestKeyFrame` are emitted together
 * only after that ready acknowledgement, never while an old worker is still
 * allowed to submit frames.
 */
struct Actions {
    bool revokeInput = false;
    bool stopWorker = false;
    bool startWorker = false;
    bool grantInput = false;
    bool resetGraphics = false;
    bool requestKeyFrame = false;
    Target target;

    bool empty() const
    {
        return !revokeInput && !stopWorker && !startWorker && !grantInput && !resetGraphics && !requestKeyFrame;
    }
};

/**
 * Pure broker-side handoff state. The eventual console host owns this object;
 * adapters/workers only report stopped and ready. It keeps the old capture
 * endpoint out of the new desktop's input path during SDDM -> Plasma handoff.
 */
class State
{
public:
    Actions reconcile(const QList<ConsoleSeat::Session> &sessions, const QString &seat = QStringLiteral("seat0"))
    {
        return select(targetFor(sessions, seat));
    }

    Actions select(const Target &wanted)
    {
        if (wanted == m_wanted) {
            return {};
        }
        m_wanted = wanted;
        if (m_running.valid() || m_starting.valid()) {
            m_running = {};
            m_starting = {};
            m_draining = true;
            Actions actions;
            actions.revokeInput = true;
            actions.stopWorker = true;
            return actions;
        }
        return startWanted();
    }

    /** Old worker has closed its socket and cannot submit frames or input. */
    Actions workerStopped()
    {
        if (!m_draining) {
            return {};
        }
        m_draining = false;
        return startWanted();
    }

    /** The named worker completed its authenticated IPC handshake and capture setup. */
    Actions workerReady(const Target &ready)
    {
        if (m_draining || ready != m_starting || ready != m_wanted) {
            return {};
        }
        m_running = ready;
        m_starting = {};
        return {.grantInput = true, .resetGraphics = true, .requestKeyFrame = true, .target = ready};
    }

    Target activeTarget() const { return m_running; }
    Target startingTarget() const { return m_starting; }
    bool inputEnabled() const { return m_running.valid(); }

private:
    Actions startWanted()
    {
        if (!m_wanted.valid()) {
            return {};
        }
        m_starting = m_wanted;
        return {.startWorker = true, .target = m_starting};
    }

    Target m_wanted;
    Target m_running;
    Target m_starting;
    bool m_draining = false;
};
}
