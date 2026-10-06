// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

// OPT-060 S3 / OPT-049: a layout release (replaced outputs removed, physical outputs restored) within
// about two seconds of the lock screen appearing killed kscreenlocker_greet and the lock was not re-armed.
// Pure decisions only; the worker supplies the observations (ScreenSaver GetActive / logind LockedHint,
// greeter process) and performs the actions (hold, re-lock). Every wait is bounded and the disconnect path
// never waits for the lock: the outputs are restored first, the guard only decides about the lock afterwards.

#include <QtGlobal>

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

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

/// The lock screen's process. /proc/PID/comm is the kernel's TASK_COMM_LEN - 1 = 15 characters, so the
/// real name (19) never appears there in full: `kscreenlocker_g` (OPT-060 D1; comparing the full name
/// made every release conclude "greeter gone" and re-lock three times for nothing).
constexpr std::string_view GreeterName = "kscreenlocker_greet";
constexpr std::size_t KernelCommLength = 15;

/// @p comm is /proc/PID/comm without its newline: the full name or the kernel's 15-character truncation of it.
inline bool greeterCommMatches(std::string_view comm)
{
    return comm == GreeterName || (comm.size() == KernelCommLength && GreeterName.substr(0, KernelCommLength) == comm);
}

/// @p cmdline is /proc/PID/cmdline (NUL separated): its program name, the confirmation after a comm match.
inline bool greeterCmdlineMatches(std::string_view cmdline)
{
    const auto argv0 = cmdline.substr(0, cmdline.find('\0'));
    return argv0.substr(argv0.rfind('/') == std::string_view::npos ? 0 : argv0.rfind('/') + 1) == GreeterName;
}

/// Whether a kscreenlocker_greet owned by @p uid runs under @p procRoot (/proc; tests pass a fake tree). The
/// comm file prefilters (15 characters, see above) and the cmdline confirms the real name.
inline bool greeterRunning(const std::filesystem::path &procRoot, uid_t uid)
{
    std::error_code error;
    for (std::filesystem::directory_iterator it(procRoot, std::filesystem::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        std::ifstream comm(it->path() / "comm");
        std::string commName;
        if (!comm || !std::getline(comm, commName) || !greeterCommMatches(commName)) continue;
        std::ifstream cmdline(it->path() / "cmdline", std::ios::binary);
        const std::string argv(std::istreambuf_iterator<char>(cmdline), {});
        if (!greeterCmdlineMatches(argv.substr(0, 512))) continue;
        struct stat st {};
        if (::stat(it->path().c_str(), &st) == 0 && st.st_uid == uid) return true;
    }
    return false;
}

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
