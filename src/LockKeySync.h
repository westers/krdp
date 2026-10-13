// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cstdint>
#include <set>
#include <vector>

#include <linux/input-event-codes.h>

namespace KRdp
{

/**
 * Keeps the host's Caps/Num/Scroll lock state in step with the client's
 * (TS_SYNC_EVENT, OPT-069).
 *
 * The compositor's lock state cannot be read through fake input, so this
 * tracks what the client reports. The first synchronize event only sets the
 * baseline (the host's real state is unknown and guessing would invert a lock
 * that is already right). After that, a lock key the client pressed is
 * followed, and a later synchronize event that differs from the tracked state
 * (a lock toggled while the client window had no focus) yields the evdev key
 * codes to tap on the host so both sides agree again.
 *
 * Pure (no Qt, no Wayland) so it is unit tested directly.
 */
class LockKeySync
{
public:
    // TS_SYNC_EVENT toggle flags (MS-RDPBCGR 2.2.8.1.1.3.1.1.5).
    static constexpr uint32_t ScrollLock = 0x1;
    static constexpr uint32_t NumLock = 0x2;
    static constexpr uint32_t CapsLock = 0x4;
    static constexpr uint32_t Known = ScrollLock | NumLock | CapsLock; // Kana (0x8) is ignored

    /** A synchronize event arrived. Returns evdev codes to tap, in order. */
    std::vector<uint32_t> synchronize(uint32_t flags)
    {
        flags &= Known;
        std::vector<uint32_t> toggles;
        if (m_known) {
            const uint32_t diff = flags ^ m_state;
            for (const auto &entry : kKeys) {
                if (diff & entry.flag) {
                    toggles.push_back(entry.code);
                }
            }
        }
        m_state = flags;
        m_known = true;
        return toggles;
    }

    /** A key event arrived; the lock keys flip the tracked state on a fresh press. */
    void key(uint32_t evdevCode, bool pressed)
    {
        for (const auto &entry : kKeys) {
            if (entry.code != evdevCode) {
                continue;
            }
            if (!pressed) {
                m_down.erase(entry.code);
            } else if (m_down.insert(entry.code).second && m_known) { // auto-repeat presses do not toggle
                m_state ^= entry.flag;
            }
        }
    }

    uint32_t state() const
    {
        return m_state;
    }
    bool baselineKnown() const
    {
        return m_known;
    }

private:
    struct Entry {
        uint32_t flag;
        uint32_t code;
    };
    static constexpr Entry kKeys[] = {{CapsLock, KEY_CAPSLOCK}, {NumLock, KEY_NUMLOCK}, {ScrollLock, KEY_SCROLLLOCK}};

    uint32_t m_state = 0;
    bool m_known = false;
    std::set<uint32_t> m_down;
};

}
