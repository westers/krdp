// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

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

}
}
