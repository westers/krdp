// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#include <QtGlobal>

namespace KRdp::AdaptiveQuality
{

constexpr int MinQuality = 10;
constexpr int StepUp = 5;
constexpr int StepDown = 10;

struct BitrateAnchor {
    double pixels;
    double kbit;
};

// "Quality 100" bitrate targets by resolution, no fps term (RustDesk's base_bitrate()).
constexpr std::array<BitrateAnchor, 4> FullQualityBitrateAnchors = {{
    {921'600.0, 1500.0}, // 1280x720
    {2'073'600.0, 3110.0}, // 1920x1080
    {3'686'400.0, 4500.0}, // 2560x1440
    {8'294'400.0, 7500.0}, // 3840x2160
}};

inline double fullQualityKbit(double pixels)
{
    const auto *nearest = std::min_element(FullQualityBitrateAnchors.begin(), FullQualityBitrateAnchors.end(), [pixels](const auto &a, const auto &b) {
        return std::abs(a.pixels - pixels) < std::abs(b.pixels - pixels);
    });
    return nearest->kbit * (pixels / nearest->pixels);
}

struct Input {
    int current;
    int cap;
    quint32 goodputKbit;
    double pixels;
    // Microseconds, not milliseconds: a millisecond-truncated RTT makes 1-4 ms
    // Wi-Fi jitter (e.g. min 1 ms, average 2 ms) look like a 2x spike and trips
    // the congestion gate on noise. See congested's definition in step().
    std::chrono::microseconds averageRtt;
    std::chrono::microseconds minimumRtt;
    // True when frames were actually observed queuing up (client/link could
    // not keep up) during the goodput sample. A scheduled bandwidth window
    // measures bytes actually SENT, not link capacity, so on an idle desktop
    // (nothing to send) it reads as "the link can barely carry anything" even
    // though nothing tried to find out. Defaults to true so callers that don't
    // track this (e.g. existing tests) get the original, more conservative
    // behavior of trusting the goodput-derived target outright.
    bool linkLimited = true;
};

struct Result {
    int next;
    int target;
    bool congested;
};

// A rising RTT counts as congestion only once it is both a real gap above the
// minimum (not sub-millisecond jitter) and proportionally large.
constexpr auto CongestionRttMargin = std::chrono::milliseconds(5);

// One control step: move `current` towards the quality the measured goodput can
// carry at this resolution, slowly upwards and quickly downwards; a rising RTT
// (at least CongestionRttMargin above the minimum, and at least 1.5x it) counts
// as congestion and forces a step down.
//
// The goodput-derived target is only trusted when `linkLimited` says the
// sample actually saw the link under pressure; otherwise (an idle desktop, or
// a fast link that was never asked to prove it) the target aims for the cap
// instead — still climbing slowly (+5) rather than jumping there — since a
// low-utilization sample is not evidence the link *can't* carry more. RTT
// congestion can still force a step down either way.
inline Result step(const Input &in)
{
    if (in.goodputKbit == 0 || in.pixels <= 0.0) {
        return {in.current, in.current, false};
    }
    const int hi = std::max(in.cap, MinQuality);
    int target = std::clamp(int(std::lround(in.goodputKbit / fullQualityKbit(in.pixels) * 100.0)), MinQuality, hi);

    const bool congested = in.minimumRtt.count() > 0 && (in.averageRtt - in.minimumRtt) >= CongestionRttMargin && in.averageRtt * 2 > in.minimumRtt * 3;

    if (!in.linkLimited) {
        target = congested ? std::clamp(in.current - StepDown, MinQuality, hi) : hi;
    } else if (congested) {
        target = std::clamp(in.current - StepDown, MinQuality, target);
    }

    int next = in.current;
    if (target < next) {
        next = std::max(target, next - StepDown);
    } else if (target > next && !congested) {
        next = std::min(target, next + StepUp);
    }
    return {next, target, congested};
}

}
