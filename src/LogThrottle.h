// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// OPT-055 K5.1: rate limiting for log sites that can repeat ("log state changes, not repetitions").
// Header-only, lock-free, clock injectable. Instances are members of the owning object, never static,
// so two sessions never hide each other's first message and a new encoder starts with a fresh budget.
// The classes and the macro are the same logic as KPipeWire's src/logthrottle_p.h (Farside robustness
// plan K1.0); scripts/check-logthrottle-copies.sh compares the two, so edit both together.

#pragma once

#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace KRdp
{

class LogThrottle
{
public:
    using Clock = std::chrono::steady_clock;

    /// At most @p burst lines per @p interval; the rest are counted.
    explicit LogThrottle(std::chrono::milliseconds interval = std::chrono::seconds(10), int burst = 1)
        : m_intervalNs(std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count())
        , m_burst(burst < 1 ? 1 : burst)
    {
    }

    /**
     * True: log now. *suppressed receives the number of calls swallowed since the previous line that
     * was allowed. Thread-safe and lock-free; the clock is injectable for tests.
     */
    bool allow(Clock::time_point now, quint64 *suppressed = nullptr)
    {
        const int64_t n = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        int64_t start = m_windowStart.load(std::memory_order_acquire);
        bool allowed = false;
        if (start == kUnset || n - start >= m_intervalNs) {
            // First call of a new window. Exactly one racing thread wins the window.
            if (m_windowStart.compare_exchange_strong(start, n, std::memory_order_acq_rel)) {
                m_inWindow.store(1, std::memory_order_release);
                allowed = true;
            }
        }
        if (!allowed && m_inWindow.fetch_add(1, std::memory_order_acq_rel) < m_burst) {
            allowed = true;
        }
        if (!allowed) {
            m_suppressed.fetch_add(1, std::memory_order_relaxed);
            m_suppressedTotal.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const quint64 swallowed = m_suppressed.exchange(0, std::memory_order_acq_rel);
        if (suppressed) {
            *suppressed = swallowed;
        }
        return true;
    }
    bool allow(quint64 *suppressed = nullptr)
    {
        return allow(Clock::now(), suppressed);
    }

    /// Every call swallowed since construction or reset(); for a session-close summary.
    quint64 suppressedTotal() const
    {
        return m_suppressedTotal.load(std::memory_order_relaxed);
    }

    /// Forget the window and the pending count, e.g. after a state change that makes the next line news.
    void reset()
    {
        m_windowStart.store(kUnset, std::memory_order_release);
        m_inWindow.store(0, std::memory_order_release);
        m_suppressed.store(0, std::memory_order_release);
    }

private:
    static constexpr int64_t kUnset = INT64_MIN;
    const int64_t m_intervalNs;
    const int m_burst;
    std::atomic<int64_t> m_windowStart{kUnset};
    std::atomic<int> m_inWindow{0};
    std::atomic<quint64> m_suppressed{0};
    std::atomic<quint64> m_suppressedTotal{0};
};

/// "Log state changes, not repetitions": the first failure of a run is reported, every further call
/// is only counted, and leaving the state reports how long it lasted and how often it repeated.
class LogEdge
{
public:
    using Clock = std::chrono::steady_clock;

    /// True only on the first call of a run. Every call is counted.
    bool enter(Clock::time_point now = Clock::now())
    {
        m_count.fetch_add(1, std::memory_order_relaxed);
        if (m_in.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        m_startNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(), std::memory_order_release);
        return true;
    }

    /// True only if the state was entered; *count and *duration describe the finished run.
    bool leave(quint64 *count, std::chrono::milliseconds *duration, Clock::time_point now = Clock::now())
    {
        if (!m_in.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }
        const quint64 total = m_count.exchange(0, std::memory_order_acq_rel);
        const int64_t startNs = m_startNs.load(std::memory_order_acquire);
        const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        if (count) {
            *count = total;
        }
        if (duration) {
            *duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(nowNs - startNs));
        }
        return true;
    }

    bool active() const
    {
        return m_in.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool m_in{false};
    std::atomic<quint64> m_count{0};
    std::atomic<int64_t> m_startNs{0};
};

}

// Logs through @p stream at most `burst` times per interval and appends "(N similar suppressed)".
// e.g. FARSIDE_LOG_THROTTLED(m_pullLog, qCWarning(PIPEWIRERECORD_LOGGING).noquote() << "Failed:" << err);
#define FARSIDE_LOG_THROTTLED(throttle, stream)                                                                                                                \
    do {                                                                                                                                                       \
        quint64 farsideSuppressed_ = 0;                                                                                                                        \
        if ((throttle).allow(&farsideSuppressed_)) {                                                                                                           \
            stream << (farsideSuppressed_ ? QStringLiteral("(%1 similar suppressed)").arg(farsideSuppressed_) : QString());                                    \
        }                                                                                                                                                      \
    } while (0)
