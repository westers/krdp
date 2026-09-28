// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

namespace KRdp
{
/**
 * AUD-FIX12: one encoded stream's liveness (PlasmaScreencastV1Session; pure, no Qt, no IO).
 *
 * ace, c27909d, 2026-09-28: the broker throttled a moving HEVC output from 60 to 6 fps and asked
 * for a keyframe. The frame-rate change renegotiates the PipeWire stream; KPipeWire's VA-API
 * import of the new buffers then failed ("Failed to create surface from DRM object", hwmap
 * EIO), its filter graph returned EIO for every later frame, and the frames it had taken in
 * filled its filter queue for good ("Filter queue is full, dropping frame"). The encoder never
 * produced another packet: 61 s of silence until the client disconnected. A keyframe request
 * cannot get through that either (KPipeWire only re-feeds a picture when its queue has room).
 * Reproduced on Hal's 780M by WorkerEndToEndTest::coalesceKeepsEveryOutputAlive.
 *
 * The rules:
 * - a keyframe request that produces no keyframe within RetryAfter is repeated once, and one
 *   that is still unanswered at RestartAfter restarts the encoded stream (a fresh KPipeWire
 *   producer, filter graph and encoder, which opens with an IDR). A restarted stream that
 *   produces no keyframe within RestartAnswerWithin is restarted again, up to MaxRestarts per
 *   request; after that the watchdog gives up until the next request.
 * - after a reconfiguration of the stream (frame rate, quality, codec, a keyframe request) the
 *   stream is armed for ArmFor. An armed stream that was moving (at least MovingPackets packets
 *   in the MovingWindow before its last one) and then goes silent for silenceLimit() is probed
 *   with a keyframe request: a healthy encoder answers at once even when the picture stopped
 *   changing (KPipeWire re-encodes the last picture), a wedged one does not, and the rule above
 *   restarts it. One probe per silence; a still picture after a burst of motion costs one
 *   keyframe, and only in the ArmFor after a reconfiguration.
 * So a moving stream is never silent for much more than silenceLimit() + RestartAfter plus the
 * restart itself (about 0.2-0.4 s): under 2 s.
 */
namespace EncoderWatchdog
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto ArmFor = std::chrono::seconds(15);
constexpr auto SilenceMin = std::chrono::milliseconds(600);
constexpr int SilenceIntervals = 4;
constexpr int MovingPackets = 3;
constexpr auto MovingWindow = std::chrono::milliseconds(1500);
constexpr auto RetryAfter = std::chrono::milliseconds(250);
constexpr auto RestartAfter = std::chrono::milliseconds(450);
constexpr auto RestartAnswerWithin = std::chrono::seconds(3);
constexpr int MaxRestarts = 2;

enum class Action {
    None,
    Probe, ///< ask the encoder for a keyframe: the stream went silent after a reconfiguration
    Retry, ///< ask again: the last request is unanswered
    Restart, ///< restart the encoded stream: requests go unanswered
    GiveUp, ///< MaxRestarts restarts produced nothing: wait for the next request
};

class Watchdog
{
public:
    /// The stream was reconfigured (frame rate, quality, codec) at \a now.
    void arm(Clock::time_point now)
    {
        m_armedUntil = std::max(m_armedUntil, now + ArmFor);
    }

    /// Someone asked the encoder for a keyframe at \a now.
    void keyFrameRequested(Clock::time_point now)
    {
        arm(now);
        m_probing = false;
        if (!m_pending) {
            m_pending = true;
            m_pendingSince = now;
            m_retried = false;
            m_restarts = 0;
        }
    }

