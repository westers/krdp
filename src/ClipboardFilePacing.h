// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

#include <QtGlobal>

#include "ClipboardFileLimits.h"

namespace KRdp::ClipboardFilePacing
{
/**
 * OPT-078 (spec 6.3): the scavenger pacer for file bytes, pure. It owns no clock: every call takes
 * a time point, so tests drive it with a fake one.
 *
 * "The sender of file bytes never has more than W bytes outstanding, and W shrinks when video or
 * audio is busy." Once per 100 ms tick, tick() moves the allowed rate R (bytes/s) by AIMD towards
 * a target:
 *   - pressure (send queue >= 64 KiB, video backlog, audio late, slow or congested link): target =
 *     floor (256 kbit/s); R halves per tick, so 100 Mbit/s reaches the floor in under a second;
 *   - capacity unknown, no pressure: no target (probe): R starts at 4 x floor and grows 5% of
 *     itself per tick, so a LAN is found in about 10 s;
 *   - capacity known: base = max(floor, capacity - other traffic (smoothed over 2 s)); idle link
 *     (no frame for a second, audio silent) = base; video or audio active = 50% of base;
 *     R grows by 5% of base per tick (about 2 s to recover).
 * User policy (Balanced: never above 50% of capacity; fixed cap; the host owner's ceiling) only
 * lowers the target; the floor holds beneath it, except that a cap below the floor wins. Unlimited
 * has no token bucket (window 16 and the send-queue rule remain).
 *
 * Derived: window W = clamp(ceil(R * 0.1 s / 256 KiB), 1, 16) requests (4 for a client without
 * KRDPCTL); slice = clamp(R * 50 ms, 16 KiB, 64 KiB); a token bucket with R * 50 ms burst (a slice
 * may overdraw it: the average stays R); a slice is refused while the socket holds more than
 * max(64 KiB, 2 x slice) once it is added (the head-of-line bound).
 */
using Clock = std::chrono::steady_clock;

constexpr quint64 FloorBitsPerSecond = 256'000;
constexpr double FloorBytesPerSecond = double(FloorBitsPerSecond) / 8.0;
constexpr double StartBytesPerSecond = 4.0 * FloorBytesPerSecond;
constexpr auto Tick = std::chrono::milliseconds(100);
constexpr double BurstSeconds = 0.05;
constexpr double WindowSeconds = 0.1;
constexpr qint64 PressureQueueBytes = 64 * 1024;
constexpr quint32 MinSliceBytes = 16 * 1024;
constexpr quint32 MaxSliceBytes = 64 * 1024;
constexpr double IncreaseFraction = 0.05;
constexpr double ActiveShare = 0.5;
constexpr double SmoothingPerTick = 0.05; ///< 100 ms / 2 s
constexpr int MaxTicksPerCall = 10;

enum class RequestedMode { Auto, Balanced, FixedCap, Unlimited };

/// The host owner's `ClipboardFileMaxRate`: auto (no ceiling), balanced, or a rate.
struct Ceiling {
    enum class Kind { None, Balanced, Rate };
    Kind kind = Kind::None;
    quint64 bytesPerSecond = 0; ///< Rate only (> 0)
    static Ceiling none()
    {
        return {};
    }
    static Ceiling balanced()
    {
        return {Kind::Balanced, 0};
    }
    static Ceiling rate(quint64 bytesPerSecond)
    {
        return {Kind::Rate, bytesPerSecond};
    }
    bool operator==(const Ceiling &) const = default;
};

/// The mode after the ceiling: independent limits that only ever lower the target.
struct Policy {
    bool unlimited = false;
    bool balanced = false; ///< never above 50% of the measured capacity
    std::optional<quint64> capBytesPerSecond; ///< fixed cap and/or ceiling
    bool stockClient = false; ///< no KRDPCTL: Auto with a window of at most 4
    bool operator==(const Policy &) const = default;
};

/// effective = min(client request, server ceiling). Unlimited is honoured only with no ceiling.
inline Policy effectivePolicy(RequestedMode mode, std::optional<quint64> userCapBytesPerSecond, const Ceiling &ceiling, bool stockClient = false)
{
    Policy policy;
    policy.stockClient = stockClient;
    if (stockClient) {
        mode = RequestedMode::Auto;
    }
    switch (mode) {
    case RequestedMode::Auto:
        break;
    case RequestedMode::Balanced:
        policy.balanced = true;
        break;
    case RequestedMode::FixedCap:
        policy.capBytesPerSecond = userCapBytesPerSecond && *userCapBytesPerSecond > 0 ? userCapBytesPerSecond : std::nullopt;
        break;
    case RequestedMode::Unlimited:
        policy.unlimited = true;
        break;
    }
    switch (ceiling.kind) {
    case Ceiling::Kind::None:
        break;
    case Ceiling::Kind::Balanced:
        policy.balanced = true;
        policy.unlimited = false;
        break;
    case Ceiling::Kind::Rate:
        policy.unlimited = false;
        if (ceiling.bytesPerSecond > 0) {
            policy.capBytesPerSecond = std::min(policy.capBytesPerSecond.value_or(ceiling.bytesPerSecond), ceiling.bytesPerSecond);
        }
        break;
    }
    return policy;
}

/// One tick's evidence (all optional signals: unknown = absent).
struct Inputs {
    std::optional<qint64> sendQueueBytes; ///< SIOCOUTQ
    std::optional<quint32> capacityKbps; ///< TCP delivery rate / probe; absent = unknown
    std::optional<quint32> otherKbps; ///< video + audio (not the file bytes)
    bool videoActive = false; ///< a frame was sent within the last second
    bool audioActive = false;
    bool videoBacklog = false; ///< FrameQueuePolicy: the link is not clear
    bool audioLate = false; ///< the audio counters moved this interval
    bool slow = false;
    bool congested = false;
    bool operator==(const Inputs &) const = default;
};

inline bool isUnderPressure(const Inputs &in)
{
    return (in.sendQueueBytes && *in.sendQueueBytes >= PressureQueueBytes) || in.videoBacklog || in.audioLate || in.slow || in.congested;
}

class FileTransferPacer
{
public:
    explicit FileTransferPacer(Policy policy = {})
        : m_policy(policy)
    {
        m_rate = startRate();
    }

