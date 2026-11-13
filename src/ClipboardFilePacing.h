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
 * Start-up (PACER ramp): a fresh pacer starts in SLOW START (Auto, Cap and a stock client; never Balanced or Unlimited). The owner reports the bytes that were
 * delivered (noteDelivered(): a valid answer arrived, or a response was written behind the head-of-line bound) and each delivery raises R by SlowStartGain x bytes /
 * WindowSeconds, i.e. the window (R x 0.1 s) grows by the bytes just delivered: ack-clocked like TCP slow start, so it follows the round-trip time of the link and
 * the speed of the peer instead of a fixed 5% per tick (the 5% probe needed about 6 s for the first 5 MiB on a loopback; slow start needs about half a second). It
 * holds while the send queue is over SlowStartHoldQueueBytes (the link is filling up) and ends, with the legacy rules above taking over from the rate it reached, at
 * the first of: pressure (the cut is then exactly the old one); the target when the capacity is known (R never passes it, and a known capacity SEEDS R at
 * SlowStartSeedShare of the target at once); the cap or the host owner's ceiling; SlowStartMaxBytesPerSecond; a tick that saw bytes delivered but fewer than
 * SlowStartKeepUp x the rate the interval began with (the link or the peer limits, not R: that interval's growth is undone); a gap of SlowStartIdleGap between
 * ticks (no transfer was running: a rate established earlier in the connection is kept as it is). Without noteDelivered() calls (a peer that never answers, or an
 * owner that does not report) nothing of this happens and R follows the legacy rules only.
 * RECOVERY: the cut at a congestion signal is the old one (halving per tick down to the floor), but the rate it was cut from is remembered: SlowStartRecoveryShare of it
 * becomes a threshold, and once the pressure is over slow start runs again (driven by noteDelivered(), so only for owners that report), from wherever the cut left R, up
 * to that threshold (never past it); then the legacy 5% rule goes on. A second signal before the threshold is reached halves the threshold (below the rate it happened
 * at, and not below the start rate). Without this the overshoot slow start can make on a link that is already full costs more than the ramp saved: the old
 * re-probe from the floor needs 5% per tick (measured on a 100 ms / 20 Mbit link: 5 MiB in 22 s, the old ramp 7 s). A gap of SlowStartIdleGap forgets the threshold.
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
constexpr double SlowStartGain = 1.0; ///< R grows by this x delivered bytes / WindowSeconds (the window grows by gain x the bytes delivered)
constexpr double SlowStartSeedShare = 0.25; ///< a known capacity seeds R at this share of the target
constexpr double SlowStartRecoveryShare = 0.5; ///< after a congestion signal slow start returns up to this share of the rate it happened at
constexpr double SlowStartKeepUp = 0.5; ///< a tick whose delivered rate is below this x the rate it began with ends slow start
constexpr double SlowStartMaxBytesPerSecond = 1.25e9; ///< slow start stops here (10 Gbit/s)
constexpr qint64 SlowStartHoldQueueBytes = PressureQueueBytes / 2; ///< from this send queue on, slow start stops growing (before the pressure threshold)
constexpr auto SlowStartIdleGap = std::chrono::seconds(2);

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

    /// A transfer starts: back to the start rate with an empty bucket, in slow start.
    void reset(Clock::time_point now)
    {
        m_rate = startRate();
        resetSlowStart();
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
            if (elapsed > SlowStartIdleGap) {
                m_slowStart = false; // nothing ran for a while: a rate established earlier is kept as it is
                m_recovering = false;
            }
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
        checkKeepUp(steps);
    }

    /**
     * \a bytes of file data were delivered (a response arrived, or was written behind the head-of-line bound). In slow start this raises the rate by
     * SlowStartGain x bytes / WindowSeconds, never past the target (known capacity), the cap or the ceiling; otherwise it only feeds the keep-up check.
     */
    void noteDelivered(Clock::time_point now, quint64 bytes)
    {
        m_delivered += double(bytes);
        if (!slowStartActive() || m_pressure || m_queueBuilding) {
            return;
        }
        refill(now);
        const double limit = std::min({hardMax(), m_slowCeiling, SlowStartMaxBytesPerSecond, m_recovering ? m_ssthresh : 1e18});
        if (m_rate < limit) {
            m_rate = std::min(limit, m_rate + SlowStartGain * double(bytes) / WindowSeconds);
        }
        if (m_rate >= SlowStartMaxBytesPerSecond) {
            m_slowStart = false; // 10 Gbit/s: nothing real carries more; the legacy rules go on from here
        }
        if (m_recovering && m_rate >= m_ssthresh) {
            m_slowStart = false; // back at the threshold: the legacy rules go on from here
            m_recovering = false;
        }
    }

    /// Whether the rate is still being found by slow start (never for Balanced or Unlimited).
    bool inSlowStart() const
    {
        return slowStartActive();
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

    /**
     * OPT-078 small files: the in-flight window as a byte budget plus a count cap, inside \a limits (the negotiated Window).
     *
     * maxBytes is the BUDGET: about WindowSeconds of data at the current rate (at least MinSliceBytes). A requester sends while the bytes in flight are
     * below it (ClipboardFileLimits::requesterAdmits), so with uniform requests the number in flight is ceil(budget / request size): exactly what the old
     * count window was (ceil(rate x 100 ms / 256 KiB) for 256 KiB requests), and the one-request overshoot that lets an Auto transfer ramp is kept. maxCount
     * is only limits.maxCount: a bigger count changes how many SMALL requests share the budget, never how many bytes may be in flight, and the token bucket still
     * bounds the rate (Cap, Balanced, floor and pressure unchanged). Unlimited: the limits themselves. A stock client (no KRDPCTL): exactly the old count
     * window (at most StockClientWindow) with no byte rule beyond it.
     */
    ClipboardFileLimits::Window window(const ClipboardFileLimits::Window &limits) const
    {
        if (m_policy.stockClient) {
            const int count = window();
            return {qint64(count) * qint64(ClipboardFileLimits::MaxRangeBytes), count};
        }
        if (m_policy.unlimited) {
            return limits;
        }
        return {std::min(limits.maxBytes, windowBytes()), limits.maxCount};
    }
    /// The byte budget the current rate allows in flight (before the limits).
    qint64 windowBytes() const
    {
        return qint64(std::max(m_rate * WindowSeconds, double(MinSliceBytes)));
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
    bool policyAllowsSlowStart() const
    {
        return !m_policy.unlimited && !m_policy.balanced;
    }
    bool slowStartActive() const
    {
        return m_slowStart && policyAllowsSlowStart();
    }
    void resetSlowStart()
    {
        m_slowStart = true;
        m_recovering = false;
        m_ssthresh = 0;
        m_queueBuilding = false;
        m_slowCeiling = 1e18;
        m_delivered = 0;
        m_rateAtTick = 0;
        m_haveKeepUp = false;
    }
    /// Once per tick() call: did the bytes delivered keep up with the rate the interval began with? (The first interval only sets the baseline.)
    void checkKeepUp(int steps)
    {
        if (slowStartActive() && m_haveKeepUp && m_delivered > 0) {
            const double delivered = m_delivered / (double(steps) * std::chrono::duration<double>(Tick).count());
            if (delivered < SlowStartKeepUp * m_rateAtTick) {
                m_slowStart = false; // the link or the peer limits, not our rate: undo this interval's growth and carry on with the legacy rules
                m_rate = std::min(m_rate, std::max(m_rateAtTick, FloorBytesPerSecond));
            }
        }
        m_rateAtTick = m_rate;
        m_delivered = 0;
        m_haveKeepUp = true;
    }
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
        const bool pressureBefore = m_pressure;
        m_pressure = isUnderPressure(in);
        m_queueBuilding = in.sendQueueBytes && *in.sendQueueBytes >= SlowStartHoldQueueBytes;
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
        if (policyAllowsSlowStart()) {
            if (m_pressure) {
                if (!pressureBefore) {
                    // the rising edge of a signal: the rate it happened at (or the threshold, if that is lower: a second signal while recovering), halved, is the threshold to come back to
                    m_ssthresh = std::max(std::min(m_rate, m_recovering ? m_ssthresh : 1e18) * SlowStartRecoveryShare, StartBytesPerSecond);
                    m_recovering = true;
                }
                m_slowStart = false; // the first congestion signal ends it: the rate is cut below exactly as it always was
            } else if (m_recovering && !m_slowStart) {
                if (m_rate >= m_ssthresh) {
                    m_recovering = false;
                } else {
                    m_slowStart = true; // the pressure is over: slow start again, up to the threshold
                }
            } else if (m_slowStart && known && !m_recovering) {
                m_rate = std::max(m_rate, std::min(target, SlowStartSeedShare * target)); // seeded from what the link is known to carry
            }
        }
        m_slowCeiling = known && !m_pressure ? target : 1e18;
        if (m_rate > target) {
            m_rate = std::max(target, m_rate / 2.0);
        } else if (m_rate < target) {
            const double increase = std::max(IncreaseFraction * (known ? base : m_rate), 1000.0);
            m_rate = std::min(target, m_rate + increase);
        }
        if (m_slowStart && known && !m_pressure && m_rate >= target) {
            m_slowStart = false; // reached what the link is known to carry
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
    bool m_slowStart = true;
    bool m_recovering = false; ///< a congestion signal cut the rate while slow start ran: slow start returns up to m_ssthresh once the pressure is over
    double m_ssthresh = 0; ///< bytes/s, valid while m_recovering
    bool m_queueBuilding = false; ///< the send queue is over SlowStartHoldQueueBytes: slow start holds (the link is filling up)
    double m_slowCeiling = 1e18; ///< the target of the last tick when the capacity is known (slow start never passes it)
    double m_delivered = 0; ///< bytes delivered since the last tick() call
    double m_rateAtTick = 0; ///< R when the last tick() call finished (the interval's baseline)
    bool m_haveKeepUp = false;
};
}
