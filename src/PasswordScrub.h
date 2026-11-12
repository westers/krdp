// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cstring>
#include <strings.h>

#include <freerdp/settings.h>

namespace KRdp
{

/**
 * GAP-G2: overwrite and drop the client's password that FreeRDP parsed into
 * \a settings (FreeRDP_Password, the Client Info PDU's password field).
 * Idempotent; a null \a settings or an unset password is fine.
 *
 * This reaches the one copy FreeRDP keeps in the settings. It does not reach
 * the bytes FreeRDP read off the wire: the TLS-decrypted PDU sits in a stream
 * of the transport's receive pool (transport.c transport_check_fds(), a
 * StreamPool that neither zeroes a returned stream nor zeroes it on free, see
 * StreamPool.c StreamPool_Take()/Stream_Release() and stream.c Stream_Free()),
 * and OpenSSL's record buffer behind it; neither is exposed by FreeRDP's
 * public API (rdpTransport and its pool are opaque).
 */
inline void scrubPeerPassword(rdpSettings *settings)
{
    if (!settings) {
        return;
    }
    if (auto *stored = const_cast<char *>(freerdp_settings_get_string(settings, FreeRDP_Password))) {
        ::explicit_bzero(stored, ::strlen(stored));
        freerdp_settings_set_string(settings, FreeRDP_Password, nullptr);
    }
}

}