    void setPolicy(const Policy &policy)
    {
        m_policy = policy;
        m_rate = std::min(m_rate, hardMax());
    }
    const Policy &policy() const
    {
        return m_policy;
    }

    /// A transfer starts: back to the start rate with an empty bucket.
    void reset(Clock::time_point now)
    {
        m_rate = startRate();
        m_balance = 0;
        m_lastRefill = now;
        m_lastTick.reset();
        m_haveOther = false;
        m_otherKbps = 0;
        m_pressure = false;
    }

    /// Advances the control loop by the 100 ms ticks elapsed since the last call (at most MaxTicksPerCall; the first call is one tick).
    void tick(Clock::time_point now, const Inputs &inputs)
    {
        refill(now);
        int steps = 1;
        if (m_lastTick) {
            const auto elapsed = now - *m_lastTick;
            steps = int(std::min<long long>(elapsed / Tick, MaxTicksPerCall));
            if (steps <= 0) {
                return;
            }
            *m_lastTick += Tick * steps;
            if (now - *m_lastTick > Tick * MaxTicksPerCall) {
                m_lastTick = now; // a long gap: do not replay it
            }
        } else {
            m_lastTick = now;
        }
        for (int i = 0; i < steps; ++i) {
            step(inputs);
        }
    }

    /// Allowed file rate in bytes/s (meaningless when unlimited()).
    double rateBytesPerSecond() const
    {
        return m_rate;
    }
    quint64 rateBitsPerSecond() const
    {
        return quint64(std::llround(m_rate * 8.0));
    }
    bool unlimited() const
    {
        return m_policy.unlimited;
    }
    bool underPressure() const
    {
        return m_pressure;
    }

