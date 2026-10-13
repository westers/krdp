// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>

#include <QtGlobal>

#include "ConsoleWorkerWire.h"

namespace KRdp
{
/**
 * OPT-055 wave 2: the broker's verdict on whether a Console worker's event loop is still turning. Pure and driven
 * by an explicit clock, so tests use a fake one.
 *
 * Any record from the authenticated worker (a Heartbeat, a frame, a result) is proof of life, and so are bytes
 * waiting unread on the socket. Silence is accumulated per check as min(time since the previous check, MaxTickCreditMs),
 * not measured as wall time: a broker that was itself stalled (suspend, SIGSTOP, CPU starvation) finds its next
 * check far later than scheduled, but that check adds at most MaxTickCreditMs, so one long stall can never turn
 * directly into "hung". Only a run of ordinary checks that all saw silence reaches the threshold.
 */
class ConsoleWorkerWatchdog
{
public:
    enum class Verdict {
        Idle, // not armed, or already reported
        Alive,
        Hung, // reported once per arming
    };

    explicit ConsoleWorkerWatchdog(int intervalMs = ConsoleWorkerWire::HeartbeatIntervalMs, int hungAfterMs = ConsoleWorkerWire::HeartbeatHungAfterMs)
        : m_hungAfterMs(hungAfterMs)
        , m_maxTickCreditMs(qint64(intervalMs) * 2)
    {
    }

    void arm(qint64 nowMs)
    {
        m_armed = true;
        m_reported = false;
        m_silentMs = 0;
        m_lastTickMs = nowMs;
    }
    void disarm() { m_armed = false; }
    bool armed() const { return m_armed; }

    /** A record arrived (or bytes are waiting). */
    void alive(qint64 nowMs)
    {
        if (!m_armed) return;
        m_silentMs = 0;
        m_lastTickMs = nowMs;
    }

    Verdict tick(qint64 nowMs, bool bytesPending = false)
    {
        if (!m_armed || m_reported) return Verdict::Idle;
        if (bytesPending) {
            alive(nowMs);
            return Verdict::Alive;
        }
        const qint64 elapsed = std::clamp<qint64>(nowMs - m_lastTickMs, 0, m_maxTickCreditMs);
        m_lastTickMs = nowMs;
        m_silentMs += elapsed;
        if (m_silentMs >= m_hungAfterMs) {
            m_reported = true;
            return Verdict::Hung;
        }
        return Verdict::Alive;
    }

    qint64 silentMs() const { return m_silentMs; }

private:
    int m_hungAfterMs;
    qint64 m_maxTickCreditMs;
    bool m_armed = false;
    bool m_reported = false;
    qint64 m_silentMs = 0;
    qint64 m_lastTickMs = 0;
};
}
