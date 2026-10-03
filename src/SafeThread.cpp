// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// Built with -fexceptions (see src/CMakeLists.txt); the rest of the tree is not.

#include "SafeThread.h"

namespace KRdp
{

bool startJThread(std::jthread &thread, std::function<void(std::stop_token)> body) noexcept
{
    try {
        thread = std::jthread(std::move(body));
        return true;
    } catch (...) {
        // std::system_error from pthread_create (EAGAIN: thread/process limit), or bad_alloc.
        return false;
    }
}

}
