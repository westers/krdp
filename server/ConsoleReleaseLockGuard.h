// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

// OPT-060 S3 / OPT-049: a layout release (replaced outputs removed, physical outputs restored) within
// about two seconds of the lock screen appearing killed kscreenlocker_greet and the lock was not re-armed.
// Pure decisions only; the worker supplies the observations (ScreenSaver GetActive / logind LockedHint,
// greeter process) and performs the actions (hold, re-lock). Every wait is bounded and the disconnect path
// never waits for the lock: the outputs are restored first, the guard only decides about the lock afterwards.

#include <QtGlobal>

#include <algorithm>
#include <optional>

namespace KRdp::ConsoleReleaseLock
{
/// A lock transition this recent makes an immediate output release risky.
constexpr qint64 SettleMs = 2000;
/// The pre-release hold never exceeds this, whatever the lock does (fail open: restore anyway).
constexpr qint64 MaxHoldMs = 2500;
constexpr int MaxRelockAttempts = 3;
/// After the restore is verified, let the greeter settle this long before the first look.
constexpr qint64 FirstCheckDelayMs = 500;
/// After a re-lock request, wait this long before looking again.
constexpr qint64 RelockWaitMs = 1500;
/// Whole post-restore verification, from the end of the restore; then give up with a warning.
constexpr qint64 TotalBoundMs = 9000;

struct Observation {
    std::optional<bool> locked; ///< Session lock state; nullopt when no source answered.
    std::optional<bool> greeterAlive; ///< kscreenlocker_greet for this user; nullopt when unknown.
};

/// Locked, and the greeter is not known to be dead.
inline bool lockHolds(const Observation &observation)
{
    return observation.locked.value_or(false) && observation.greeterAlive.value_or(true);
}

/**
 * Milliseconds to wait before starting the release so the lock transition settles.
 * @p sinceLockChange: time since the last observed lock/unlock edge (nullopt: none seen).
 * @p heldMs: how long this release has already been held. Never waits past MaxHoldMs in total.
 */
inline qint64 holdBeforeRelease(std::optional<qint64> sinceLockChange, qint64 heldMs)
{
    if (!sinceLockChange || *sinceLockChange < 0 || *sinceLockChange >= SettleMs) return 0;
    return std::clamp<qint64>(std::min(SettleMs - *sinceLockChange, MaxHoldMs - heldMs), 0, SettleMs);
}

struct Context {
    bool lockedBefore = false; ///< LockedHint / screensaver state recorded before the release began.
    bool lockSeenDuringWindow = false; ///< A lock edge arrived between release start and verified restore.
    bool expectLocked() const { return lockedBefore || lockSeenDuringWindow; }
};

enum class Step {
    Done, ///< Nothing to do, or the lock holds.
    Relock, ///< Ask for the lock again, wait RelockWaitMs, observe again.
    GiveUp, ///< Bound reached: warn once, leave the session as it is. Never loops.
};

struct Verdict {
    Step step = Step::Done;
    const char *reason = "";
};

/**
 * @p attempts re-lock requests already made, @p elapsedMs time since the restore was verified.
 */
inline Verdict afterRestore(const Context &context, const Observation &observation, int attempts, qint64 elapsedMs)
{
    if (!context.expectLocked()) return {Step::Done, "session was not locked"};
    if (lockHolds(observation)) return {Step::Done, "lock holds with a live greeter"};
    if (attempts >= MaxRelockAttempts || elapsedMs >= TotalBoundMs) return {Step::GiveUp, "re-lock did not verify within the bound"};
    if (observation.locked.value_or(false)) return {Step::Relock, "greeter is gone while the session is marked locked"};
    return {Step::Relock, observation.locked ? "session is unlocked after the release" : "lock state unknown after the release"};
}
}
