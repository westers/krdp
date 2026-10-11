// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

/**
 * OPT-066: the per-connection video quality a client asks for in the KRDPCTL `codec` request
 * (`quality`, 10..100), pure so the clamp and the debounce are unit-tested without a stream.
 *
 * The host's `Quality` is the default and the ceiling; the optional `ClientQualityMax` raises the
 * ceiling for clients (0 = same as `Quality`). A client never gets above the ceiling.
 */
namespace KRdp::ClientQuality
{
constexpr int MinRequest = 10; ///< lowest value a request may carry (the adaptive floor)
constexpr int MaxRequest = 100;

/// The highest quality a client may request: `ClientQualityMax` when set, else the host's `Quality`.
constexpr int ceiling(int hostQuality, int clientQualityMax)
{
    return clientQualityMax > 0 ? std::min(clientQualityMax, MaxRequest) : hostQuality;
}

/// True for a value a `codec` request may carry.
constexpr bool validRequest(long long value)
{
    return value >= MinRequest && value <= MaxRequest;
}

struct Plan {
    int cap = 0; ///< the quality cap this connection runs under (AdaptiveQuality steps below it, never above)
    int ceiling = 0; ///< what the host allows this client
    bool clamped = false; ///< the request was above the ceiling and got cut to it
    bool operator==(const Plan &) const = default;
};

/// effective = clamp(requested, 10, ceiling); without a request, the host's own `Quality`.
constexpr Plan plan(int hostQuality, int clientQualityMax, std::optional<int> requested)
{
    Plan result;
    result.ceiling = ceiling(hostQuality, clientQualityMax);
    if (!requested) {
        result.cap = hostQuality;
        return result;
    }
    const int wanted = std::max(*requested, MinRequest);
    result.cap = std::min(wanted, result.ceiling);
    result.clamped = wanted > result.ceiling;
    return result;
}

/// What the answer says happened to the request (`qualityEffect` on the wire).
enum class Effect {
    None, ///< no request (host default)
    Applied, ///< applied now
    Pending, ///< accepted; applied once the slider settles (Debounce)
    IgnoredBitrateMode, ///< accepted, but the running encoder is in bitrate mode and does not use the quality
    NotController, ///< a Console viewer: only the controlling connection's request applies
};

constexpr const char *effectName(Effect effect)
{
    switch (effect) {
    case Effect::None:
        return "none";
    case Effect::Applied:
        return "applied";
    case Effect::Pending:
        return "pending";
    case Effect::IgnoredBitrateMode:
        return "ignoredBitrateMode";
    case Effect::NotController:
        return "notController";
    }
    return "none";
}

using Clock = std::chrono::steady_clock;

/// A re-sent request is applied once the value stopped changing for SettleTime, and never more often than
/// MinApplyInterval (a VAAPI encoder reopens with a keyframe on every quality change).
constexpr auto SettleTime = std::chrono::milliseconds(500);
constexpr auto MinApplyInterval = std::chrono::seconds(2);

/**
 * How long to wait before applying the latest requested value: zero for the first request of a connection
 * (nothing was applied yet), else until \a lastChange + SettleTime and \a lastApply + MinApplyInterval have both passed.
 */
constexpr Clock::duration applyDelay(Clock::time_point now, Clock::time_point lastChange, std::optional<Clock::time_point> lastApply)
{
    if (!lastApply) {
        return Clock::duration::zero();
    }
    const auto settled = lastChange + SettleTime;
    const auto allowed = *lastApply + MinApplyInterval;
    return std::max(Clock::duration::zero(), std::max(settled, allowed) - now);
}
}
