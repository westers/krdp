// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

/**
 * What holds a stream back: the network, the encoder, or the client (AUD-FIX13), judged over a
 * window of socket readings (AUD-FIX14).
 *
 * AUD-FIX13 counted any one socket signal in one 1.5 s interval as network evidence: TCP's RTT at
 * 2x its minimum and 20 ms over it, two retransmits, a capacity estimate under the slow threshold,
 * a growing send queue. cray a312776 (2026-09-29, Buzz on 5 GHz Wi-Fi and busy): Wi-Fi's RTT
 * swings (23-61 ms) and its retransmits (952 in 13 min) met those rules on an unthrottled LAN. The
 * link was declared slow 9 s in at 6.6 Mbit/s while TCP measured 18-51 Mbit/s of capacity, the
 * client-limited verdict never came (network evidence cancelled it every interval), and the slow
 * link never cleared (4 min 26 s after a 6 Mbit/s tbf went away).
 *
 * Now the measured capacity decides and everything else only corroborates:
 * - capacity: Stats::CapacityEstimate, TCP's own delivery rate over samples that were *not*
 *   application-limited (the path, not the sender, set the pace), capacitySource "tcp". Samples
 *   taken while the receiver's window limited the sender (RwndLimitedShare of the busy time) are
 *   the client's reading rate and are left out.
 * - link-bound ("network"): over the last SlowWindow (judged once there is WindowFull of history),
 *   the SlowPercentile of the capacity samples is under the slow-link threshold *and* under
 *   LinkBoundFactor times the median rate sent: most of the window, the path carried little more
 *   than the stream and less than a normal link. Corroborated (TCP's RTT inflated in
 *   CorroboratedRttShare of the window's intervals and retransmits in CorroboratedRetransmitShare
 *   of them), CorroboratedBoundFactor is enough: a client-paced stream that fills a throttled path
 *   only in bursts. Or, with no capacity sample in the window at all (a kernel without delivery-rate
 *   samples: a modern one measures any sender that is not application-limited), the send queue
 *   backed up and growing (SendQueueBackedUp, two readings) in QueueShare of the samples, grown by
 *   SendQueueBackedUp over the window, the sender not application-limited in QueueShare of them,
 *   and the stream congested in CongestedShare of them. SIOCOUTQ counts what is in flight too, so a
 *   Wi-Fi round trip alone fills it; hence the net growth.
 * - corroboration (OPT-061, Buzz 2026-10-07 18:58 "capacity at most 744 kbit/s, 704 kbit/s sent", 187
 *   Mbit/s 15 s later; four false slow links and codec flips in 30 min): TCP's delivery rate of a sender
 *   that sends almost nothing is about what it sends, whatever the path can carry. A low capacity
 *   estimate alone therefore no longer declares a link-bound window. It needs at least one of:
 *   the estimate under the threshold in each of the last PersistIntervals intervals (not a dip),
 *   TCP's round trip QueueingDelay over its minimum (median of the window), retransmits at
 *   LossRatio of the segments sent, or a send queue backed up and growing in QueueShare of the
 *   window. A stream sending under LowVolumeShare of the threshold proves little about the path
 *   (that is exactly the false case): there only a backed-up growing send queue, or queueing delay
 *   together with that loss ratio, counts. The persistence rule does not.
 * - slow (Verdict::slow): link-bound and corroborated. The codec policy enters a slow link on it.
 * - fast (Verdict::fast): over the last RecoverWindow (full), capacity samples in at least
 *   RecoverKnownShare of the intervals and their median at or above the recovery threshold: the
 *   slow-link threshold, or RecoverGrowth times the most the path carried while it was link-bound
 *   (the highest window median of the last BoundMemory) if that is less (a 2-monitor stream's 30 Mbit/s threshold is more than 5 GHz Wi-Fi
 *   measures, 28-33 Mbit/s, yet 5x a 6 Mbit/s tbf). The codec policy leaves a slow link on it,
 *   whatever the delivery throttle or the client does.
 * - headroom: not link-bound, and the median capacity at least HeadroomFactor times the median
 *   sent (or, with no capacity sample, a sender that is application-limited with a clear queue, or
 *   the receiver's window as the limit), and the send queue not growing now.
 * - clientLimited: congestion (the client behind, a full frame window, RDP's round trip inflated)
 *   with headroom, or a delivery throttle the client caused, for ClientLimitedAfter intervals in a
 *   row; off again after as many without, or at once when the link turns link-bound. RTT spikes and
 *   retransmits never end it.
 * - RTT inflation and retransmits are only listed in the reasons (corroboration).
 * Without socket figures (a test transport, a closed socket) `network` is unknown and callers
 * keep the old rules (congestion counts as the link's).
 */
