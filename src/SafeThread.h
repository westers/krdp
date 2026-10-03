// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "krdp_export.h"

#include <functional>
#include <stop_token>
#include <thread>

namespace KRdp
{

/**
 * K6.5: start a std::jthread without letting a failed pthread_create (EAGAIN, ENOMEM) escape as
 * std::system_error. The tree is -fno-exceptions, so such an exception cannot be caught where
 * the thread is started and would reach std::terminate; this helper lives in an exception-
 * enabled translation unit and reports failure as `false`. On failure `thread` is left as it was.
 */
KRDP_EXPORT bool startJThread(std::jthread &thread, std::function<void(std::stop_token)> body) noexcept;

}
