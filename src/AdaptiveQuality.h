// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <algorithm>
#include <chrono>

namespace KRdp::AdaptiveQuality
{

constexpr int MinQuality = 10;
constexpr int StepUp = 5;
constexpr int StepDown = 10;

// A rising RTT counts as congestion only once it is both a real gap above the
// minimum (not sub-millisecond jitter) and proportionally large (>= 1.5x).
constexpr auto CongestionRttMargin = std::chrono::milliseconds(5);

// The client is "backlogged" when it never got within this many
// unacknowledged frames of caught up during the whole decision interval
// (VideoStream tracks the minimum after each ack and at decision time).
constexpr int BacklogFrames = 2;

// After a step down, hold before climbing again so a limited link settles
// instead of sawtoothing every interval.
constexpr auto ClimbHoldAfterStepDown = std::chrono::seconds(5);

struct Input {
    int current;
    int cap;
    // Microseconds, not milliseconds: a millisecond-truncated RTT makes 1-4 ms
    // Wi-Fi jitter (min 1 ms, average 2 ms) look like a 2x spike.
    std::chrono::microseconds averageRtt;
    std::chrono::microseconds minimumRtt;
    // True when the client stayed >= BacklogFrames frames behind for the
    // whole interval - the client or the link could not keep up.
    bool backlogged;
    // False while ClimbHoldAfterStepDown has not elapsed since the last step down.
    bool climbAllowed;
};

struct Result {
    int next;
    bool congested;
};

// One control step. Pressure (RTT congestion or a persistent frame backlog)
// steps quality down by StepDown; a clear interval steps it up by StepUp
// towards the cap, but only once climbAllowed. The result never exceeds the
// cap and never drops below MinQuality.
//
// There is deliberately no goodput term: a passively measured goodput is a
// lower bound on capacity, not an estimate of it (an idle desktop sends
// almost nothing), and a CQP encoder's bitrate varies ~100x with content, so
// a bitrate->quality table mis-steers in both directions. See Task 7 of the
// OPT-016 plan for the 2026-09-16 journal evidence.
inline Result step(const Input &in)
{
    const int hi = std::max(in.cap, MinQuality);
    const bool congested = in.minimumRtt.count() > 0 && (in.averageRtt - in.minimumRtt) >= CongestionRttMargin && in.averageRtt * 2 > in.minimumRtt * 3;

    int next = in.current;
    if (congested || in.backlogged) {
        next = in.current - StepDown;
    } else if (in.climbAllowed) {
        next = in.current + StepUp;
    }
    next = std::clamp(next, MinQuality, hi);
    return {next, congested};
}

}
