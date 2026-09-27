// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cstdint>

namespace KRdp
{

/**
 * Version gating for Wayland requests (AUD-FIX F1).
 *
 * A request marked `since="N"` in a protocol XML only exists on objects bound
 * at version N or later. Sending it on an older bind is a protocol error, and
 * the compositor then disconnects the whole client: on Sol, sending
 * org_kde_kwin_fake_input.destroy (since 5) on a v4 bind made KWin raise
 * "invalid method 11 (since 4 < 5)" and krdpserver exited 255 on every
 * disconnect.
 *
 * Callers pass the generated `<IFACE>_<REQUEST>_SINCE_VERSION` constant, never
 * a hand-typed number, so the check follows the XML the build used.
 *
 * Pure (no Wayland, no Qt) so it is unit tested directly.
 */
namespace WaylandRequestVersion
{

/// True when a request introduced in @p sinceVersion may be sent on an object bound at @p boundVersion.
constexpr bool supports(uint32_t boundVersion, uint32_t sinceVersion)
{
    return boundVersion != 0 && boundVersion >= sinceVersion;
}

/// How to let go of a proxy whose interface has a destructor request.
enum class Teardown {
    /// Nothing is bound: nothing to do.
    None,
    /// Send the destructor request (it also frees the proxy).
    SendDestructor,
    /// The bound version predates the destructor: free the client-side proxy only, sending nothing.
    DropProxy,
};

constexpr Teardown teardownFor(uint32_t boundVersion, uint32_t destructorSinceVersion)
{
    if (boundVersion == 0) {
        return Teardown::None;
    }
    return supports(boundVersion, destructorSinceVersion) ? Teardown::SendDestructor : Teardown::DropProxy;
}

}
}