namespace KRdp::LinkEvidence
{
using Clock = std::chrono::steady_clock;

/// One reading of the connection's socket (RdpConnection::tcpInfo() + socketQueuedBytes()).
struct Socket {
    qint64 rttUs = 0; ///< tcpi_rtt, smoothed
    qint64 minRttUs = 0; ///< tcpi_min_rtt (the kernel's windowed minimum), 0 = unknown
    quint64 totalRetransmits = 0; ///< tcpi_total_retrans (running total)
    qint64 queuedBytes = -1; ///< SIOCOUTQ: unsent plus unacknowledged, -1 = unknown
    bool appLimited = true; ///< the latest delivery-rate sample was application-limited
    std::optional<quint64> busyUs; ///< tcpi_busy_time (running total), absent on old kernels
    std::optional<quint64> rwndLimitedUs; ///< tcpi_rwnd_limited (running total)
};

/// One adaptive-quality interval (VideoStream::updateAdaptiveQuality(), 1.5 s).
struct Signals {
    Clock::time_point at{}; ///< when; epoch = Interval after the previous one
    bool congested = false; ///< AdaptiveQuality's verdict: RDP RTT inflated, client behind, or a full window
    bool throttled = false; ///< the delivery throttle holds the source under the policy's rate
    std::optional<quint32> sentKbps; ///< NetworkDetection's measurement (demand-limited)
    std::optional<quint32> capacityKbps; ///< Stats::CapacityEstimate (TCP, network-limited samples only)
    std::optional<Socket> socket; ///< absent: no socket figures
    double slowBelowKbps = 15000; ///< the codec policy's slow-link threshold for these pixels
};

constexpr auto Interval = std::chrono::milliseconds(1500);
constexpr auto SlowWindow = std::chrono::seconds(10);
constexpr auto WindowFull = std::chrono::milliseconds(8900); ///< of history before anything is judged link-bound
constexpr double SlowPercentile = 0.65;
constexpr double LinkBoundFactor = 1.25;
constexpr double CorroboratedBoundFactor = 1.5;
constexpr double CorroboratedRttShare = 0.5;
constexpr double CorroboratedRetransmitShare = 0.34;
constexpr double RecoverGrowth = 2.0;
constexpr auto BoundMemory = std::chrono::seconds(120);
constexpr int MinCapacitySamples = 4;
constexpr int PersistIntervals = 3; ///< corroboration: the capacity estimate stayed low this many intervals in a row (4.5 s)
constexpr double LowVolumeShare = 0.05; ///< of the slow-link threshold: a stream this small hardly exercises the path
constexpr auto QueueingDelay = std::chrono::milliseconds(30); ///< corroboration: median TCP RTT over its minimum
constexpr double LossRatio = 0.01; ///< corroboration: retransmits per segment sent
constexpr quint64 LossMinRetransmits = 5; ///< ... and at least this many in the window
constexpr int TcpSegmentBits = 1448 * 8;
constexpr double QueueShare = 0.5;
constexpr double CongestedShare = 0.6;
constexpr auto RecoverWindow = std::chrono::seconds(15);
constexpr auto RecoverWindowFull = std::chrono::milliseconds(13400);
constexpr double RecoverKnownShare = 0.5;
constexpr auto History = std::chrono::seconds(20);

constexpr quint64 RetransmitsPerInterval = 2; ///< corroboration only
constexpr double RttInflationFactor = 2.0; ///< corroboration only
constexpr auto RttInflationMin = std::chrono::milliseconds(20);
constexpr qint64 SendQueueBackedUp = 64 * 1024; ///< FrameQueuePolicy::LinkClearBytes
constexpr double RwndLimitedShare = 0.5;
constexpr double HeadroomFactor = 1.5;
constexpr int ClientLimitedAfter = 3; ///< intervals (4.5 s)

enum class Limit { None, Link, Encoder, Client };
inline const char *limitName(Limit limit)
{
    switch (limit) {
    case Limit::None: return "none";
    case Limit::Link: return "link";
    case Limit::Encoder: return "encoder";
    case Limit::Client: return "client";
    }
    return "none";
}

enum class Cause { None, Link, Client };

struct Verdict {
    std::optional<bool> network; ///< link-bound over the window; nullopt = no socket figures
    bool slow = false; ///< enter a slow link (network, sustained over the window, corroborated)
    bool uncorroborated = false; ///< a low capacity window that was left undeclared for want of corroboration
    bool fast = false; ///< the capacity proves the link fast again (RecoverWindow)
    std::optional<quint32> capacityKbps; ///< median capacity over SlowWindow (none: no sample)
    double recoverKbps = 0; ///< the recovery threshold (see fast)
    bool capacityNow = false; ///< this interval had a capacity sample
    bool idle = false; ///< the sender is application-limited and the queue clear (the path takes all)
    bool throttledByLink = false; ///< the delivery throttle is on, and the link engaged it
    Cause throttleCause = Cause::None; ///< who engaged the running delivery throttle
    bool headroom = false;
    bool rwndLimited = false; ///< the receiver's window limited the sender this interval
    bool clientLimited = false; ///< held (ClientLimitedAfter)
    bool clientLimitedChanged = false;
    QString why; ///< the evidence, for logs and the `client-limited` event
    QString linkWhy; ///< why slow (slow) or fast again (fast), with the corroboration
};

struct Sample {
    Clock::time_point at;
    std::optional<quint32> capacityKbps; ///< left out while the receiver's window limited the sender
    std::optional<quint32> sentKbps;
    bool congested = false;
    bool queueGrowing = false;
    bool appLimited = true;
    qint64 queuedBytes = -1;
    bool rttInflated = false;
    quint64 retransmits = 0;
    qint64 rttUs = 0;
    qint64 minRttUs = 0; ///< tcpi_min_rtt, 0 = unknown
};

struct State {
    std::optional<Socket> previous;
    Clock::time_point previousAt{};
    bool previousBackedUp = false;
    QList<Sample> samples; ///< the last History
    quint32 boundKbps = 0; ///< the most the path carried while link-bound in the last BoundMemory (0 = none)
    QList<std::pair<Clock::time_point, quint32>> bound; ///< link-bound intervals: when, the median capacity (or sent)
    int clientSamples = 0; ///< in a row with the client-limited condition
    int otherSamples = 0; ///< in a row without it
    bool clientLimited = false;
    Cause throttleCause = Cause::None; ///< who engaged the running delivery throttle
};

/// Whether \a socket's round trip is inflated over its minimum (queueing on the path, or Wi-Fi).
inline bool rttInflated(const Socket &socket)
{
    if (socket.rttUs <= 0 || socket.minRttUs <= 0) return false;
    const qint64 margin = std::chrono::duration_cast<std::chrono::microseconds>(RttInflationMin).count();
    return double(socket.rttUs) >= RttInflationFactor * double(socket.minRttUs) && socket.rttUs - socket.minRttUs >= margin;
}

/// The nearest-rank \a p percentile (0-1) of \a values (not empty).
inline quint32 percentile(QList<quint32> values, double p)
{
    std::sort(values.begin(), values.end());
    const qsizetype rank = std::clamp<qsizetype>(qsizetype(std::ceil(p * double(values.size()))), 1, values.size());
    return values.at(rank - 1);
}

/// What corroborates a low capacity estimate (see the file comment); \a accepted when enough does.
struct Corroboration {
    bool accepted = false;
    QString why;
};

namespace detail
{
/// The corroborating signals over \a samples: RTT inflation and retransmits.
inline QString corroboration(const QList<Sample> &samples)
{
    int inflated = 0;
    quint64 retransmits = 0;
    qint64 maxRtt = 0;
    for (const auto &s : samples) {
        inflated += s.rttInflated ? 1 : 0;
        retransmits += s.retransmits;
        maxRtt = std::max(maxRtt, s.rttUs);
    }
    QStringList parts;
    if (inflated > 0) parts << QStringLiteral("TCP round trip inflated in %1 of %2 intervals (up to %3 ms)").arg(inflated).arg(samples.size()).arg(maxRtt / 1000);
    if (retransmits > 0) parts << QStringLiteral("%1 retransmits").arg(retransmits);
    return parts.join(QStringLiteral(", "));
}


/**
 * Whether the low capacity over \a window is corroborated. \a history is every kept sample (the
 * last PersistIntervals are the persistence test), \a queuedSamples the window's samples with a
 * growing send queue.
 */
inline Corroboration corroborate(const QList<Sample> &history, const QList<Sample> &window, double slowBelowKbps, quint32 medianSentKbps, int queuedSamples)
{
    Corroboration out;
    if (window.isEmpty()) return out;
    const bool lowVolume = double(medianSentKbps) < LowVolumeShare * slowBelowKbps;

    bool persisted = history.size() >= PersistIntervals;
    for (qsizetype i = history.size() - 1; persisted && i >= history.size() - PersistIntervals; --i) {
        const auto &c = history.at(i).capacityKbps;
        persisted = c && double(*c) < slowBelowKbps;
    }

    QList<qint64> delays;
    quint64 retransmits = 0;
    double segments = 0;
    const double intervalSeconds = std::chrono::duration<double>(Interval).count();
    for (const auto &s : window) {
        if (s.rttUs > 0 && s.minRttUs > 0) delays << std::max<qint64>(0, s.rttUs - s.minRttUs);
        retransmits += s.retransmits;
        segments += double(s.sentKbps.value_or(0)) * 1000.0 * intervalSeconds / TcpSegmentBits;
    }
    std::sort(delays.begin(), delays.end());
    const qint64 delayUs = delays.isEmpty() ? 0 : delays.at(delays.size() / 2);
    const bool queueing = delayUs >= std::chrono::duration_cast<std::chrono::microseconds>(QueueingDelay).count();
    const bool loss = retransmits >= LossMinRetransmits && segments > 0 && double(retransmits) >= LossRatio * segments;
    const bool queue = queuedSamples >= QueueShare * window.size();

    QStringList parts;
    if (lowVolume) {
        if (queue) parts << QStringLiteral("send queue growing in %1 of %2 intervals").arg(queuedSamples).arg(window.size());
        if (queueing && loss) parts << QStringLiteral("queueing delay %1 ms and %2 retransmits").arg(delayUs / 1000).arg(retransmits);
    } else {
        if (persisted) parts << QStringLiteral("low for the last %1 intervals").arg(PersistIntervals);
        if (queue) parts << QStringLiteral("send queue growing in %1 of %2 intervals").arg(queuedSamples).arg(window.size());
        if (queueing) parts << QStringLiteral("queueing delay %1 ms").arg(delayUs / 1000);
        if (loss) parts << QStringLiteral("%1 retransmits in about %2 segments").arg(retransmits).arg(qRound(segments));
    }
    out.accepted = !parts.isEmpty();
    out.why = parts.isEmpty() ? QStringLiteral("not corroborated") : QStringLiteral("corroborated by ") + parts.join(QStringLiteral(" and "));
    return out;
}
}

inline Verdict judge(State &state, const Signals &in)
{
    Verdict v;
    const Clock::time_point now = in.at != Clock::time_point{} ? in.at
        : state.previousAt != Clock::time_point{}              ? state.previousAt + Interval
                                                               : Clock::time_point{} + std::chrono::hours(1);
    state.previousAt = now;
    std::optional<quint32> medianSent;
    std::optional<quint32> capacityMedian;
    bool queueGrowing = false;
    if (!in.socket) {
        state.previous.reset();
        state.previousBackedUp = false;
        state.samples.clear();
        state.bound.clear();
        state.boundKbps = 0;
    } else {
        const Socket &socketNow = *in.socket;
        const Socket *before = state.previous ? &*state.previous : nullptr;
        if (before && socketNow.busyUs && before->busyUs && socketNow.rwndLimitedUs && before->rwndLimitedUs && *socketNow.busyUs > *before->busyUs) {
            const double busy = double(*socketNow.busyUs - *before->busyUs);
            const double rwnd = double(*socketNow.rwndLimitedUs >= *before->rwndLimitedUs ? *socketNow.rwndLimitedUs - *before->rwndLimitedUs : 0);
            v.rwndLimited = rwnd >= RwndLimitedShare * busy;
        }
        Sample sample;
        sample.at = now;
        sample.congested = in.congested;
        sample.sentKbps = in.sentKbps;
        sample.rttUs = socketNow.rttUs;
        sample.minRttUs = socketNow.minRttUs;
        sample.retransmits = before && socketNow.totalRetransmits >= before->totalRetransmits ? socketNow.totalRetransmits - before->totalRetransmits : 0;
        sample.rttInflated = rttInflated(socketNow);
        const bool backedUp = socketNow.queuedBytes >= SendQueueBackedUp;
        queueGrowing = backedUp && state.previousBackedUp && before && socketNow.queuedBytes >= before->queuedBytes && !v.rwndLimited;
        sample.queueGrowing = queueGrowing;
        sample.appLimited = socketNow.appLimited;
        sample.queuedBytes = socketNow.queuedBytes;
        if (!v.rwndLimited) {
            sample.capacityKbps = in.capacityKbps;
        }
        v.capacityNow = sample.capacityKbps.has_value();
        v.idle = socketNow.appLimited && !backedUp;
        state.previousBackedUp = backedUp;
        state.previous = socketNow;
        state.samples.append(sample);
        while (!state.samples.isEmpty() && now - state.samples.first().at > History) {
            state.samples.removeFirst();
        }

        // The windows.
        const bool full = now - state.samples.first().at >= WindowFull;
        const bool recoverFull = now - state.samples.first().at >= RecoverWindowFull;
        QList<Sample> window;
        QList<quint32> capacities;
        QList<quint32> sent;
        int queued = 0;
        int congested = 0;
        int busy = 0;
        QList<quint32> recoverCapacities;
        int recoverSamples = 0;
        for (const auto &s : std::as_const(state.samples)) {
            if (now - s.at <= RecoverWindow) {
                ++recoverSamples;
                if (s.capacityKbps) recoverCapacities << *s.capacityKbps;
            }
            if (now - s.at > SlowWindow) continue;
            window << s;
            if (s.capacityKbps) capacities << *s.capacityKbps;
            if (s.sentKbps) sent << *s.sentKbps;
            queued += s.queueGrowing ? 1 : 0;
            congested += s.congested ? 1 : 0;
            busy += s.appLimited ? 0 : 1;
        }
        const qint64 queueGrowth = window.isEmpty() || window.first().queuedBytes < 0 || window.last().queuedBytes < 0
            ? 0
            : window.last().queuedBytes - window.first().queuedBytes;
        if (!sent.isEmpty()) medianSent = percentile(sent, 0.5);
        if (!capacities.isEmpty()) capacityMedian = percentile(capacities, 0.5);
        v.capacityKbps = capacityMedian;
        const QString corroborated = detail::corroboration(window);
        const auto withCorroboration = [&corroborated](const QString &why) {
            return corroborated.isEmpty() ? why : why + QStringLiteral("; ") + corroborated;
        };
        const auto seconds = [](auto d) {
            return std::chrono::duration_cast<std::chrono::seconds>(d).count();
        };

        int inflated = 0;
        int retransmitting = 0;
        for (const auto &s : std::as_const(window)) {
            inflated += s.rttInflated ? 1 : 0;
            retransmitting += s.retransmits > 0 ? 1 : 0;
        }
        const bool corroboratedNow = inflated >= CorroboratedRttShare * window.size() && retransmitting >= CorroboratedRetransmitShare * window.size();
        bool linkBound = false;
        if (full && capacities.size() >= MinCapacitySamples && medianSent) {
            const quint32 high = percentile(capacities, SlowPercentile);
            const double factor = corroboratedNow ? CorroboratedBoundFactor : LinkBoundFactor;
            if (double(high) < in.slowBelowKbps && double(high) < factor * double(*medianSent)) {
                const Corroboration proof = detail::corroborate(state.samples, window, in.slowBelowKbps, *medianSent, queued);
                if (proof.accepted) {
                    linkBound = true;
                    state.bound.append({now, *capacityMedian});
                    v.linkWhy = withCorroboration(QStringLiteral("TCP capacity at most %1 kbit/s in %2 % of %3 s (threshold %4, %5 kbit/s sent); %6")
                                                      .arg(high)
                                                      .arg(qRound(SlowPercentile * 100))
                                                      .arg(seconds(SlowWindow))
                                                      .arg(qRound(in.slowBelowKbps))
                                                      .arg(*medianSent)
                                                      .arg(proof.why));
                } else {
                    v.uncorroborated = true;
                }
            }
        } else if (full && capacities.isEmpty() && queued >= QueueShare * window.size() && busy >= QueueShare * window.size()
                   && queueGrowth >= SendQueueBackedUp && congested >= CongestedShare * window.size()) {
            linkBound = true;
            if (medianSent) state.bound.append({now, *medianSent});
            v.linkWhy = withCorroboration(QStringLiteral("no capacity sample; send queue growing in %1 (by %2 KiB) and congested in %3 of %4 intervals")
                                              .arg(queued)
                                              .arg(queueGrowth / 1024)
                                              .arg(congested)
                                              .arg(window.size()));
        }
        while (!state.bound.isEmpty() && now - state.bound.first().first > BoundMemory) {
            state.bound.removeFirst();
        }
        state.boundKbps = 0;
        for (const auto &b : std::as_const(state.bound)) {
            state.boundKbps = std::max(state.boundKbps, b.second);
        }
        v.network = linkBound;
        v.slow = linkBound;
        v.recoverKbps = state.boundKbps > 0 ? std::min(in.slowBelowKbps, RecoverGrowth * double(state.boundKbps)) : in.slowBelowKbps;
        if (!linkBound && recoverFull && !recoverCapacities.isEmpty() && recoverCapacities.size() >= RecoverKnownShare * recoverSamples) {
            const quint32 median = percentile(recoverCapacities, 0.5);
            if (double(median) >= v.recoverKbps) {
                v.fast = true;
                v.linkWhy = QStringLiteral("TCP capacity median %1 kbit/s over %2 s (recovery threshold %3)").arg(median).arg(seconds(RecoverWindow)).arg(qRound(v.recoverKbps));
            }
        }

        QStringList room;
        if (!linkBound && !queueGrowing) {
            const double sentKbps = medianSent.value_or(0);
            if (capacityMedian && double(*capacityMedian) >= HeadroomFactor * sentKbps) {
                room << QStringLiteral("TCP capacity %1 kbit/s for %2 sent").arg(*capacityMedian).arg(quint32(sentKbps));
            } else if (!capacityMedian && v.idle) {
                room << QStringLiteral("TCP application-limited");
            }
            if (v.rwndLimited) {
                room << QStringLiteral("the client's receive window is full");
            }
        }
        v.headroom = !room.isEmpty();
        if (v.headroom) {
            room << QStringLiteral("TCP round trip %1 ms").arg(double(socketNow.rttUs) / 1000.0, 0, 'f', 1);
            if (!corroborated.isEmpty()) room << corroborated;
            v.why = room.join(QStringLiteral(", "));
        } else if (linkBound) {
            v.why = v.linkWhy;
        }
    }

    if (!in.throttled) {
        state.throttleCause = Cause::None;
    } else if (in.congested && v.network) {
        state.throttleCause = *v.network ? Cause::Link : v.headroom ? Cause::Client : state.throttleCause;
    } else if (state.throttleCause == Cause::Link && v.network == false
               && ((capacityMedian && double(*capacityMedian) >= in.slowBelowKbps) || v.headroom)) {
        // The path has room again: the throttle the link engaged is stale (it is still climbing
        // back, e.g. after a tbf went away).
        state.throttleCause = Cause::None;
    }
    v.throttleCause = state.throttleCause;
    v.throttledByLink = in.throttled && state.throttleCause == Cause::Link;
    const bool condition = v.headroom && ((in.congested && !v.throttledByLink) || (in.throttled && state.throttleCause == Cause::Client));
    if (condition) {
        state.otherSamples = 0;
        ++state.clientSamples;
    } else {
        state.clientSamples = 0;
        ++state.otherSamples;
    }
    const bool was = state.clientLimited;
    if (v.network.value_or(true)) {
        state.clientLimited = false; // link-bound (or no socket figures) ends it at once
    } else if (state.clientSamples >= ClientLimitedAfter) {
        state.clientLimited = true;
    } else if (state.otherSamples >= ClientLimitedAfter) {
        state.clientLimited = false;
    }
    v.clientLimited = state.clientLimited;
    v.clientLimitedChanged = was != state.clientLimited;
    return v;
}

/**
 * The `policy.limit` of a stats sample: the link when the codec policy sees a slow link, the
 * window is link-bound, or the link engaged the delivery throttle; else the encoder when the
 * software encoder is over its CPU budget; else the client when it is client-limited; else none.
 */
inline Limit classify(bool slowLink, const Verdict &v, bool encoderOverBudget)
{
    if (slowLink || v.network.value_or(false) || v.throttledByLink) return Limit::Link;
    if (encoderOverBudget) return Limit::Encoder;
    if (v.clientLimited) return Limit::Client;
    return Limit::None;
}
}
