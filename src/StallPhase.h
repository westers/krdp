// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <atomic>

namespace KRdp
{

/**
 * K6.2 phase marker: what the main thread is doing, for the stall detector's "blocked" line.
 * Header-only on purpose, so code compiled into executables and tests can mark a phase without
 * linking the detector. The variable has default visibility so the executable and libKRdp share
 * one copy under -fvisibility=hidden.
 */
__attribute__((visibility("default"))) inline std::atomic<const char *> g_stallPhase{"event loop"};

/// Marks `phase` (a string literal) for the lifetime of the object and restores the previous one.
class StallPhase
{
public:
    explicit StallPhase(const char *phase)
        : m_previous(g_stallPhase.exchange(phase, std::memory_order_relaxed))
    {
    }
    ~StallPhase()
    {
        g_stallPhase.store(m_previous, std::memory_order_relaxed);
    }
    StallPhase(const StallPhase &) = delete;
    StallPhase &operator=(const StallPhase &) = delete;

private:
    const char *m_previous;
};

}
