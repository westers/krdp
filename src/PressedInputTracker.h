// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <utility>

namespace KRdp
{

/**
 * The keys and pointer buttons a session has injected as pressed and not yet
 * released (AUD-P2).
 *
 * It mirrors what was sent to the compositor, not what the client holds: a
 * key the session pressed through fake input stays down in KWin until the
 * session sends its release, so a client that drops while holding Ctrl, or a
 * session that is torn down by a rebuild, would otherwise leave it stuck.
 * releaseAll() sends a release for everything still down, and the destructor
 * does the same, so an owner that declares this after the input object it
 * releases through gets the releases on destruction.
 *
 * Pure (no Wayland, no Qt GUI) so it is unit tested with a fake sink.
 */
class PressedInputTracker
{
public:
    enum class Kind {
        Key,
        Button,
    };
    using ReleaseSink = std::function<void(Kind kind, uint32_t code)>;

    explicit PressedInputTracker(ReleaseSink sink)
        : m_sink(std::move(sink))
    {
    }
    ~PressedInputTracker()
    {
        releaseAll();
    }
    PressedInputTracker(const PressedInputTracker &) = delete;
    PressedInputTracker &operator=(const PressedInputTracker &) = delete;

    /** Record an evdev key code sent as pressed (true) or released (false). */
    void key(uint32_t code, bool pressed)
    {
        record(m_keys, code, pressed);
    }
    /** Record an evdev button code (BTN_*) sent as pressed or released. */
    void button(uint32_t code, bool pressed)
    {
        record(m_buttons, code, pressed);
    }

    bool keyDown(uint32_t code) const
    {
        return m_keys.contains(code);
    }
    bool buttonDown(uint32_t code) const
    {
        return m_buttons.contains(code);
    }
    bool empty() const
    {
        return m_keys.empty() && m_buttons.empty();
    }

    /**
     * Send a release for every key and button still down, buttons first (a
     * held modifier must not turn a button release into a modified click),
     * and forget them. Sending is idempotent: a second call sends nothing.
     */
    void releaseAll()
    {
        auto buttons = std::exchange(m_buttons, {});
        auto keys = std::exchange(m_keys, {});
        if (!m_sink) {
            return;
        }
        for (const auto code : buttons) {
            m_sink(Kind::Button, code);
        }
        for (const auto code : keys) {
            m_sink(Kind::Key, code);
        }
    }

private:
    static void record(std::set<uint32_t> &set, uint32_t code, bool pressed)
    {
        if (pressed) {
            set.insert(code);
        } else {
            set.erase(code);
        }
    }

    ReleaseSink m_sink;
    std::set<uint32_t> m_keys;
    std::set<uint32_t> m_buttons;
};

} // namespace KRdp
