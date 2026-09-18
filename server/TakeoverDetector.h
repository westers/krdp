// SPDX-FileCopyrightText: 2026 KDE Contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QPoint>
#include <QtGlobal>

#include <algorithm>

/**
 * Console takeover detection for `MonitorMode=virtual` with the replace
 * policy (OPT-041 Task 6c).
 *
 * While the physical monitors are off, the only pointer on the desktop is
 * the one the server injects for the remote client - unless someone at the
 * console moves the real mouse. The screencast's cursor metadata reports
 * where the pointer actually is; comparing that with where the server last
 * put it, or with where it was a sample ago while the server put it
 * nowhere, tells the two apart. Pure: no clock, no Qt object, so
 * autotests/TakeoverDetectorTest.cpp can state the rules exactly.
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
/** Ignore samples this long after the captured output moved: the churn that follows warps the pointer too. */
constexpr int OutputMoveSuspendMs = 3000;

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
        m_everInjected = true;
    }

    /**
     * The reference injection is no longer where the pointer is: the output
     * it was mapped through has moved since (its origin changed after the
     * move was recorded), so the next injection has to become the reference
     * before a sample can be judged against it. The injection's time still
     * counts for the quiet window, and the sample-to-sample rule is not
     * affected.
     */
    void forgetInjected()
    {
        m_hasInjected = false;
    }

    /**
     * The captured output has moved (a park, a replayed arrangement after a
     * physical output came or went): the injected reference AND the previous
     * sample were mapped through its old origin, so neither may be compared
     * with what comes next - two samples straddling the move would be the
     * output's displacement apart with nobody touching a mouse - and the
     * churn that follows warps the pointer. Both references go, and samples
     * are ignored until \a nowMs + OutputMoveSuspendMs.
     */
    void outputMoved(qint64 nowMs)
    {
        forgetInjected();
        m_hasPreviousSample = false;
        suspend(nowMs + OutputMoveSuspendMs);
    }

    /**
     * Ignore samples until \a untilMs: a restore or park is in progress and
     * the compositor is removing and re-adding outputs, which warps the
     * pointer and produces samples nobody asked for. Not a latch; the
     * detector resumes at the deadline.
     */
    void suspend(qint64 untilMs)
    {
        m_suspendedUntilMs = std::max(m_suspendedUntilMs, untilMs);
    }

    /**
     * The takeover has happened by another trigger (tray, shortcut) or the
     * physical outputs have been released: there is nothing left to detect,
     * and the output churn a restore causes must never read as a second
     * console activity. Same end state as a fired detector.
     */
    void latch()
    {
        m_fired = true;
    }

    /**
     * Every cursor metadata sample. True exactly once, for the first sample
     * that can only be local motion; latched afterwards, since the takeover
     * it announces is done once.
     *
     * Two rules, either fires. (1) The sample is further than the threshold
     * from the last injected position, outside the quiet window after that
     * injection. (2) The sample is further than the threshold from the
     * previous sample, and neither sample is inside the quiet window after
     * an injection - so no injection lies between them or just before the
     * first. Rule 2 needs no injection at all: a remote session that has not
     * moved its pointer since the replace (keyboard work) still gives the
     * console its monitors back on the first real mouse motion. Samples
     * inside the arm delay or a suspend window are neither judged nor kept
     * as the previous sample, so a warp never becomes the reference.
     */
    bool observed(const QPoint &globalLogical, qint64 nowMs)
    {
        if (m_fired || !m_isArmed) {
            return false;
        }
        if (nowMs - m_armedMs < ArmDelayMs || nowMs < m_suspendedUntilMs) {
            return false;
        }
        const bool quietNow = !m_everInjected || nowMs - m_lastInjectedMs > QuietWindowMs;
        const bool quietAtPrevious = !m_everInjected || m_previousSampleMs - m_lastInjectedMs > QuietWindowMs;
        bool local = false;
        if (quietNow && m_hasInjected && (globalLogical - m_lastInjected).manhattanLength() > DistanceThresholdPx) {
            local = true;
        } else if (quietNow && quietAtPrevious && m_hasPreviousSample && (globalLogical - m_previousSample).manhattanLength() > DistanceThresholdPx) {
            local = true;
        }
        m_previousSample = globalLogical;
        m_previousSampleMs = nowMs;
        m_hasPreviousSample = true;
        if (local) {
            m_fired = true;
        }
        return local;
    }

    bool fired() const
    {
        return m_fired;
    }

private:
    bool m_isArmed = false;
    // m_hasInjected: the position reference is valid; m_everInjected: the
    // time reference is (forgetInjected() clears only the former).
    bool m_hasInjected = false;
    bool m_everInjected = false;
    bool m_hasPreviousSample = false;
    bool m_fired = false;
    qint64 m_armedMs = 0;
    qint64 m_lastInjectedMs = 0;
    qint64 m_previousSampleMs = 0;
    qint64 m_suspendedUntilMs = 0;
    QPoint m_lastInjected;
    QPoint m_previousSample;
};
}
