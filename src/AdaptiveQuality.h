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
// Backlog depth is roughly frame rate x ack latency and is independent of QP,
// so 2 would read a healthy 60 fps stream with a ~35 ms ack lag as backlogged
// every interval; 4 needs a sustained >= 67 ms lag at 60 fps, while a
// saturated socket holds 10+ frames and is still caught.
constexpr int BacklogFrames = 4;

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
    // AVC444 connections have one more rung above the QP ladder: the auxiliary chroma stream.
    // It is the first thing shed under pressure and the last thing restored on a clear link.
    bool chromaAvailable = false;
    bool chromaEnabled = true;
};

struct Result {
    int next;
    bool congested;
    bool chromaEnabled;
};

// One control step. Pressure (RTT congestion or a persistent frame backlog) first sheds the
// chroma stream (AVC444, QP unchanged), then steps quality down by StepDown; a clear interval
// steps quality up by StepUp towards the cap, and only once at the cap re-enables chroma - both
// only once climbAllowed. The result never exceeds the cap and never drops below MinQuality.
// Without chromaAvailable the rung does not exist and the numbers are exactly the AVC420 ones.
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
    const bool chromaOn = !in.chromaAvailable || in.chromaEnabled;

    int next = in.current;
    bool chroma = in.chromaEnabled;
    if (congested || in.backlogged) {
        if (in.chromaAvailable && chromaOn) {
            chroma = false;
        } else {
            next = in.current - StepDown;
        }
    } else if (in.climbAllowed) {
        if (in.current < hi) {
            next = in.current + StepUp;
        } else if (in.chromaAvailable && !chromaOn) {
            chroma = true;
        }
    }
    next = std::clamp(next, MinQuality, hi);
    return {next, congested, chroma};
}

}
