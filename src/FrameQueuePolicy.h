// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <type_traits>
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
 * - frames: the frames that fit in one RTT plus AckAllowance at the current
 *   frame rate, per surface, between MinInFlightFrames and MaxInFlightFrames
 *   (a LAN at 60 fps gets 4, a 100 ms path 10). AUD-FIX7 F2: while the link
 *   is clear, the RTT is the larger of the minimum network RTT and the recent
 *   frame-ack latency
 *   (FrameAckTracker::ackLatency(), p75 of the last AckLatencyRecent), so a
 *   client that takes 285 ms to decode and present - Sol's stock client on
 *   :3391, 2026-09-28 - gets the 11 frames it needs for 30 fps instead of 4
 *   (which capped it at 14 fps and turned the queue behind the window into a
 *   keyframe a second);
 * - bytes: the measured rate for one minimum RTT plus InFlightByteHorizon,
 *   at least MinInFlightBytes. A single frame larger than the budget (a
 *   keyframe) still goes out when nothing else is in flight. AUD-FIX7 F2:
 *   while the link is clear (the socket has sent what it was given, so the
 *   delay is the client's, not the link's) it also covers the frame window of
 *   average frames plus the largest keyframe; on a congested link it stays the
 *   link's, so the D1 bound on what FreeRDP queues holds.
 */
constexpr int MinInFlightFrames = 4;
constexpr int MaxInFlightFrames = 16;
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
/**
 * The first gap between two keyframe requests from coalescing, per monitor.
 * AUD-FIX6 F2: each further request waits twice as long (1 s, 2 s, 4 s, ... up
 * to CoalesceKeyFrameMaxInterval) until a keyframe of that monitor is
 * acknowledged; see KeyFrameRequestBackoff.
 */
constexpr auto CoalesceKeyFrameMinInterval = std::chrono::seconds(1);
constexpr auto CoalesceKeyFrameMaxInterval = std::chrono::seconds(16);
/// AUD-FIX7 F2: the keyframe-request back-off starts over only after this long without a drop.
constexpr auto BackoffQuietReset = std::chrono::seconds(10);

/**
 * AUD-FIX6 F2: keyframes and the byte budget.
 *
 * On Sol's console host (:3391, 2026-09-28) the worker's keyframe was 265 KB,
 * twice MinInFlightBytes. With the keyframe in flight the byte budget was used
 * up, the P-frames behind it waited, more than MaxHeldFramesPerMonitor of them
 * were dropped and another keyframe was requested - which was again 265 KB, so
 * the stock client got one keyframe a second and nothing else. A 4K or
 * multi-monitor krdpserver stream (keyframes of 1 MB and more) and our own
 * client would do the same. So:
 * - the byte budget is at least KeyFrameBudgetFactor times the largest
 *   keyframe of the last KeyFrameSizeHorizon (the one waiting to go included),
 *   so the P-frames behind a keyframe can follow it;
 * - while a monitor's keyframe is unacknowledged its later frames are held,
 *   not dropped, for up to KeyFrameHold of frames (keyFrameHoldFrames()), and
 *   no keyframe is requested for that monitor (it would only queue another
 *   one behind it);
 * - keyframe requests after a drop back off (KeyFrameRequestBackoff).
 */
constexpr int KeyFrameBudgetFactor = 2;
constexpr auto KeyFrameSizeHorizon = std::chrono::seconds(30);
constexpr auto KeyFrameHold = std::chrono::seconds(2);
constexpr int MaxKeyFrameHoldFrames = 240;

/**
 * AUD-FIX7 F2: the frame-ack latency the window follows. The p75 of the
 * acknowledgements of the last AckLatencyRecent (at least AckLatencyMinSamples
 * of them: fewer, and the last AckLatencyMinSamples of the horizon count;
 * none there, unknown); its floor, the smallest of the last
 * AckLatencyHorizon, is the client's own pipeline delay (decode and present)
 * without queueing, which sets how many frames behind a healthy client is.
 */
constexpr auto AckLatencyRecent = std::chrono::seconds(2);
constexpr auto AckLatencyHorizon = std::chrono::seconds(10);
constexpr int AckLatencyMinSamples = 8;
constexpr size_t AckLatencyMaxSamples = 1024;

