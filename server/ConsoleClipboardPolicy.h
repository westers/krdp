// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include <QString>
#include <QtGlobal>

#include "ConsoleSeat.h"

namespace KRdp::ConsoleClipboardPolicy
{
/**
 * OPT-067 / OPT-072: which directions of the clipboard bridge (text and images alike) a connection may use, decided per state.
 * Pure (no Qt GUI, no I/O), shared by the Console and Virtual hosts. The effective policy is the
 * state's rule AND the administrator's ClipboardDirection; the setting never widens a state's rule.
 *
 *   greeter                      host->client deny, client->host allow (write only: paste a name or password)
 *   locked, same account         both allow (the account PAM just authenticated)
 *   locked, other account        both deny
 *   unlocked, same account       both allow
 *   not the controller (viewer)  both deny
 *   virtual desktop              both allow (always the authenticated owner)
 *   no seat adapter / no uid     both deny
 *
 * OPT-078 adds `files` (file copy, spec 4.5 and the 2026-10-17 amendment 12.1). Per state, for a NEW transfer:
 *
 *   greeter                      both deny
 *   locked, same account         host->client allow, client->host DENY (LockedNewTransfer)
 *   locked, other account        both deny
 *   unlocked, same account       both allow
 *   not the controller (viewer)  both deny
 *   virtual desktop              both allow
 *   no seat adapter / no uid     both deny
 *
 * A transfer that already started when the screen locked CONTINUES (Input::*Started): locking only denies new
 * ones. Control moving away, another account, the greeter, or the settings turning it off still end it.
 * The effective decision is the state's rule AND ClipboardFiles (Input::filesSetting) AND ClipboardDirection.
 */
enum class Route { Console, Virtual };
enum class Direction { Both, HostToClient, ClientToHost, Off };
enum class Reason {
    Allowed, ///< the state allows what the setting allows
    Greeter, ///< host->client is withheld on the login screen
    ForeignLocked, ///< another account's locked screen
    ForeignUnlocked, ///< another account's unlocked desktop (should never be admitted)
    Viewer, ///< not the controlling connection
    NoSeat, ///< no session on the seat or a route with no desktop
    UnknownUser, ///< the connection has no authenticated uid
    Setting, ///< the state would allow more than ClipboardDirection does
    LockedNewTransfer, ///< OPT-078: a NEW client->host file transfer on a locked desktop (an started one continues)
};

inline std::optional<Direction> parseDirection(const QString &value)
{
    if (value == QLatin1String("both")) return Direction::Both;
    if (value == QLatin1String("host-to-client")) return Direction::HostToClient;
    if (value == QLatin1String("client-to-host")) return Direction::ClientToHost;
    if (value == QLatin1String("off")) return Direction::Off;
    return std::nullopt;
}

inline QString directionName(Direction direction)
{
    switch (direction) {
    case Direction::Both: return QStringLiteral("both");
    case Direction::HostToClient: return QStringLiteral("host-to-client");
    case Direction::ClientToHost: return QStringLiteral("client-to-host");
    case Direction::Off: return QStringLiteral("off");
    }
    return QStringLiteral("off");
}

inline const char *reasonName(Reason reason)
{
    switch (reason) {
    case Reason::Allowed: return "allowed";
    case Reason::Greeter: return "greeter";
    case Reason::ForeignLocked: return "foreign-locked";
    case Reason::ForeignUnlocked: return "foreign-unlocked";
    case Reason::Viewer: return "viewer";
    case Reason::NoSeat: return "no-seat";
    case Reason::UnknownUser: return "unknown-user";
    case Reason::Setting: return "setting";
    case Reason::LockedNewTransfer: return "locked-new-transfer";
    }
    return "unknown";
}

struct Input {
    Route route = Route::Console;
    ConsoleSeat::Adapter adapter = ConsoleSeat::Adapter::None;
    bool locked = false; ///< the session the worker is attached to is locked
    std::optional<quint32> clientUid; ///< the connection's PAM uid
    quint32 sessionUid = 0; ///< the uid of the session the worker is attached to
    bool ownsControl = false;
    Direction setting = Direction::Both;
    /// OPT-078: ClipboardFiles (same shape as ClipboardDirection).
    Direction filesSetting = Direction::Both;
    /// OPT-078: a file transfer in that direction is already running (it was allowed when it started).
    bool hostToClientFileStarted = false;
    bool clientToHostFileStarted = false;
    bool operator==(const Input &) const = default;
};

/// OPT-078: what file copy may do in this state (for the Input's "started" flags: a started transfer is asked "may it continue").
struct FilesPlan {
    bool hostToClient = false;
    bool clientToHost = false;
    Reason reason = Reason::NoSeat;
    bool operator==(const FilesPlan &) const = default;
};

struct Plan {
    FilesPlan files;
    bool hostToClient = false;
    bool clientToHost = false;
    Reason reason = Reason::NoSeat;
    bool greeter = false; ///< the worker is the login screen's
    bool locked = false;
    bool operator==(const Plan &) const = default;
};

inline Plan plan(const Input &input)
{
    Plan result;
    result.greeter = input.route == Route::Console && input.adapter == ConsoleSeat::Adapter::Greeter;
    result.locked = input.locked;
    bool hostToClient = false;
    bool clientToHost = false;
    Reason reason = Reason::Allowed;
    if (!input.clientUid || !*input.clientUid) {
        reason = Reason::UnknownUser;
    } else if (!input.ownsControl) {
        reason = Reason::Viewer;
    } else if (input.route == Route::Virtual) {
        // Always the PAM user that owns the desktop; the transport has checked the binding.
        if (input.adapter == ConsoleSeat::Adapter::VirtualUser && *input.clientUid == input.sessionUid) {
            hostToClient = clientToHost = true;
        } else {
            reason = Reason::NoSeat;
        }
    } else {
        switch (input.adapter) {
        case ConsoleSeat::Adapter::Greeter:
            clientToHost = true;
            reason = Reason::Greeter;
            break;
        case ConsoleSeat::Adapter::PhysicalUser:
            if (*input.clientUid == input.sessionUid) {
                hostToClient = clientToHost = true;
            } else {
                reason = input.locked ? Reason::ForeignLocked : Reason::ForeignUnlocked;
            }
            break;
        case ConsoleSeat::Adapter::None:
        case ConsoleSeat::Adapter::VirtualUser:
            reason = Reason::NoSeat;
            break;
        }
    }
    // Files: the state rule, separately for a new transfer and for one that continues.
    bool filesNewHost = false, filesNewClient = false, filesKeepHost = false, filesKeepClient = false;
    Reason filesReason = reason;
    if (reason == Reason::Allowed || reason == Reason::Greeter) {
        if (hostToClient && clientToHost) { // same account, controlling (physical user or virtual owner)
            filesNewHost = filesKeepHost = filesKeepClient = true;
            filesNewClient = !(input.route == Route::Console && input.locked);
            if (!filesNewClient && !input.clientToHostFileStarted) filesReason = Reason::LockedNewTransfer;
        } else if (reason == Reason::Greeter) {
            filesReason = Reason::Greeter; // a file on a login screen is never wanted
        }
    }
    const bool filesWantHost = (input.filesSetting == Direction::Both || input.filesSetting == Direction::HostToClient)
        && (input.setting == Direction::Both || input.setting == Direction::HostToClient);
    const bool filesWantClient = (input.filesSetting == Direction::Both || input.filesSetting == Direction::ClientToHost)
        && (input.setting == Direction::Both || input.setting == Direction::ClientToHost);
    const bool stateHost = input.hostToClientFileStarted ? filesKeepHost : filesNewHost;
    const bool stateClient = input.clientToHostFileStarted ? filesKeepClient : filesNewClient;
    result.files.hostToClient = stateHost && filesWantHost;
    result.files.clientToHost = stateClient && filesWantClient;
    result.files.reason = filesReason;
    if (filesReason == Reason::Allowed && ((stateHost && !filesWantHost) || (stateClient && !filesWantClient))) {
        result.files.reason = Reason::Setting;
    }
    const bool wantHost = input.setting == Direction::Both || input.setting == Direction::HostToClient;
    const bool wantClient = input.setting == Direction::Both || input.setting == Direction::ClientToHost;
    const bool narrowed = (hostToClient && !wantHost) || (clientToHost && !wantClient);
    result.hostToClient = hostToClient && wantHost;
    result.clientToHost = clientToHost && wantClient;
    result.reason = narrowed && reason == Reason::Allowed ? Reason::Setting : reason;
    return result;
}
}
