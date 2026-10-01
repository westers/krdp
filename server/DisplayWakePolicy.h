// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "ConsoleWorkerWire.h"

namespace KRdp
{
struct DisplayViewer {
    bool admitted = false;
    bool streaming = false;
    bool wakeEnabled = true;
};

inline ConsoleWorkerWire::DisplayPolicy displayPolicyFor(const QVector<DisplayViewer> &viewers)
{
    ConsoleWorkerWire::DisplayPolicy result;
    for (const auto &viewer : viewers) {
        if (!viewer.admitted || !viewer.streaming) continue;
        result.active = true;
        result.wakeEnabled |= viewer.wakeEnabled;
    }
    return result; // Endpoint supplies the revision after aggregation.
}

/** One worker launch. Stale/duplicate/conflicting old revisions cannot wake a
 * released desktop or reacquire a cookie. New socket launches start at zero. */
class DisplayWakePolicy
{
public:
    bool accept(const ConsoleWorkerWire::DisplayPolicy &policy)
    {
        if (!policy.revision || policy.revision <= m_policy.revision
            || (!policy.active && policy.wakeEnabled)) return false;
        m_policy = policy;
        return true;
    }
    const ConsoleWorkerWire::DisplayPolicy &current() const { return m_policy; }
private:
    ConsoleWorkerWire::DisplayPolicy m_policy;
};
}