    /// Requests the receiver may have outstanding.
    int window() const
    {
        const int cap = m_policy.stockClient ? ClipboardFileLimits::StockClientWindow : ClipboardFileLimits::MaxWindow;
        if (m_policy.unlimited) {
            return cap;
        }
        const double wanted = std::ceil(m_rate * WindowSeconds / double(ClipboardFileLimits::RangeBytes));
        return std::clamp(int(wanted), 1, cap);
    }

    /// The bytes of one write to the socket.
    quint32 sliceBytes() const
    {
        if (m_policy.unlimited) {
            return MaxSliceBytes;
        }
        const double burst = m_rate * BurstSeconds;
        return quint32(std::clamp(burst, double(MinSliceBytes), double(MaxSliceBytes)));
    }

    /// Whether a slice of \a slice bytes may be handed to the socket now (tokens and the head-of-line bound).
    bool maySend(Clock::time_point now, std::optional<qint64> sendQueueBytes, quint32 slice)
    {
        refill(now);
        if (!m_policy.unlimited && m_balance < 0) {
            return false;
        }
        if (sendQueueBytes && *sendQueueBytes >= 0) {
            const qint64 limit = std::max<qint64>(PressureQueueBytes, 2 * qint64(slice));
            if (*sendQueueBytes + qint64(slice) > limit) {
                return false;
            }
        }
        return true;
    }

    /// \a bytes were written.
    void consume(Clock::time_point now, quint64 bytes)
    {
        refill(now);
        m_balance -= double(bytes);
    }

    /// How long until the bucket is back at zero (0 = may send now).
    std::chrono::milliseconds delayUntilSend(Clock::time_point now)
    {
        refill(now);
        if (m_policy.unlimited || m_balance >= 0) {
            return std::chrono::milliseconds(0);
        }
        return std::chrono::milliseconds(qint64(std::ceil(-m_balance / m_rate * 1000.0)));
    }

private:
    double hardMax() const
    {
        return m_policy.capBytesPerSecond ? double(*m_policy.capBytesPerSecond) : 1e18;
    }
    double startRate() const
    {
        return std::min(StartBytesPerSecond, hardMax());
    }
    void refill(Clock::time_point now)
    {
        if (now <= m_lastRefill) {
            return;
        }
        const double seconds = std::chrono::duration<double>(now - m_lastRefill).count();
        m_lastRefill = now;
        m_balance = std::min(m_balance + m_rate * seconds, m_rate * BurstSeconds);
    }
    void step(const Inputs &in)
    {
        if (in.otherKbps) {
            m_otherKbps = m_haveOther ? m_otherKbps + (double(*in.otherKbps) - m_otherKbps) * SmoothingPerTick : double(*in.otherKbps);
            m_haveOther = true;
        }
        m_pressure = isUnderPressure(in);
        if (m_policy.unlimited) {
            return;
        }
        const bool known = in.capacityKbps && *in.capacityKbps > 0;
        const double capacity = known ? double(*in.capacityKbps) * 125.0 : 0.0; // bytes/s
        const double base = known ? std::max(FloorBytesPerSecond, capacity - m_otherKbps * 125.0) : 0.0;
        double target;
        if (m_pressure) {
            target = FloorBytesPerSecond;
        } else if (!known) {
            target = 1e18; // probe
        } else if (!in.videoActive && !in.audioActive) {
            target = base;
        } else {
            target = base * ActiveShare;
        }
        if (m_policy.balanced && known && !m_pressure) {
            target = std::min(target, capacity * 0.5);
        }
        target = std::min(hardMax(), std::max(FloorBytesPerSecond, target));
        if (m_rate > target) {
            m_rate = std::max(target, m_rate / 2.0);
        } else if (m_rate < target) {
            const double increase = std::max(IncreaseFraction * (known ? base : m_rate), 1000.0);
            m_rate = std::min(target, m_rate + increase);
        }
    }

    Policy m_policy;
    double m_rate = StartBytesPerSecond;
    double m_balance = 0;
    Clock::time_point m_lastRefill{};
    std::optional<Clock::time_point> m_lastTick;
    double m_otherKbps = 0;
    bool m_haveOther = false;
    bool m_pressure = false;
};
}
