// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <QtGlobal>

namespace KRdp::Stats
{
/**
 * OPT-078 phase 0: the measurements the file-transfer acceptance (spec 6.4) needs and the stats path
 * lacked: a bounded histogram (frame delay at send, send-queue depth sampled at send) and the audio
 * flow counters (late deliveries, overflow drops, producer gaps). Header-only, lock-free and
 * pure: no clock of its own (callers pass time points), nothing here changes behaviour.
 */

/// The plain, copyable totals of one histogram (the stats snapshot carries these; samples send the difference).
template<typename Traits>
struct HistogramCounts {
    static constexpr size_t Bounds = Traits::Bounds.size();
    std::array<quint64, Bounds + 1> buckets{}; ///< bucket i: value <= Bounds[i]; the last: above the largest bound
    quint64 count = 0;
    quint64 sum = 0;
    quint64 max = 0; ///< largest value seen since the start (not a delta)

    /// This minus \a previous (counts only; a total that went down restarted and counts from zero). max stays the running maximum.
    HistogramCounts since(const HistogramCounts &previous) const
    {
        if (count < previous.count) {
            return *this;
        }
        HistogramCounts result;
        for (size_t i = 0; i < buckets.size(); ++i) {
            result.buckets[i] = buckets[i] >= previous.buckets[i] ? buckets[i] - previous.buckets[i] : buckets[i];
        }
        result.count = count - previous.count;
        result.sum = sum >= previous.sum ? sum - previous.sum : sum;
        result.max = max;
        return result;
    }
    /// The upper bound of the bucket holding the \a fraction (0..1) quantile, capped at the running maximum; 0 without samples.
    quint64 quantile(double fraction) const
    {
        if (count == 0) {
            return 0;
        }
        const quint64 rank = std::max<quint64>(1, quint64(std::ceil(fraction * double(count))));
        quint64 seen = 0;
        for (size_t i = 0; i < buckets.size(); ++i) {
            seen += buckets[i];
            if (seen >= rank) {
                return i < Bounds ? std::min<quint64>(Traits::Bounds[i], max) : max;
            }
        }
        return max;
    }
    bool operator==(const HistogramCounts &) const = default;
};

/// The recorder: relaxed atomics, any thread.
template<typename Traits>
class Histogram
{
public:
    using Counts = HistogramCounts<Traits>;
    void record(quint64 value)
    {
        size_t i = 0;
        while (i < Counts::Bounds && value > Traits::Bounds[i]) {
            ++i;
        }
        m_buckets[i].fetch_add(1, std::memory_order_relaxed);
        m_count.fetch_add(1, std::memory_order_relaxed);
        m_sum.fetch_add(value, std::memory_order_relaxed);
        quint64 seen = m_max.load(std::memory_order_relaxed);
        while (value > seen && !m_max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
        }
    }
    Counts counts() const
    {
        Counts result;
        for (size_t i = 0; i < m_buckets.size(); ++i) {
            result.buckets[i] = m_buckets[i].load(std::memory_order_relaxed);
        }
        result.count = m_count.load(std::memory_order_relaxed);
        result.sum = m_sum.load(std::memory_order_relaxed);
        result.max = m_max.load(std::memory_order_relaxed);
        return result;
    }

private:
    std::array<std::atomic<quint64>, Counts::Bounds + 1> m_buckets{};
    std::atomic<quint64> m_count = 0;
    std::atomic<quint64> m_sum = 0;
    std::atomic<quint64> m_max = 0;
};

/// Milliseconds a frame waited between queueFrame() and the RDPGFX send.
struct FrameDelayTraits {
    static constexpr std::array<quint64, 11> Bounds{1, 2, 5, 10, 20, 40, 80, 160, 320, 640, 1280};
};
/// Bytes the socket still held (SIOCOUTQ) when a frame was handed to it.
struct SendQueueTraits {
    static constexpr std::array<quint64, 9> Bounds{0, 4096, 16384, 65536, 131072, 262144, 524288, 1048576, 4194304};
};
using FrameDelay = Histogram<FrameDelayTraits>;
using FrameDelayCounts = FrameDelay::Counts;
using SendQueue = Histogram<SendQueueTraits>;
using SendQueueCounts = SendQueue::Counts;

/// Whole milliseconds, rounded up, of a (non-negative) duration.
inline quint64 ceilMilliseconds(std::chrono::nanoseconds duration)
{
    const auto ns = duration.count();
    return ns <= 0 ? 0 : quint64((ns + 999'999) / 1'000'000);
}

/// One audio path's running totals (PCM 44.1 kHz stereo 16-bit, 20 ms packets).
struct AudioFlow {
    quint64 packets = 0; ///< packets handed to RDPSND
    quint64 late = 0; ///< packets taken while more than LateBacklogMs of audio was already waiting
    quint64 overflowEvents = 0; ///< the bounded queue dropped its oldest audio (the session loop fell behind)
    quint64 overflowBytes = 0;
    quint64 producerGaps = 0; ///< capture delivered nothing for more than ProducerGapMs (PipeWire path only)
    quint32 maxGapMs = 0; ///< longest producer gap since the start
    quint32 maxBacklogMs = 0; ///< deepest backlog seen at a take, since the start
    bool operator==(const AudioFlow &) const = default;
    AudioFlow &operator+=(const AudioFlow &other)
    {
        packets += other.packets;
        late += other.late;
        overflowEvents += other.overflowEvents;
        overflowBytes += other.overflowBytes;
        producerGaps += other.producerGaps;
        maxGapMs = std::max(maxGapMs, other.maxGapMs);
        maxBacklogMs = std::max(maxBacklogMs, other.maxBacklogMs);
        return *this;
    }
};

class AudioFlowCounters
{
public:
    static constexpr quint32 BytesPerSecond = 44100 * 4;
    static constexpr quint32 LateBacklogMs = 60; ///< three 20 ms packets
    static constexpr quint32 ProducerGapMs = 100; ///< the acceptance criterion's "no gap > 100 ms"
    using Clock = std::chrono::steady_clock;

    static quint32 bytesToMs(quint64 bytes)
    {
        return quint32(std::min<quint64>(bytes * 1000 / BytesPerSecond, 0xffffffffu));
    }

    /// The capture side delivered audio at \a now (PipeWire's process callback).
    void noteProduced(Clock::time_point now)
    {
        const qint64 ns = now.time_since_epoch().count();
        const qint64 last = m_lastProducedNs.exchange(ns, std::memory_order_relaxed);
        if (last != 0 && ns > last) {
            const quint32 gapMs = quint32(std::min<qint64>((ns - last) / 1'000'000, 0xffffffff));
            if (gapMs > ProducerGapMs) {
                m_producerGaps.fetch_add(1, std::memory_order_relaxed);
            }
            raise(m_maxGapMs, gapMs);
        }
    }
    /// The bounded queue dropped \a bytes of its oldest audio.
    void noteOverflow(quint64 bytes)
    {
        m_overflowEvents.fetch_add(1, std::memory_order_relaxed);
        m_overflowBytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    /// A packet was taken while \a backlogBytes (including that packet) were waiting.
    void noteDelivered(quint64 backlogBytes)
    {
        m_packets.fetch_add(1, std::memory_order_relaxed);
        const quint32 backlogMs = bytesToMs(backlogBytes);
        if (backlogMs > LateBacklogMs) {
            m_late.fetch_add(1, std::memory_order_relaxed);
        }
        raise(m_maxBacklogMs, backlogMs);
    }
    AudioFlow flow() const
    {
        AudioFlow result;
        result.packets = m_packets.load(std::memory_order_relaxed);
        result.late = m_late.load(std::memory_order_relaxed);
        result.overflowEvents = m_overflowEvents.load(std::memory_order_relaxed);
        result.overflowBytes = m_overflowBytes.load(std::memory_order_relaxed);
        result.producerGaps = m_producerGaps.load(std::memory_order_relaxed);
        result.maxGapMs = m_maxGapMs.load(std::memory_order_relaxed);
        result.maxBacklogMs = m_maxBacklogMs.load(std::memory_order_relaxed);
        return result;
    }

private:
    static void raise(std::atomic<quint32> &target, quint32 value)
    {
        quint32 seen = target.load(std::memory_order_relaxed);
        while (value > seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
        }
    }
    std::atomic<quint64> m_packets = 0;
    std::atomic<quint64> m_late = 0;
    std::atomic<quint64> m_overflowEvents = 0;
    std::atomic<quint64> m_overflowBytes = 0;
    std::atomic<quint64> m_producerGaps = 0;
    std::atomic<quint32> m_maxGapMs = 0;
    std::atomic<quint32> m_maxBacklogMs = 0;
    std::atomic<qint64> m_lastProducedNs = 0;
};
}
