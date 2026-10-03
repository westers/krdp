// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "krdp_export.h"

namespace KRdp
{

/**
 * K6.4: install a std::terminate handler that writes
 * "farside: <program>: std::terminate: <type>: <what>" to fd 2 and calls abort() (never _exit:
 * the core must survive). Compiled with exceptions so it can rethrow the active exception to
 * read it; the rest of the tree is -fno-exceptions.
 */
KRDP_EXPORT void installTerminateHandler(const char *program);

}
