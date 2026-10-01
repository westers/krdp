// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "VideoCodecSupport.h"
#include <atomic>
#include <mutex>
#include <optional>

namespace KRdp
{
/** Serialize the peer's standard caps with authenticated main-thread policy.
 * Encoder changes select a supported AVC format without inventing client caps.
 * The optional returned codec requests a session refresh; readers use snapshots
 * rather than the value of a possibly queued notification. */
class AvcCodecSelection
{
public:
    std::optional<VideoCodec> setPreference(CodecPreference preference)
    {
        std::lock_guard lock(m_mutex);
        m_preference = preference;
        return publish();
    }
    CodecPreference preference() const
    {
        std::lock_guard lock(m_mutex);
        return m_preference;
    }
    std::optional<VideoCodec> setAvc444Available(bool available)
    {
        std::lock_guard lock(m_mutex);
        m_avc444Available = available;
        return publish();
    }
    std::optional<VideoCodec> acceptCaps(uint32_t version, uint32_t flags)
    {
        std::lock_guard lock(m_mutex);
        m_clientAvc = VideoCodecSupport::codecFor(version, flags, CodecPreference::Auto);
        return publish();
    }
    std::optional<VideoCodec> setPrivateCodec(std::optional<VideoCodec> codec)
    {
        std::lock_guard lock(m_mutex);
        m_privateCodec = codec;
        return publish();
    }
    std::optional<VideoCodec> negotiated() const
    {
        const int codec = m_snapshot.load();
        return codec < 0 ? std::nullopt : std::optional(VideoCodec(codec));
    }
    VideoCodec forSessions() const
    {
        return selected(m_snapshot.load());
    }

private:
    // One atomic snapshot includes the pre-caps expectation. Readers must not
    // combine an old undecided flag with a new unsupported expectation.
    static constexpr int waiting(VideoCodec codec) { return -int(codec) - 2; }
    static constexpr VideoCodec selected(int snapshot)
    {
        return VideoCodec(snapshot < 0 ? -snapshot - 2 : snapshot);
    }
    // Caller holds m_mutex; return notifications after releasing it.
    std::optional<VideoCodec> publish()
    {
        const int before = m_snapshot.load();
        const bool use444 = m_avc444Available && m_preference != CodecPreference::Avc420;
        const auto expected = use444 ? VideoCodec::Avc444v2 : VideoCodec::Avc420;
        const auto standard = m_clientAvc ? std::optional(use444 ? *m_clientAvc : VideoCodec::Avc420) : std::nullopt;
        const auto chosen = m_privateCodec ? m_privateCodec : standard;
        const int next = chosen ? int(*chosen) : waiting(expected);
        m_snapshot.store(next);
        return before != next ? std::optional(selected(next)) : std::nullopt;
    }

    mutable std::mutex m_mutex;
    CodecPreference m_preference = CodecPreference::Auto;
    bool m_avc444Available = true; // Legacy in-process sessions keep their existing default.
    std::optional<VideoCodec> m_clientAvc;
    std::optional<VideoCodec> m_privateCodec;
    std::atomic<int> m_snapshot{waiting(VideoCodec::Avc444v2)};
};
}
