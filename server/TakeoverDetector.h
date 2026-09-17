// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QPoint>
#include <QtGlobal>

/**
 * Console takeover detection for `MonitorMode=virtual` with the replace
 * policy (OPT-041 Task 6c).
 *
 * While the physical monitors are off, the only pointer on the desktop is
 * the one the server injects for the remote client - unless someone at the
 * console moves the real mouse. The screencast's cursor metadata reports
 * where the pointer actually is; comparing that with where the server last
 * put it tells the two apart. Pure: no clock, no Qt object, so
 * autotests/TakeoverDetectorTest.cpp can state the rule exactly.
 *
 * Every position is KWin-global logical, the space fake input works in, so
 * an injected move and the cursor sample it produces compare directly.
 */
namespace KRdp::Takeover
{
/** A sample further than this (Manhattan) from the last injected position cannot be that injection. */
constexpr int DistanceThresholdPx = 24;
/** A sample this soon after an injection may still be the compositor catching up with it. */
constexpr int QuietWindowMs = 300;
/** Ignore samples this long after arming: disabling outputs makes KWin warp the pointer. */
constexpr int ArmDelayMs = 2000;

struct Detector {
    /** Call when the replace policy has been applied; nothing fires before this. */
    void armed(qint64 nowMs)
    {
        m_armedMs = nowMs;
        m_isArmed = true;
    }

    /** Every pointer motion the server injects for the client. */
    void injected(const QPoint &globalLogical, qint64 nowMs)
    {
        m_lastInjected = globalLogical;
        m_lastInjectedMs = nowMs;
        m_hasInjected = true;
    }

    /**
     * Every cursor metadata sample. True exactly once, for the first sample
     * that can only be local motion; latched afterwards, since the takeover
     * it announces is done once.
     */
    bool observed(const QPoint &globalLogical, qint64 nowMs)
    {
        if (m_fired || !m_isArmed || !m_hasInjected) {
            return false;
        }
        if (nowMs - m_armedMs < ArmDelayMs) {
            return false;
        }
        if (nowMs - m_lastInjectedMs <= QuietWindowMs) {
            return false;
        }
        if ((globalLogical - m_lastInjected).manhattanLength() <= DistanceThresholdPx) {
            return false;
        }
        m_fired = true;
        return true;
    }

    bool fired() const
    {
        return m_fired;
    }

private:
    bool m_isArmed = false;
    bool m_hasInjected = false;
    bool m_fired = false;
    qint64 m_armedMs = 0;
    qint64 m_lastInjectedMs = 0;
    QPoint m_lastInjected;
};
}
