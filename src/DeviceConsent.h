// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <cstdint>
#include <mutex>

namespace KRdp
{
/**
 * One device's consent (playback, microphone or camera), serialized with
 * delivery. Each consent period has its own generation: a device context
 * (AUDIN, an RDPECAM device, a playback capture) is bound to the generation it
 * was created for, and only that generation's data is ever delivered. A
 * disable/re-enable cycle, or a renew(), cannot make an old context eligible
 * again, even if the session loop missed the brief disabled interval.
 * Sink callbacks must not reenter this object.
 */
class DeviceConsent
{
public:
    struct Snapshot {
        uint64_t generation = 0;
        bool enabled = false;
    };
    /** Idempotent: a new generation only when the enabled state changes. */
    void setEnabled(bool enabled)
    {
        std::lock_guard lock(m_mutex);
        if (m_state.enabled != enabled) {
            ++m_state.generation;
            m_state.enabled = enabled;
        }
    }
    /**
     * Enable with a fresh generation even when already enabled (a KRDPCTL
     * `device` `on` or `reselect`): the session loop retires the current
     * context and creates a new one. Returns the new generation.
     */
    uint64_t renew()
    {
        std::lock_guard lock(m_mutex);
        ++m_state.generation;
        m_state.enabled = true;
        return m_state.generation;
    }
    Snapshot snapshot() const
    {
        std::lock_guard lock(m_mutex);
        return m_state;
    }
    template<typename Sink> bool deliver(uint64_t generation, Sink &&sink)
    {
        std::lock_guard lock(m_mutex);
        if (!m_state.enabled || !generation || generation != m_state.generation) return false;
        sink();
        return true;
    }
private:
    mutable std::mutex m_mutex;
    Snapshot m_state;
};
}