struct WindowLimits {
    int frames = MinInFlightFrames;
    int64_t bytes = MinInFlightBytes;
    std::chrono::microseconds suspendedLifetime = SuspendedFrameLifetime;
    bool operator==(const WindowLimits &) const = default;
};

/**
 * The window for \a frameRate (per surface, capped rate), \a surfaces
 * surfaces, a minimum RTT of \a minimumRtt (0 = unknown), a measured
 * rate of \a rateKbps (0 = unknown) and the largest recent keyframe
 * \a keyFrameBytes (0 = none seen; AUD-FIX6 F2). AUD-FIX7 F2: with
 * \a linkClear, the recent frame-ack latency \a ackLatency (0 = unknown) sizes
 * the frame count when it is longer than the minimum RTT, and the byte budget
 * covers that many frames of \a averageFrameBytes plus the keyframe.
 */
inline WindowLimits windowLimits(int frameRate,
                                 int surfaces,
                                 std::chrono::microseconds minimumRtt,
                                 uint32_t rateKbps,
                                 int64_t keyFrameBytes = 0,
                                 std::chrono::microseconds ackLatency = {},
                                 int64_t averageFrameBytes = 0,
                                 bool linkClear = false)
{
    using namespace std::chrono;
    const int count = std::max(1, surfaces);
    const auto rtt = std::max(minimumRtt, microseconds(0));
    // Only a clear link lets the ack latency size the window: on a congested one the ack latency
    // is the link's queue, and a bigger window would only lengthen it (the D1 bound).
    const auto frameRtt = linkClear ? std::max(rtt, ackLatency) : rtt;
    const double seconds = duration<double>(frameRtt + AckAllowance).count();
    const int frames = int(std::ceil(std::max(1, frameRate) * count * seconds));
    WindowLimits limits;
    limits.frames = std::clamp(frames, MinInFlightFrames * count, MaxInFlightFrames * count);
    const double bytes = double(rateKbps) * 1000.0 / 8.0 * duration<double>(rtt + InFlightByteHorizon).count();
    const int64_t keyFrame = std::max<int64_t>(0, keyFrameBytes);
    limits.bytes = std::max({MinInFlightBytes, int64_t(bytes), KeyFrameBudgetFactor * keyFrame});
    if (linkClear && ackLatency > rtt && averageFrameBytes > 0) {
        // The client, not the link, holds the frames: let the byte budget follow the frame window.
        limits.bytes = std::max(limits.bytes, int64_t(double(limits.frames) * double(averageFrameBytes) * 1.25) + keyFrame);
    }
    limits.suspendedLifetime = duration_cast<microseconds>(SuspendedFrameLifetime) + rtt;
    return limits;
}

/**
 * AUD-FIX7 F2: whether the link carried what was sent: the socket's queue
 * (\a socketQueuedBytes, SIOCOUTQ: unsent plus unacknowledged by TCP; -1 =
 * unknown) is under LinkClearBytes (or the largest recent keyframe). On a slow
 * link the frames in flight sit in that queue; with a slow client they are
 * already across. False while unknown. VideoStream also requires LinkClearHold
 * of clear checks in a row.
 */
constexpr int64_t LinkClearBytes = 64 * 1024;
/// A link counts as clear only once no check saw it otherwise for this long (hysteresis).
constexpr auto LinkClearHold = std::chrono::seconds(2);
inline bool linkClear(int64_t socketQueuedBytes, int64_t keyFrameBytes = 0)
{
    // A keyframe on its way out of a fast socket is not congestion.
    return socketQueuedBytes >= 0 && socketQueuedBytes < std::max(LinkClearBytes, keyFrameBytes);
}

/**
 * AUD-FIX7 F2: how many unacknowledged frames a client that keeps up still
 * has: its pipeline delay (\a baseAckLatency, FrameAckTracker's floor) at
 * \a frameRate on \a surfaces surfaces. The adaptive-quality backlog rule
 * reads a client as behind only beyond this (AdaptiveQuality::BacklogFrames
 * at least), so a slow-but-steady decoder is not "congested".
 */
inline int backlogFrames(int frameRate, int surfaces, std::chrono::microseconds baseAckLatency, int minimum)
{
    const double frames = std::ceil(std::max(1, frameRate) * std::max(1, surfaces) * std::chrono::duration<double>(baseAckLatency).count());
    return std::max(minimum, int(frames) + 1);
}

