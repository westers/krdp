// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-060: "someone is at the desk" reaches the Console worker once per physical input event (several a
// second while a person works). Giving the host's screens back is done once per episode (one applied
// layout); repeats only count. Pure and header-only so the rule is unit tested.

#pragma once

#include <QtGlobal>

namespace KRdp
{

class ConsoleReclaimEpisode
{
public:
    /// True when the reclaim work must run: none was done since the layout was (re)applied. A repeat
    /// returns false and is counted.
    bool needed()
    {
        if (m_done) {
            ++m_repeats;
            return false;
        }
        return true;
    }

    /// The reclaim ran and its layout was verified; the next needed() is a repeat.
    void done()
    {
        m_done = true;
    }

    bool isDone() const
    {
        return m_done;
    }

    /// A new layout was planned: the next reclaim is real work. Returns the repeats swallowed in the
    /// episode that just ended (for one summary line).
    quint64 reset()
    {
        const quint64 repeats = m_repeats;
        m_done = false;
        m_repeats = 0;
        return repeats;
    }

private:
    bool m_done = false;
    quint64 m_repeats = 0;
};

}