    /// The encoder produced a packet at \a now.
    void packet(Clock::time_point now, bool keyFrame)
    {
        if (keyFrame && m_pending && m_probing) {
            // The answer to a probe is no motion: probe again only after new motion.
            m_sinceProbe = 0;
            m_probing = false;
        } else {
            m_sinceProbe = std::min(m_sinceProbe + 1, MovingPackets);
        }
        m_packets[m_next] = now;
        m_next = (m_next + 1) % m_packets.size();
        m_count = std::min(m_count + 1, m_packets.size());
        m_lastPacket = now;
        m_probed = false;
        if (keyFrame) {
            m_pending = false;
            m_restarts = 0;
        }
    }

    /// The stream was torn down on purpose (stop, a new capture): nothing is owed any more.
    void reset()
    {
        m_pending = false;
        m_restarts = 0;
        m_count = 0;
        m_probed = false;
        m_probing = false;
        m_sinceProbe = MovingPackets;
    }

    /// What to do at \a now (call every TickInterval while needsTimer()).
    Action poll(Clock::time_point now)
    {
        if (m_pending) {
            if (m_restarts > 0) {
                if (now - m_lastRestart < RestartAnswerWithin) {
                    return Action::None;
                }
                if (m_restarts >= MaxRestarts) {
                    m_pending = false;
                    return Action::GiveUp;
                }
                ++m_restarts;
                m_lastRestart = now;
                return Action::Restart;
            }
            if (!m_retried && now - m_pendingSince >= RetryAfter) {
                m_retried = true;
                return Action::Retry;
            }
            if (now - m_pendingSince >= RestartAfter) {
                m_restarts = 1;
                m_lastRestart = now;
                return Action::Restart;
            }
            return Action::None;
        }
        if (now >= m_armedUntil || m_probed || m_sinceProbe < MovingPackets || !moving()) {
            return Action::None;
        }
        if (now - m_lastPacket < silenceLimit()) {
            return Action::None;
        }
        m_probed = true;
        m_probing = true;
        m_pending = true;
        m_pendingSince = now;
        m_retried = false;
        m_restarts = 0;
        return Action::Probe;
    }

    /// Whether poll() has anything to watch.
    bool needsTimer(Clock::time_point now) const
    {
        return m_pending || now < m_armedUntil;
    }

    bool pending() const
    {
        return m_pending;
    }

    /// How long an armed, moving stream may be silent before it is probed: SilenceMin, or
    /// SilenceIntervals of its recent packet interval when that is longer (a throttled stream).
    Clock::duration silenceLimit() const
    {
        const auto window = recent();
        if (window.count < 2) {
            return SilenceMin;
        }
        const auto interval = (window.last - window.first) / (window.count - 1);
        return std::max<Clock::duration>(SilenceMin, interval * SilenceIntervals);
    }

    /// At least MovingPackets packets within MovingWindow before the last one.
    bool moving() const
    {
        return recent().count >= size_t(MovingPackets);
    }

    static constexpr auto TickInterval = std::chrono::milliseconds(50);

private:
    struct Recent {
        size_t count = 0;
        Clock::time_point first;
        Clock::time_point last;
    };
    Recent recent() const
    {
        Recent r;
        r.last = m_lastPacket;
        r.first = m_lastPacket;
        for (size_t i = 0; i < m_count; ++i) {
            const auto at = m_packets[(m_next + m_packets.size() - 1 - i) % m_packets.size()];
            if (m_lastPacket - at > MovingWindow) {
                break;
            }
            r.first = at;
            ++r.count;
        }
        return r;
    }

    std::array<Clock::time_point, 16> m_packets{};
    size_t m_next = 0;
    size_t m_count = 0;
    Clock::time_point m_lastPacket{};
    Clock::time_point m_armedUntil{};
    bool m_pending = false;
    Clock::time_point m_pendingSince{};
    bool m_retried = false;
    int m_restarts = 0;
    Clock::time_point m_lastRestart{};
    bool m_probed = false; ///< this silence was probed
    bool m_probing = false; ///< the pending request is a probe
    int m_sinceProbe = MovingPackets; ///< packets since a probe's answer (capped)
};
}
}
