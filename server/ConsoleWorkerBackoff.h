// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QtGlobal>

namespace KRdp
{
/**
 * Relaunch delay for a console capture worker that failed before (or after)
 * becoming ready: a crash, a failed launch, or a worker of another paired wire
 * version. Without it the broker relaunched the same failing worker on every
 * seat change, a storm of processes and log lines (AUD-C-6).
 *
 * 1 s, 2 s, 4 s, ... capped at 30 s. Each level is reported once, so a worker
 * that keeps failing at the cap logs a single line, not one every 30 s.
 */
class ConsoleWorkerBackoff
{
public:
    static constexpr int InitialMs = 1000;
    static constexpr int MaximumMs = 30000;

    struct Step {
        int delayMs = 0;
        bool newLevel = false; ///< First time this delay is used since reset(): log it.
    };

    Step next()
    {
        const int delay = m_failures >= 5 ? MaximumMs : qMin(MaximumMs, InitialMs << m_failures);
        const bool newLevel = delay != m_lastDelay;
        m_lastDelay = delay;
        if (m_failures < 5) {
            ++m_failures;
        }
        return {delay, newLevel};
    }

    void reset()
    {
        m_failures = 0;
        m_lastDelay = 0;
    }

    int failures() const { return m_failures; }

private:
    int m_failures = 0;
    int m_lastDelay = 0;
};
}
