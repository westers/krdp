// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <optional>

#include "ConsoleHandoff.h"

namespace KRdp::ConsoleAdmission
{
/**
 * Which PAM-authenticated RDP user may see and drive the physical console
 * (AUD-C-1). The console host accepts any PAM user, so the desktop owner must
 * be checked here: an unlocked desktop belongs to its logind uid alone. The
 * SDDM greeter and a locked desktop show only a login/lock screen, which any
 * authenticated user may use (to log in, or to switch user). Re-evaluated on
 * every seat change: unlocking, or the greeter handing over to a desktop,
 * removes clients of other accounts.
 *
 * No uid (a configured non-PAM account, or not authenticated) never admits.
 */
inline bool allowed(const ConsoleHandoff::Target &seat, bool locked, std::optional<quint32> clientUid)
{
    if (!clientUid) {
        return false;
    }
    switch (seat.adapter) {
    case ConsoleSeat::Adapter::Greeter:
        return true;
    case ConsoleSeat::Adapter::PhysicalUser:
        return *clientUid == seat.uid || locked;
    case ConsoleSeat::Adapter::None:
        // Nothing is on the seat to see or drive; the next seat change
        // re-evaluates before any worker can become ready.
        return true;
    case ConsoleSeat::Adapter::VirtualUser:
        return false;
    }
    return false;
}
}
