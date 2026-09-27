// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <mutex>
#include <pipewire/pipewire.h>

namespace KRdp
{
/**
 * Process-wide, reference-counted pw_init()/pw_deinit() for KRdp's own
 * PipeWire endpoints (microphone, camera, playback capture).
 *
 * Each endpoint holds one Reference from just before it creates its
 * pw_thread_loop until just after it has destroyed it. The first reference
 * calls pw_init(), the last one pw_deinit(). libpipewire refcounts its own
 * initialisation too (checked against 1.6: two pw_init() and one pw_deinit()
 * leave it usable), so other users in the same process, such as KPipeWire's
 * call_once pw_init(), keep working after KRdp drops its last reference.
 *
 * Header-only: the endpoints are also compiled straight into the console
 * worker, probes and tests, so each binary gets its own balanced counter.
 */
class PipeWireRuntime
{
public:
    class Reference
    {
    public:
        Reference() = default;
        ~Reference()
        {
            release();
        }
        Reference(const Reference &) = delete;
        Reference &operator=(const Reference &) = delete;

        /** Take the reference if not already held. Idempotent. */
        void acquire()
        {
            if (!m_held) {
                PipeWireRuntime::acquire();
                m_held = true;
            }
        }
        /** Drop the reference if held. Call only after every pw_* object of the owner is destroyed. */
        void release()
        {
            if (m_held) {
                m_held = false;
                PipeWireRuntime::release();
            }
        }
        bool held() const
        {
            return m_held;
        }

    private:
        bool m_held = false;
    };

    /** Number of outstanding references in this binary (for tests and diagnostics). */
    static int references()
    {
        std::lock_guard lock(state().mutex);
        return state().references;
    }

private:
    struct State {
        std::mutex mutex;
        int references = 0;
    };
    static State &state()
    {
        static State instance;
        return instance;
    }
    static void acquire()
    {
        std::lock_guard lock(state().mutex);
        if (state().references++ == 0) {
            pw_init(nullptr, nullptr);
        }
    }
    static void release()
    {
        std::lock_guard lock(state().mutex);
        if (state().references > 0 && --state().references == 0) {
            pw_deinit();
        }
    }
};
}
