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
    std::chrono::milliseconds averageRtt;
    std::chrono::milliseconds minimumRtt;
};

struct Result {
    int next;
    int target;
    bool congested;
};

// One control step: move `current` towards the quality the measured goodput can
// carry at this resolution, slowly upwards and quickly downwards; a rising RTT
// (average > 1.5x minimum) counts as congestion and forces a step down.
inline Result step(const Input &in)
{
    if (in.goodputKbit == 0 || in.pixels <= 0.0) {
        return {in.current, in.current, false};
    }
    const int hi = std::max(in.cap, MinQuality);
    int target = std::clamp(int(std::lround(in.goodputKbit / fullQualityKbit(in.pixels) * 100.0)), MinQuality, hi);

    const bool congested = in.minimumRtt.count() > 0 && in.averageRtt.count() > in.minimumRtt.count() * 3 / 2;
    if (congested) {
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
