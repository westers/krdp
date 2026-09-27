// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

namespace KRdp
{

/**
 * Closed-stream recovery schedule of PlasmaScreencastV1Session.
 *
 * A DPMS wake makes KWin tear down and re-add every output, so the session
 * waits for the output set to settle, keeps retrying for a while, and only
 * settles for a workspace stream in the last few attempts. Once every attempt
 * has failed the session reports error() (AUD-P4) so the connection is closed
 * with a reason the client can show, instead of streaming nothing forever.
 *
 * Pure, so the schedule is unit tested.
 */
namespace StreamRecoveryPolicy
{

constexpr int MaxAttempts = 24;
constexpr int IntervalMs = 500;
constexpr int SettleMs = 750;
constexpr int WorkspaceFallbackAttempts = 4;

enum class Next {
    /** The attempt recovered the stream: nothing more to do. */
    Done,
    /** Schedule attempt + 1 after IntervalMs. */
    Retry,
    /** Every attempt failed: report the session as failed. */
    GiveUp,
};

/** Whether attempt (0-based) may fall back to the whole workspace. */
constexpr bool allowWorkspaceFallback(int attempt)
{
    return attempt >= MaxAttempts - WorkspaceFallbackAttempts;
}

constexpr Next next(int attempt, bool recovered)
{
    if (recovered) {
        return Next::Done;
    }
    return attempt + 1 < MaxAttempts ? Next::Retry : Next::GiveUp;
}

} // namespace StreamRecoveryPolicy
} // namespace KRdp
