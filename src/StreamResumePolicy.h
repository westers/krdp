// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QtGlobal>

namespace KRdp
{

/**
 * What a session does when its streaming is switched back on after it was stopped.
 *
 * KPipeWire resets the stream's node ID to 0 when the producer thread of a stopped stream
 * ends, and start() on a stream without a node (or one still shutting down) only logs and
 * returns. A bare start() on resume therefore froze the picture silently while the
 * connection stayed up. The compositor's screencast outlives the consumer, so the session
 * re-attaches the node it remembers through its deferred restart, or asks for a new
 * screencast when there is none.
 *
 * Pure, so the decision table is unit tested.
 */
namespace StreamResumePolicy
{

enum class State {
    Idle,
    Recording,
    Rendering,
};

enum class Action {
    /** Nothing to do: running, or a restart/attach is already on its way. */
    None,
    /** The consumer still has its node and is idle: start() works. */
    Start,
    /** Re-attach `node` through the deferred restart (it waits for the old producer to exit). */
    Reattach,
    /** No node and no screencast request: create a new screencast (reports error() if it cannot). */
    Recreate,
};

struct Decision {
    Action action = Action::None;
    quint32 node = 0;
    friend bool operator==(const Decision &, const Decision &) = default;
};

/**
 * @param nodeId the consumer's current node (0 after its producer ended)
 * @param restartPending a deferred restart/attach is already scheduled
 * @param rememberedNode the node of the live screencast request (0 when unknown)
 * @param requestAlive a screencast request exists (its created() signal may still be pending)
 */
constexpr Decision decide(quint32 nodeId, State state, bool restartPending, quint32 rememberedNode, bool requestAlive)
{
    if (restartPending) {
        return {Action::None, 0};
    }
    if (nodeId != 0) {
        if (state == State::Recording) {
            return {Action::None, 0};
        }
        return state == State::Idle ? Decision{Action::Start, nodeId} : Decision{Action::Reattach, nodeId};
    }
    if (rememberedNode != 0 && requestAlive) {
        return {Action::Reattach, rememberedNode};
    }
    if (requestAlive) {
        return {Action::None, 0}; // created() is still to come and attaches the node
    }
    return {Action::Recreate, 0};
}

} // namespace StreamResumePolicy
} // namespace KRdp