/**
 * AUD-FIX6 F2: how many frames of one monitor may wait behind its
 * unacknowledged keyframe before they are coalesced: KeyFrameHold at
 * \a frameRate, at least MaxHeldFramesPerMonitor and at most
 * MaxKeyFrameHoldFrames.
 */
inline int keyFrameHoldFrames(int frameRate)
{
    const double frames = std::ceil(std::max(1, frameRate) * std::chrono::duration<double>(KeyFrameHold).count());
    return std::clamp(int(frames), MaxHeldFramesPerMonitor, MaxKeyFrameHoldFrames);
}

/**
 * AUD-FIX6 F2: whether a monitor is (still) working off frames that piled up
 * behind its keyframe, given whether it was (\a wasBacklog), whether its
 * keyframe is unacknowledged (\a keyFrameInFlight) and how many of its frames
 * are queued (\a queued). Such a monitor may hold keyFrameHoldFrames() frames:
 * the backlog behind a big keyframe starts to drain only once the keyframe is
 * acknowledged, and coalescing it then would drop exactly the frames the hold
 * kept and ask for another keyframe. It ends once the monitor is back at
 * MaxHeldFramesPerMonitor or fewer.
 */
inline bool keyFrameBacklog(bool wasBacklog, bool keyFrameInFlight, int queued)
{
    if (keyFrameInFlight) {
        return true;
    }
    return wasBacklog && queued > MaxHeldFramesPerMonitor;
}

/**
 * AUD-FIX6 F2: the largest keyframe recorded in the last KeyFrameSizeHorizon.
 * Not thread-safe.
 */
class KeyFramePeak
{
public:
    using Clock = std::chrono::steady_clock;
    void record(int64_t bytes, Clock::time_point now)
    {
        // A smaller or equal older entry can never be the maximum again.
        while (!m_entries.empty() && m_entries.back().bytes <= bytes) {
            m_entries.pop_back();
        }
        m_entries.push_back({bytes, now});
    }
    int64_t largest(Clock::time_point now)
    {
        while (!m_entries.empty() && now - m_entries.front().at >= KeyFrameSizeHorizon) {
            m_entries.pop_front();
        }
        return m_entries.empty() ? 0 : m_entries.front().bytes;
    }
    void clear()
    {
        m_entries.clear();
    }

private:
    struct Entry {
        int64_t bytes = 0;
        Clock::time_point at;
    };
    std::deque<Entry> m_entries; // sizes strictly decreasing from the front
};

/**
 * AUD-FIX6 F2: when a monitor whose frames were coalesced away may ask for a
 * keyframe. Never while a keyframe of that monitor is still unacknowledged
 * (the answer would only queue behind it, and the frames behind it are the
 * ones that were dropped). Otherwise the first request goes at once, the next
 * CoalesceKeyFrameMinInterval later, then 2x, 4x, ... up to
 * CoalesceKeyFrameMaxInterval. AUD-FIX7 F2: an acknowledged keyframe no longer
 * resets the back-off (a client that acks every keyframe and falls behind
 * again got a request a second: 76 in 77 s on Sol's :3391); only
 * BackoffQuietReset without a drop does (dropped()). Not thread-safe; one per
 * monitor.
 */
class KeyFrameRequestBackoff
{
public:
    using Clock = std::chrono::steady_clock;
    /** Whether to request now; if so, the request is counted. */
    bool shouldRequest(Clock::time_point now, bool keyFrameInFlight)
    {
        if (keyFrameInFlight) {
            return false;
        }
        if (m_requests > 0 && now - m_lastRequest < interval()) {
            return false;
        }
        m_lastRequest = now;
        ++m_requests;
        return true;
    }
    /**
     * Frames of this monitor were coalesced away at \a now. The first drop after
     * BackoffQuietReset without one starts the back-off over.
     */
    void dropped(Clock::time_point now)
    {
        if (m_lastDrop != Clock::time_point{} && now - m_lastDrop >= BackoffQuietReset) {
            m_requests = 0;
        }
        m_lastDrop = now;
    }
    /** The wait after the last request before the next may go. */
    std::chrono::milliseconds interval() const
    {
        using namespace std::chrono;
        const int doublings = std::clamp(m_requests - 1, 0, 16);
        const auto wait = duration_cast<milliseconds>(CoalesceKeyFrameMinInterval) * (int64_t(1) << doublings);
        return std::min(wait, duration_cast<milliseconds>(CoalesceKeyFrameMaxInterval));
    }
    int requests() const
    {
        return m_requests;
    }

private:
    Clock::time_point m_lastRequest{};
    Clock::time_point m_lastDrop{};
    int m_requests = 0; // since the back-off last started over
};

