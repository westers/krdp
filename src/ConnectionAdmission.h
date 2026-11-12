// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// REVIEW-S1: admission policy for incoming RDP connections. Pure (no sockets, no
// clock: time is an argument), so it is unit-tested without a server. Not
// thread-safe; Server guards its instance with a mutex.
//
// Two defences against a client that has not proven who it is:
//  1. Caps on connections that have not authenticated yet, in total and per
//     source address. Each costs a thread, a TLS handshake and a certificate
//     re-read for up to the handshake timeout. Authenticated sessions never
//     count.
//  2. A per-source failure backoff. After `failuresBeforeBackoff` failed logins
//     from one source within `failureWindow`, new connections from it are
//     refused for a delay that starts at `backoffStart` and doubles with every
//     further failure up to `backoffMax`. A successful login clears the source.
//
// Loopback is deliberately not exempt: a local process can guess passwords too.

#pragma once

#include <QHostAddress>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>

namespace KRdp
{

class ConnectionAdmission
{
public:
    using Clock = std::chrono::steady_clock;

    struct Limits {
        int maxUnauthenticated = 16; ///< all sources together; <= 0 disables the cap
        int maxUnauthenticatedPerSource = 4; ///< <= 0 disables the cap
        int failuresBeforeBackoff = 3; ///< failed logins within failureWindow that start the backoff; <= 0 disables it
        std::chrono::seconds failureWindow{60};
        std::chrono::seconds backoffStart{2};
        std::chrono::seconds backoffMax{60};
        std::size_t maxSources = 1024; ///< size of the failure table; the least recently failed source is dropped
        std::chrono::seconds refusalLogInterval{10};
    };

    enum class Verdict {
        Admit,
        RefuseTotalCap,
        RefuseSourceCap,
        RefuseBackoff,
    };

    ConnectionAdmission() = default;
    explicit ConnectionAdmission(const Limits &limits)
        : m_limits(limits)
    {
    }

    const Limits &limits() const
    {
        return m_limits;
    }
    void setLimits(const Limits &limits)
    {
        m_limits = limits;
    }

    /**
     * The table key of a peer address: IPv4-mapped IPv6 addresses count as the
     * IPv4 address, other IPv6 addresses as their /64 (one subscriber usually
     * owns a whole /64, so rotating inside it must not dodge the limits), and
     * the scope id is dropped.
     */
    static QString sourceKey(const QHostAddress &address)
    {
        if (address.isNull()) {
            return QStringLiteral("unknown");
        }
        bool ok = false;
        const quint32 v4 = address.toIPv4Address(&ok);
        if (ok) {
            return QHostAddress(v4).toString();
        }
        const Q_IPV6ADDR v6 = address.toIPv6Address();
        Q_IPV6ADDR prefix{};
        for (int i = 0; i < 8; ++i) {
            prefix[i] = v6[i];
        }
        return QHostAddress(prefix).toString() + QStringLiteral("/64");
    }

    /**
     * A connection from @p source arrived. On Admit it is counted as
     * unauthenticated under @p id until release(id) (authenticated or closed).
     * A refused connection is not counted and needs no release().
     */
    Verdict admit(quint64 id, const QString &source, Clock::time_point now)
    {
        if (const auto it = m_failures.find(source); it != m_failures.end() && now < it->second.blockedUntil) {
            return Verdict::RefuseBackoff;
        }
        if (m_limits.maxUnauthenticated > 0 && int(m_pending.size()) >= m_limits.maxUnauthenticated) {
            return Verdict::RefuseTotalCap;
        }
        if (m_limits.maxUnauthenticatedPerSource > 0 && pendingFrom(source) >= m_limits.maxUnauthenticatedPerSource) {
            return Verdict::RefuseSourceCap;
        }
        m_pending.emplace(id, source);
        return Verdict::Admit;
    }

    /** The connection @p id authenticated or closed: it no longer counts. Idempotent. */
    void release(quint64 id)
    {
        m_pending.erase(id);
    }

