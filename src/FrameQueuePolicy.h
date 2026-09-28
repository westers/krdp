// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace KRdp
{

/**
 * What the pending-send queue may throw away when a new frame arrives.
 *
 * Lives in its own header so the rule is a pure predicate over plain values:
 * no FreeRDP, no Qt GUI, no locks, so autotests/FrameQueuePolicyTest.cpp can
 * state it without a display or a connection.
 */
namespace FrameQueuePolicy
{

/**
 * Whether a queued frame of \a queuedMonitorIndex is made redundant by a
 * keyframe that just arrived for \a keyframeMonitorIndex.
 *
 * A keyframe is self-contained, so everything still queued for the SAME
 * monitor can be dropped: that monitor's backlog can never outgrow one
 * keyframe interval, and the client is shown the newest picture rather than
 * replaying a stale one.
 *
 * It says nothing at all about any OTHER monitor. In `MonitorMode=multi` each
 * monitor is captured by its own session and encoded by its own encoder, so
 * the queued frames of another monitor are a different reference chain
 * entirely: dropping them would leave that monitor's surface frozen until its
 * own next IDR, which on a static desktop is seconds away.
 *
 * With a single surface every frame carries index 0, so every queued frame is
 * superseded and the queue is cleared exactly as it always was.
 */
inline bool supersededByKeyframe(int queuedMonitorIndex, int keyframeMonitorIndex)
{
    return queuedMonitorIndex == keyframeMonitorIndex;
}

/**
 * Remove from \a queue every frame superseded by a keyframe for
 * \a keyframeMonitorIndex, keeping the rest in order.
 *
 * \a Queue is any Qt container of a type with a `monitorIndex` member (the
 * live one is QQueue<VideoFrame>).
 */
template<class Queue>
void dropSupersededFrames(Queue &queue, int keyframeMonitorIndex)
{
    queue.removeIf([keyframeMonitorIndex](const auto &queued) {
        return supersededByKeyframe(queued.monitorIndex, keyframeMonitorIndex);
    });
}

/**
 * AUD-FIX4 D1: how much video may be on its way to the client at once.
 *
 * Without a cap the submission thread handed every encoded frame to FreeRDP
 * as it arrived. On a saturated link (Sol/Buzz 2026-09-27) 316-341 frames,
 * about 11 s of video, were unacknowledged. FreeRDP's server queues channel
 * data without bound (WTSVirtualChannelWrite) and its peer loop drains the
 * whole queue with blocking writes (transport_default_write waits for the
 * socket with no timeout), so the peer thread wrote for seconds without
 * reading: the client's heartbeats and acks went unread, its own writes
 * stalled, and its TLS layer gave up. Bounding what is in flight bounds that
 * queue, and so how long the peer loop can go without reading.
 *
 * The window is a frame count and a byte budget:
 * - frames: the frames that fit in one minimum RTT plus AckAllowance at the
 *   current frame rate, per surface, between MinInFlightFrames and
 *   MaxInFlightFrames (a LAN at 60 fps gets 4, a 100 ms path 8);
 * - bytes: the measured rate for one minimum RTT plus InFlightByteHorizon,
 *   at least MinInFlightBytes. A single frame larger than the budget (a
 *   keyframe) still goes out when nothing else is in flight.
 */
constexpr int MinInFlightFrames = 4;
constexpr int MaxInFlightFrames = 8;
constexpr auto AckAllowance = std::chrono::milliseconds(60);
constexpr int64_t MinInFlightBytes = 128 * 1024;
constexpr auto InFlightByteHorizon = std::chrono::milliseconds(250);
/**
 * With acknowledgements suspended (MS-RDPEGFX SUSPEND_FRAME_ACKNOWLEDGEMENT)
 * there are no acks to open the window: a frame then counts as in flight for
 * this long plus one minimum RTT (a time budget), and the socket's own unsent
 * bytes must stay under the byte budget.
 */
constexpr auto SuspendedFrameLifetime = std::chrono::milliseconds(250);
/**
 * A frame the client never acknowledged (a client that stopped acking without
 * suspending) stops counting after this long, so a broken client slows the
 * stream to a trickle instead of freezing it.
 */
constexpr auto AckTimeout = std::chrono::seconds(10);
/**
 * While the window is full, at most this many frames per monitor wait in the
 * send queue. Beyond it the monitor's queued frames are dropped (every P-frame
 * needs its predecessor, so they go together), later P-frames are dropped too,
 * and a keyframe is requested: the next picture the client gets is the newest.
 */
constexpr int MaxHeldFramesPerMonitor = 4;
/// Between two keyframe requests from coalescing, per monitor.
constexpr auto CoalesceKeyFrameMinInterval = std::chrono::seconds(1);

struct WindowLimits {
    int frames = MinInFlightFrames;
    int64_t bytes = MinInFlightBytes;
    std::chrono::microseconds suspendedLifetime = SuspendedFrameLifetime;
    bool operator==(const WindowLimits &) const = default;
};

/**
 * The window for \a frameRate (per surface, capped rate), \a surfaces
 * surfaces, a minimum RTT of \a minimumRtt (0 = unknown) and a measured
 * rate of \a rateKbps (0 = unknown).
 */
inline WindowLimits windowLimits(int frameRate, int surfaces, std::chrono::microseconds minimumRtt, uint32_t rateKbps)
{
    using namespace std::chrono;
    const int count = std::max(1, surfaces);
    const auto rtt = std::max(minimumRtt, microseconds(0));
    const double seconds = duration<double>(rtt + AckAllowance).count();
    const int frames = int(std::ceil(std::max(1, frameRate) * count * seconds));
    WindowLimits limits;
    limits.frames = std::clamp(frames, MinInFlightFrames * count, MaxInFlightFrames * count);
    const double bytes = double(rateKbps) * 1000.0 / 8.0 * duration<double>(rtt + InFlightByteHorizon).count();
    limits.bytes = std::max<int64_t>(MinInFlightBytes, int64_t(bytes));
    limits.suspendedLifetime = duration_cast<microseconds>(SuspendedFrameLifetime) + rtt;
    return limits;
}

/**
 * The frames sent and not yet acknowledged, and whether the client suspended
 * acknowledgements (AUD-P6).
 *
 * MS-RDPEGFX 2.2.2.13: a FRAME_ACKNOWLEDGE with queueDepth
 * SUSPEND_FRAME_ACKNOWLEDGEMENT (0xFFFFFFFF) tells the server to stop
 * expecting acks. Without handling it every later frame stayed "pending"
 * forever, the set grew without bound, and the adaptive-quality backlog
 * signal read the silence as a client that cannot keep up. While suspended
 * nothing is pending and pending() is 0. Any later ack with another
 * queueDepth resumes tracking.
 *
 * AUD-FIX4 D1: it also keeps each frame's size and send time, for the
 * in-flight window (canSend()). While suspended, the frames sent in the last
 * WindowLimits::suspendedLifetime count as in flight instead.
 *
 * Not thread-safe: VideoStream guards it with pendingFramesMutex.
 */
class FrameAckTracker
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint32_t SuspendFrameAcknowledgement = 0xFFFFFFFF;

    enum class Ack {
        /** A pending frame was acknowledged. */
        Acknowledged,
        /** The frame id was not pending (already acked, or sent before a reset). */
        Unknown,
        /** The client suspended acknowledgements with this ack. */
        Suspended,
        /** The first ack after a suspension: tracking resumes. */
        Resumed,
    };

    void frameSent(uint32_t frameId, int64_t bytes = 0, Clock::time_point now = Clock::now())
    {
        if (m_suspended) {
            m_recent.push_back({bytes, now});
            m_recentBytes += bytes;
        } else {
            m_pending[frameId] = {bytes, now};
            m_pendingBytes += bytes;
        }
    }

    Ack acknowledge(uint32_t frameId, uint32_t queueDepth)
    {
        if (queueDepth == SuspendFrameAcknowledgement) {
            const bool wasSuspended = m_suspended;
            m_suspended = true;
            clearPending();
            return wasSuspended ? Ack::Unknown : Ack::Suspended;
        }
        if (m_suspended) {
            // Frames sent while suspended were never tracked, so this id is
            // not pending; the ack itself is the resume.
            m_suspended = false;
            m_recent.clear();
            m_recentBytes = 0;
            erase(frameId);
            return Ack::Resumed;
        }
        return erase(frameId) ? Ack::Acknowledged : Ack::Unknown;
    }

    /** Frames still expected to be acknowledged; 0 while suspended. */
    int pending() const
    {
        return int(m_pending.size());
    }
    bool suspended() const
    {
        return m_suspended;
    }
    /** A reset or close: nothing is outstanding. Keeps the suspension, which is per connection. */
    void clear()
    {
        clearPending();
        m_recent.clear();
        m_recentBytes = 0;
    }

    /**
     * Forget what no longer counts as in flight at \a now: while suspended the
     * frames older than \a limits' lifetime, otherwise the frames unacknowledged
     * for AckTimeout. Returns how many unacknowledged frames timed out.
     */
    int expire(Clock::time_point now, const WindowLimits &limits)
    {
        while (!m_recent.empty() && now - m_recent.front().sentAt >= limits.suspendedLifetime) {
            m_recentBytes -= m_recent.front().bytes;
            m_recent.pop_front();
        }
        int timedOut = 0;
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (now - it->second.sentAt >= AckTimeout) {
                m_pendingBytes -= it->second.bytes;
                it = m_pending.erase(it);
                ++timedOut;
            } else {
                ++it;
            }
        }
        return timedOut;
    }

    /** What the window counts: the pending frames, or while suspended the recently sent ones. */
    int inFlightFrames() const
    {
        return m_suspended ? int(m_recent.size()) : int(m_pending.size());
    }
    int64_t inFlightBytes() const
    {
        return m_suspended ? m_recentBytes : m_pendingBytes;
    }

    /**
     * Whether another frame may be sent now under \a limits. \a socketQueuedBytes
     * is what the kernel still holds for the connection's socket (SIOCOUTQ, -1 =
     * unknown); it only counts while acks are suspended, when it is the one
     * backpressure signal left.
     */
    bool canSend(const WindowLimits &limits, int64_t socketQueuedBytes = -1) const
    {
        const int frames = inFlightFrames();
        if (frames >= limits.frames) {
            return false;
        }
        if (frames > 0 && inFlightBytes() >= limits.bytes) {
            return false;
        }
        if (m_suspended && socketQueuedBytes >= limits.bytes) {
            return false;
        }
        return true;
    }

private:
    struct Sent {
        int64_t bytes = 0;
        Clock::time_point sentAt;
    };
    bool erase(uint32_t frameId)
    {
        const auto it = m_pending.find(frameId);
        if (it == m_pending.end()) {
            return false;
        }
        m_pendingBytes -= it->second.bytes;
        m_pending.erase(it);
        return true;
    }
    void clearPending()
    {
        m_pending.clear();
        m_pendingBytes = 0;
    }

    std::unordered_map<uint32_t, Sent> m_pending;
    int64_t m_pendingBytes = 0;
    std::deque<Sent> m_recent;
    int64_t m_recentBytes = 0;
    bool m_suspended = false;
};

/**
 * AUD-FIX4 D1: while the send window is full, keep at most \a maxHeld frames of
 * each monitor in \a queue. A monitor over the limit loses every queued frame
 * (an encoded P-frame is useless without its predecessors) and its index is
 * added to \a starved: the caller drops that monitor's P-frames until its next
 * keyframe and asks the encoder for one. Returns the number of frames dropped.
 */
template<class Queue>
int coalesceHeldFrames(Queue &queue, int maxHeld, std::vector<int> &starved)
{
    std::unordered_map<int, int> perMonitor;
    for (const auto &frame : queue) {
        ++perMonitor[frame.monitorIndex];
    }
    int dropped = 0;
    for (const auto &[monitor, count] : perMonitor) {
        if (count <= maxHeld) {
            continue;
        }
        const int index = monitor;
        queue.removeIf([index](const auto &queued) {
            return queued.monitorIndex == index;
        });
        dropped += count;
        starved.push_back(index);
    }
    std::sort(starved.begin(), starved.end());
    return dropped;
}

}
}
