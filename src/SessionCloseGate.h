// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

namespace KRdp
{

/**
 * GAP-G5: the hand-over of a "close this connection" request to the thread
 * that owns the FreeRDP peer.
 *
 * Every call that closes or writes to the peer (freerdp_set_error_info,
 * freerdp_send_error_info, freerdp_peer::Close) has to run on the session
 * thread, the one that reads the peer in CheckFileDescriptor(): FreeRDP's
 * transport and rdp state are not safe against a second thread. Any other
 * thread records its request here instead; the session loop takes it at a
 * safe point and performs it. This class is only the state, no FreeRDP.
 *
 * - The first request wins: once any request was accepted, later ones are
 *   Ignored (the close sequence is already on its way, or done).
 * - A request is Queued only while the session loop is active and the caller
 *   is not the session thread. Otherwise it is Inline: the caller performs it
 *   itself, which is safe because no loop is reading the peer (before the
 *   thread starts, after it ended) or because the caller already is that loop.
 * - finishLoop() hands the session thread any request that arrived after its
 *   last look, and from then on submissions are Inline.
 */
class SessionCloseGate
{
public:
    struct Request {
        /// An MS-RDPBCGR ERRINFO_* code to set before the close; none = leave the peer's value.
        std::optional<std::uint32_t> errorInfo;
        /// Also send the Set Error Info PDU now (Close() sends Deactivate All first, after which
        /// a client ignores a later Set Error Info).
        bool sendErrorInfoNow = false;
    };

    enum class Route {
        Queued, ///< Recorded; the session loop performs it.
        Inline, ///< Accepted; the caller performs it on its own thread.
        Ignored, ///< An earlier request was accepted already.
    };

    /** Before the session thread is started (called on the thread that starts it). */
    void beginLoop()
    {
        std::lock_guard lock(m_mutex);
        m_loopActive = true;
    }

    /** The session thread could not be started after beginLoop(). */
    void abortLoop()
    {
        std::lock_guard lock(m_mutex);
        m_loopActive = false;
    }

    /** Any thread. \a onSessionThread: the caller is the thread running the loop. */
    Route submit(const Request &request, bool onSessionThread)
    {
        std::lock_guard lock(m_mutex);
        if (m_accepted) {
            return Route::Ignored;
        }
        m_accepted = true;
        if (m_loopActive && !onSessionThread) {
            m_pending = request;
            return Route::Queued;
        }
        return Route::Inline;
    }

    /** Session thread, at a safe point of its loop: a queued request, once. */
    std::optional<Request> take()
    {
        std::lock_guard lock(m_mutex);
        return std::exchange(m_pending, std::nullopt);
    }

    /** Session thread, when the loop ends: the request that raced the end, if any. */
    std::optional<Request> finishLoop()
    {
        std::lock_guard lock(m_mutex);
        m_loopActive = false;
        return std::exchange(m_pending, std::nullopt);
    }

    bool accepted() const
    {
        std::lock_guard lock(m_mutex);
        return m_accepted;
    }

    bool hasPending() const
    {
        std::lock_guard lock(m_mutex);
        return m_pending.has_value();
    }

private:
    mutable std::mutex m_mutex;
    bool m_loopActive = false;
    bool m_accepted = false;
    std::optional<Request> m_pending;
};

}