/**
 * AUD-FIX12: how long a surface may drop every frame it gets while it waits for a keyframe
 * (starved behind a coalesce: queueFrame(); a chain waiting for a keyframe of its codec:
 * sendFrame()) before the keyframe is asked for again, whatever the back-off
 * (KeyFrameRequestBackoff) or an unacknowledged keyframe in flight says; and again every
 * SilentSurfaceRetry while it still sends nothing. Frames are being captured, so the picture
 * must not freeze: on ace (c27909d) the back-off and the unacked-keyframe rule could hold a
 * starved monitor for up to 16 s and 10 s. The session's EncoderWatchdog restarts an encoder
 * that answers none of these requests.
 */
constexpr auto SilentSurfaceLimit = std::chrono::milliseconds(1000);
constexpr auto SilentSurfaceRetry = std::chrono::milliseconds(1000);

/** AUD-FIX12: one surface's run of dropped frames since it last sent one (see SilentSurfaceLimit). Not thread-safe. */
class SurfaceSilence
{
public:
    using Clock = std::chrono::steady_clock;
    /** A frame of this surface was dropped for want of a keyframe at \a now. */
    void dropped(Clock::time_point now)
    {
        if (m_since == Clock::time_point{}) {
            m_since = now;
        }
    }
    /** A frame of this surface went out. */
    void delivered()
    {
        m_since = {};
        m_lastForced = {};
        m_forced = 0;
    }
    /** Whether to ask for a keyframe now regardless of the back-off; if so, it is counted. */
    bool shouldForce(Clock::time_point now)
    {
        if (m_since == Clock::time_point{} || now - m_since < SilentSurfaceLimit) {
            return false;
        }
        if (m_lastForced != Clock::time_point{} && now - m_lastForced < SilentSurfaceRetry) {
            return false;
        }
        m_lastForced = now;
        ++m_forced;
        return true;
    }
    /** Forced requests in this run. */
    int forced() const
    {
        return m_forced;
    }
    Clock::time_point silentSince() const
    {
        return m_since;
    }

private:
    Clock::time_point m_since{};
    Clock::time_point m_lastForced{};
    int m_forced = 0;
};

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

    /**
     * \a keyFrame and \a monitorIndex (AUD-FIX6 F2) say whether the frame is a
     * keyframe and of which monitor, for keyFrameInFlight().
     */
    void frameSent(uint32_t frameId, int64_t bytes = 0, Clock::time_point now = Clock::now(), bool keyFrame = false, int monitorIndex = 0)
    {
        if (m_suspended) {
            m_recent.push_back({bytes, now});
            m_recentBytes += bytes;
        } else {
            m_pending[frameId] = {bytes, now, keyFrame, monitorIndex};
            m_pendingBytes += bytes;
        }
    }

    Ack acknowledge(uint32_t frameId, uint32_t queueDepth, Clock::time_point now = Clock::now())
    {
        m_acknowledgedKeyFrame = -1;
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
            erase(frameId, now);
            return Ack::Resumed;
        }
        return erase(frameId, now) ? Ack::Acknowledged : Ack::Unknown;
    }

    /**
     * AUD-FIX7 F2: the p75 of the send-to-ack latency of the frames acknowledged
     * in the last AckLatencyRecent before \a now (or of the last
     * AckLatencyMinSamples in AckLatencyHorizon when fewer came); 0 when the
     * horizon has fewer than that (unknown).
     */
    std::chrono::microseconds ackLatency(Clock::time_point now) const
    {
        // The last AckLatencyRecent, or - when acks are sparse (a stall, a slow link) - the last
        // AckLatencyMinSamples of the horizon, so the window does not collapse while nothing is acked.
        std::vector<Clock::duration> recent;
        for (auto it = m_ackLatencies.rbegin(); it != m_ackLatencies.rend() && now - it->at < AckLatencyHorizon; ++it) {
            if (now - it->at >= AckLatencyRecent && int(recent.size()) >= AckLatencyMinSamples) {
                break;
            }
            recent.push_back(it->latency);
        }
        if (int(recent.size()) < AckLatencyMinSamples) {
            return {};
        }
        const size_t index = (recent.size() * 3) / 4;
        std::nth_element(recent.begin(), recent.begin() + index, recent.end());
        return std::chrono::duration_cast<std::chrono::microseconds>(recent[index]);
    }
    /**
     * AUD-FIX7 F2: the smallest ack latency of the last AckLatencyHorizon: the
     * client's own delay without queueing. 0 with fewer than
     * AckLatencyMinSamples acknowledgements in that time.
     */
    std::chrono::microseconds baseAckLatency(Clock::time_point now) const
    {
        int samples = 0;
        std::optional<Clock::duration> lowest;
        for (auto it = m_ackLatencies.rbegin(); it != m_ackLatencies.rend() && now - it->at < AckLatencyHorizon; ++it) {
            ++samples;
            lowest = lowest ? std::min(*lowest, it->latency) : it->latency;
        }
        return samples >= AckLatencyMinSamples && lowest ? std::chrono::duration_cast<std::chrono::microseconds>(*lowest) : std::chrono::microseconds(0);
    }

    /**
     * The monitor of the keyframe the last acknowledge() acknowledged, or -1
     * when it acknowledged something else (AUD-FIX6 F2).
     */
    int acknowledgedKeyFrame() const
    {
        return m_acknowledgedKeyFrame;
    }
    /**
     * Whether a keyframe of \a monitorIndex was sent and is neither
     * acknowledged nor timed out (AUD-FIX6 F2). Always false while
     * acknowledgements are suspended: nothing is tracked then.
     */
    bool keyFrameInFlight(int monitorIndex) const
    {
        return std::any_of(m_pending.cbegin(), m_pending.cend(), [monitorIndex](const auto &entry) {
            return entry.second.keyFrame && entry.second.monitorIndex == monitorIndex;
        });
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
        bool keyFrame = false;
        int monitorIndex = 0;
    };
    bool erase(uint32_t frameId, Clock::time_point now)
    {
        const auto it = m_pending.find(frameId);
        if (it == m_pending.end()) {
            return false;
        }
        if (now >= it->second.sentAt) {
            while (!m_ackLatencies.empty()
                   && (m_ackLatencies.size() >= AckLatencyMaxSamples || now - m_ackLatencies.front().at >= AckLatencyHorizon)) {
                m_ackLatencies.pop_front();
            }
            m_ackLatencies.push_back({now, now - it->second.sentAt});
        }
        if (it->second.keyFrame) {
            m_acknowledgedKeyFrame = it->second.monitorIndex;
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
    int m_acknowledgedKeyFrame = -1;
    struct AckLatency {
        Clock::time_point at;
        Clock::duration latency;
    };
    std::deque<AckLatency> m_ackLatencies; // oldest first, AckLatencyHorizon at most
};

/**
 * AUD-FIX4 D1: while the send window is full, keep at most \a maxHeld frames of
 * each monitor in \a queue. A monitor over the limit loses every queued frame
 * (an encoded P-frame is useless without its predecessors) and its index is
 * added to \a starved: the caller drops that monitor's P-frames until its next
 * keyframe and asks the encoder for one. Returns the number of frames dropped.
 */
template<class Queue, class Coalesce>
    requires std::is_invocable_r_v<bool, Coalesce, int, int>
int coalesceHeldFrames(Queue &queue, Coalesce coalesce, std::vector<int> &starved)
{
    std::unordered_map<int, int> perMonitor;
    for (const auto &frame : queue) {
        ++perMonitor[frame.monitorIndex];
    }
    int dropped = 0;
    for (const auto &[monitor, count] : perMonitor) {
        if (!coalesce(monitor, count)) {
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

/// The same limit, \a maxHeld, for every monitor.
template<class Queue>
int coalesceHeldFrames(Queue &queue, int maxHeld, std::vector<int> &starved)
{
    return coalesceHeldFrames(
        queue,
        [maxHeld](int, int count) {
            return count > maxHeld;
        },
        starved);
}

/**
 * Whether a monitor with \a queued frames held while the window is full is
 * coalesced (its frames dropped, a keyframe requested). AUD-FIX7 F2: only
 * beyond \a keyFrameHold (keyFrameHoldFrames(), the latency bound). Dropping
 * a queued encoded P-frame breaks the reference chain, so every coalesce
 * costs a keyframe - the loop Sol's :3391 showed (a request a second while
 * the backlog was merely "as big as a keyframe"). Below the bound frames wait,
 * and the source slows down instead (DeliveryThrottle: fewer frames are
 * captured and encoded, none is thrown away after encoding).
 */
inline bool shouldCoalesce(int queued, int keyFrameHold)
{
    return queued > std::max(MaxHeldFramesPerMonitor, keyFrameHold);
}

/**
 * AUD-FIX7 F2: slow the source down instead of dropping encoded frames. Once
 * per adaptive interval (1.5 s): when frames had to wait behind a full window
 * (\a pressure) and the source made clearly more than the client took
 * (\a offered over ThrottleOutpaced times \a delivered, frames per second and
 * surface), the frame rate asked of the capture drops to
 * ThrottleDeliveredShare of what the client took, not below
 * MinThrottledFrameRate. A quiet desktop whose one keyframe waited is not
 * throttled (it offers no more than gets through), and a stream that could
 * not deliver anything keeps its rate (a stall is the ack timeout's
 * business). After ThrottleRaiseAfter without pressure it climbs by
 * ThrottleRaiseStep, back to \a cap (throttle off). Pressure within the wait
 * after a raise doubles the next wait (anti-flap, up to ThrottleMaxRaiseWait);
 * ThrottleCalm without pressure resets it. Not thread-safe.
 */
constexpr int MinThrottledFrameRate = 5;
constexpr double ThrottleDeliveredShare = 0.9;
constexpr double ThrottleOutpaced = 1.15;
constexpr double ThrottleRaiseStep = 1.5;
constexpr auto ThrottleRaiseAfter = std::chrono::seconds(3);
constexpr auto ThrottleMaxRaiseWait = std::chrono::seconds(24);
constexpr auto ThrottleCalm = std::chrono::seconds(60);

class DeliveryThrottle
{
public:
    using Clock = std::chrono::steady_clock;
    /** One interval; returns the frame rate to ask for (\a cap when not throttled). */
    int update(Clock::time_point now, int cap, double offered, double delivered, bool pressure)
    {
        cap = std::max(1, cap);
        if (pressure) {
            const int current = rate(cap);
            const int target = std::max(std::min(MinThrottledFrameRate, cap), int(std::floor(delivered * ThrottleDeliveredShare)));
            if (delivered > 0 && offered > delivered * ThrottleOutpaced && target < current) {
                if (m_raisedAt != Clock::time_point{} && now - m_raisedAt <= m_raiseWait * 2) {
                    m_raiseWait = std::min<Clock::duration>(m_raiseWait * 2, ThrottleMaxRaiseWait); // that raise was too much
                }
                m_rate = target;
                m_raisedAt = {};
                m_changedAt = now;
            }
            m_lastPressure = now;
        } else if (m_rate > 0) {
            const auto quietSince = std::max(m_changedAt, m_lastPressure);
            if (now - quietSince >= m_raiseWait) {
                const int raised = int(std::ceil(m_rate * ThrottleRaiseStep));
                m_rate = raised >= cap ? 0 : raised;
                m_raisedAt = now;
                m_changedAt = now;
            }
        }
        if (m_lastPressure != Clock::time_point{} && now - m_lastPressure >= ThrottleCalm) {
            m_raiseWait = ThrottleRaiseAfter;
        }
        return rate(cap);
    }
    /** The frame rate under \a cap. */
    int rate(int cap) const
    {
        return m_rate > 0 ? std::min(m_rate, cap) : cap;
    }
    bool active() const
    {
        return m_rate > 0;
    }
    std::chrono::milliseconds raiseWait() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(m_raiseWait);
    }
    void reset()
    {
        *this = {};
    }

private:
    int m_rate = 0; // 0 = not throttled
    Clock::time_point m_changedAt{};
    Clock::time_point m_raisedAt{};
    Clock::time_point m_lastPressure{};
    Clock::duration m_raiseWait = ThrottleRaiseAfter;
};

}
}
