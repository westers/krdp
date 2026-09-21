// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <cstdint>
#include <mutex>

namespace KRdp
{
// Serializes consent changes with delivery. A disable/re-enable cycle cannot
// make an old AUDIN context eligible again, even if the session loop missed
// the brief disabled interval. Sink callbacks must not reenter this object.
class MicrophoneConsent
{
public:
    struct Snapshot {
        uint64_t generation = 0;
        bool enabled = false;
    };
    void setEnabled(bool enabled)
    {
        std::lock_guard lock(m_mutex);
        if (m_state.enabled != enabled) {
            ++m_state.generation;
            m_state.enabled = enabled;
        }
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
