// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cstdint>
#include <unordered_set>

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
 * The frames sent and not yet acknowledged, and whether the client suspended
 * acknowledgements (AUD-P6).
 *
 * MS-RDPEGFX 2.2.2.13: a FRAME_ACKNOWLEDGE with queueDepth
 * SUSPEND_FRAME_ACKNOWLEDGEMENT (0xFFFFFFFF) tells the server to stop
 * expecting acks. Without handling it every later frame stayed "pending"
 * forever, the set grew without bound, and the adaptive-quality backlog
 * signal read the silence as a client that cannot keep up. While suspended
 * nothing is tracked and pending() is 0. Any later ack with another
 * queueDepth resumes tracking.
 *
 * Not thread-safe: VideoStream guards it with pendingFramesMutex.
 */
class FrameAckTracker
{
public:
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

    void frameSent(uint32_t frameId)
    {
        if (!m_suspended) {
            m_pending.insert(frameId);
        }
    }

    Ack acknowledge(uint32_t frameId, uint32_t queueDepth)
    {
        if (queueDepth == SuspendFrameAcknowledgement) {
            const bool wasSuspended = m_suspended;
            m_suspended = true;
            m_pending.clear();
            return wasSuspended ? Ack::Unknown : Ack::Suspended;
        }
        if (m_suspended) {
            // Frames sent while suspended were never tracked, so this id is
            // not pending; the ack itself is the resume.
            m_suspended = false;
            m_pending.erase(frameId);
            return Ack::Resumed;
        }
        return m_pending.erase(frameId) ? Ack::Acknowledged : Ack::Unknown;
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
        m_pending.clear();
    }

private:
    std::unordered_set<uint32_t> m_pending;
    bool m_suspended = false;
};

}
}
