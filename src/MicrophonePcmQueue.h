// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QByteArray>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <deque>
#include <mutex>

namespace KRdp
{
// 48kHz stereo S16LE. Packet timestamps prevent an idle/stalled broker from
// replaying old speech; consent generations prevent replay into a new owner.
class MicrophonePcmQueue
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr qsizetype PacketBytes = 3840; // 20ms
    static constexpr qsizetype Capacity = PacketBytes * 10; // 200ms
    void reset(uint64_t generation)
    {
        std::lock_guard lock(m_mutex);
        if (generation <= m_generation) return;
        m_generation = generation;
        m_chunks.clear();
        m_size = 0;
    }
    bool write(uint64_t generation, const QByteArray &pcm, Clock::time_point now = Clock::now())
    {
        std::lock_guard lock(m_mutex);
        if (!generation || generation != m_generation || pcm.isEmpty() || pcm.size() % 4 || pcm.size() > 192000) return false;
        m_chunks.push_back({pcm.right(Capacity), now});
        m_size += m_chunks.back().bytes.size();
        while (m_size > Capacity) {
            const qsizetype count = std::min(m_size - Capacity, m_chunks.front().bytes.size());
            m_chunks.front().bytes.remove(0, count);
            m_size -= count;
            if (m_chunks.front().bytes.isEmpty()) m_chunks.pop_front();
        }
        return true;
    }
    QByteArray take(uint64_t generation, Clock::time_point now = Clock::now())
    {
        std::lock_guard lock(m_mutex);
        if (!generation || generation != m_generation) return {};
        QByteArray result;
        while (!m_chunks.empty() && result.size() < PacketBytes) {
            auto &chunk = m_chunks.front();
            if (now - chunk.when > std::chrono::milliseconds(250)) {
                m_size -= chunk.bytes.size();
                m_chunks.pop_front();
                continue;
            }
            const qsizetype count = std::min(PacketBytes - result.size(), chunk.bytes.size());
            result.append(chunk.bytes.constData(), count);
            chunk.bytes.remove(0, count);
            m_size -= count;
            if (chunk.bytes.isEmpty()) m_chunks.pop_front();
        }
        return result;
    }
private:
    struct Chunk { QByteArray bytes; Clock::time_point when; };
    std::mutex m_mutex;
    uint64_t m_generation = 0;
    qsizetype m_size = 0;
    std::deque<Chunk> m_chunks;
};
}
