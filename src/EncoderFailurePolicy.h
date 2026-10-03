// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <chrono>
#include <cstddef>
#include <deque>

namespace KRdp
{

/**
 * What a session does when KPipeWire reports that its encoder failed for good
 * (PipeWireBaseEncodedStream::encoderFailed, OPT-055 K4.1).
 *
 * Pure: no Qt, no clocks of its own. Every call carries the time, so the
 * autotest drives it with made-up time points.
 *
 *  - a failure while a restart is in flight, or while streaming is not
 *    requested, is Ignored (the restart in flight already replaces the encoder);
 *  - otherwise Restart while fewer than MaxRestarts restarts happened in the
 *    last Window, else Close;
 *  - the budget is forgiven once a restart is older than Window, i.e. after
 *    Window of streaming without another failure.
 */
class EncoderFailurePolicy
{
public:
    using Clock = std::chrono::steady_clock;
    enum class Action {
        Ignore,
        Restart,
        Close,
    };

    static constexpr std::size_t MaxRestarts = 2;
    static constexpr std::chrono::seconds Window{60};

    Action onFailure(Clock::time_point now, bool restartInFlight, bool streamingRequested)
    {
        if (restartInFlight || !streamingRequested) {
            return Action::Ignore;
        }
        while (!m_restarts.empty() && now - m_restarts.front() >= Window) {
            m_restarts.pop_front();
        }
        if (m_restarts.size() >= MaxRestarts) {
            return Action::Close;
        }
        m_restarts.push_back(now);
        return Action::Restart;
    }

    void reset()
    {
        m_restarts.clear();
    }

private:
    std::deque<Clock::time_point> m_restarts;
};

/**
 * K4.4: when the user server restarts itself after a KPipeWire producer thread
 * was abandoned (it still holds its encoder and a GPU device until the process
 * ends). Only while nobody is connected, after Delay of idleness.
 */
struct IdleRestartPolicy {
    static constexpr std::chrono::seconds Delay{10};
    static constexpr int ExitCode = 70; // not 75 (RestartPreventExitStatus), so Restart=on-failure restarts the service

    /// Whether to arm the idle timer: a connection just closed (or the process just became idle).
    static bool shouldArm(std::size_t connections, int abandonedProducers)
    {
        return connections == 0 && abandonedProducers > 0;
    }
    /// Whether the exit is still due when the timer fires.
    static bool shouldExit(std::size_t connections, int abandonedProducers)
    {
        return shouldArm(connections, abandonedProducers);
    }
};

}