    /** The unauthenticated connections currently counted. */
    int pending() const
    {
        return int(m_pending.size());
    }
    int pendingFrom(const QString &source) const
    {
        int n = 0;
        for (const auto &entry : m_pending) {
            n += entry.second == source;
        }
        return n;
    }
    /** The source an admitted, still unreleased connection came from. */
    std::optional<QString> sourceOf(quint64 id) const
    {
        const auto it = m_pending.find(id);
        return it == m_pending.end() ? std::nullopt : std::optional<QString>(it->second);
    }

    /** A login from @p source failed. Returns how long new connections are now refused (0 when not at all). */
    std::chrono::seconds recordFailure(const QString &source, Clock::time_point now)
    {
        if (m_limits.failuresBeforeBackoff <= 0) {
            return std::chrono::seconds(0);
        }
        auto it = m_failures.find(source);
        if (it == m_failures.end()) {
            if (m_failures.size() >= std::max<std::size_t>(m_limits.maxSources, 1)) {
                evictOldest();
            }
            it = m_failures.emplace(source, Failures{}).first;
        }
        Failures &f = it->second;
        while (!f.times.empty() && now - f.times.front() > m_limits.failureWindow) {
            f.times.pop_front();
        }
        if (f.times.empty()) {
            f.delay = std::chrono::seconds(0); // a quiet window forgives the doubling
        }
        f.times.push_back(now);
        // More than failuresBeforeBackoff entries are never needed.
        while (int(f.times.size()) > m_limits.failuresBeforeBackoff) {
            f.times.pop_front();
        }
        if (int(f.times.size()) >= m_limits.failuresBeforeBackoff) {
            f.delay = f.delay.count() == 0 ? m_limits.backoffStart : std::min(f.delay * 2, m_limits.backoffMax);
            f.blockedUntil = now + f.delay;
            return f.delay;
        }
        return std::chrono::seconds(0);
    }

    /** A login from @p source succeeded: its failures are forgotten. */
    void recordSuccess(const QString &source)
    {
        m_failures.erase(source);
    }

    /** Whether @p source is refused at @p now because of earlier failures. */
    bool blocked(const QString &source, Clock::time_point now) const
    {
        const auto it = m_failures.find(source);
        return it != m_failures.end() && now < it->second.blockedUntil;
    }
    std::size_t trackedSources() const
    {
        return m_failures.size();
    }

    /**
     * Rate limit for the "connection refused" log line. Returns the number of
     * refusals since the last logged one (including this one) when a line may be
     * written now, otherwise nothing.
     */
    std::optional<int> takeRefusalLog(Clock::time_point now)
    {
        ++m_suppressedRefusals;
        if (m_haveRefusalLog && now - m_lastRefusalLog < m_limits.refusalLogInterval) {
            return std::nullopt;
        }
        m_haveRefusalLog = true;
        m_lastRefusalLog = now;
        return std::exchange(m_suppressedRefusals, 0);
    }

private:
    struct Failures {
        std::deque<Clock::time_point> times;
        std::chrono::seconds delay{0};
        Clock::time_point blockedUntil{};
        Clock::time_point lastFailure() const
        {
            return times.empty() ? Clock::time_point{} : times.back();
        }
    };

    void evictOldest()
    {
        auto oldest = m_failures.begin();
        for (auto it = m_failures.begin(); it != m_failures.end(); ++it) {
            if (it->second.lastFailure() < oldest->second.lastFailure()) {
                oldest = it;
            }
        }
        if (oldest != m_failures.end()) {
            m_failures.erase(oldest);
        }
    }

    Limits m_limits;
    std::unordered_map<quint64, QString> m_pending;
    struct QStringHash {
        std::size_t operator()(const QString &s) const
        {
            return qHash(s);
        }
    };
    std::unordered_map<QString, Failures, QStringHash> m_failures;
    Clock::time_point m_lastRefusalLog{};
    bool m_haveRefusalLog = false;
    int m_suppressedRefusals = 0;
};

}
