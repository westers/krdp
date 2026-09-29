// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <optional>

/**
 * AUD-FIX13: what holds a stream back: the network, the encoder, or the client.
 *
 * cray (573fa31, hardware AV1, 2026-09-28): Buzz decodes AV1 in software and takes 10-14 of 30
 * fps. Its acks lag, so the frame window fills, the RDP-level round trip (answered by the
 * client's busy thread) inflates and the delivery throttle stays on: AdaptiveQuality's
 * "congested" was true all session, and the codec policy declared a slow link 9 s into an
 * unthrottled LAN (17.4 Mbit/s sent, 38.8 Mbit/s TCP capacity, 0 retransmits, 19 ms RTT) and
 * never cleared it. Slow acks alone say nothing about the network.
 *
 * The socket does: the kernel measures its own round trip (tcpi_rtt against tcpi_min_rtt, not
 * delayed by the client's application), counts retransmits, holds what the path does not take
 * in the send queue (SIOCOUTQ), and flags delivery-rate samples that were limited by the
 * application or by the receiver's window. judge() turns one adaptive-quality interval of those
 * into:
 * - network: socket-level evidence that the network is the limit. Any of: the TCP capacity
 *   estimate (Stats::CapacityEstimate, network-limited delivery-rate samples) under the slow-link
 *   threshold; RetransmitsPerInterval or more retransmits; the kernel's RTT inflated (at least
 *   RttInflationFactor times its minimum and RttInflationMin over it); a send queue backed up
 *   (SendQueueBackedUp) and growing over two intervals. The last two, and the capacity, do not
 *   count while the receiver's window limited the sender (RwndLimitedShare of the busy time):
 *   then the client is not reading, and that is the client's limit.
 * - headroom: no such evidence, and positive evidence of room: the capacity well above what is
 *   sent (HeadroomFactor, and at least the slow threshold), the latest delivery-rate sample
 *   application-limited (the sender had less to send than the path could take), or the
 *   receiver's window as the limit.
 * - throttle cause: the delivery throttle is engaged by an interval with pressure; whose it is
 *   follows the last congested interval: the link's with network evidence, the client's with
 *   headroom. A throttled stream that fits the path shows no congestion, but a throttle the link
 *   caused still holds it (a 14 fps client paced to 10 fps by a 6 Mbit/s tbf is the link's limit):
 *   throttledByLink, until TCP measures the path above the slow threshold again.
 * - clientLimited: congestion with headroom, or a throttle the client caused, for
 *   ClientLimitedAfter samples in a row (and off again after as many without, or at once with
 *   network evidence).
 * Without socket figures (a test transport, a closed socket) `network` is unknown and callers
 * keep the old rule (congestion counts as the link's).
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
    bool congested = false; ///< AdaptiveQuality's verdict: RDP RTT inflated, client behind, or a full window
    bool throttled = false; ///< the delivery throttle holds the source under the policy's rate
    std::optional<quint32> sentKbps; ///< NetworkDetection's measurement (demand-limited)
    std::optional<quint32> capacityKbps; ///< Stats::CapacityEstimate (TCP, network-limited samples only)
    std::optional<Socket> socket; ///< absent: no socket figures
    double slowBelowKbps = 15000; ///< the codec policy's slow-link threshold for these pixels
};

constexpr quint64 RetransmitsPerInterval = 2;
constexpr double RttInflationFactor = 2.0;
constexpr auto RttInflationMin = std::chrono::milliseconds(20);
constexpr qint64 SendQueueBackedUp = 64 * 1024; ///< FrameQueuePolicy::LinkClearBytes
constexpr double RwndLimitedShare = 0.5;
constexpr double HeadroomFactor = 1.5;
constexpr int ClientLimitedAfter = 2; ///< samples (3 s)

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
    std::optional<bool> network; ///< nullopt = no socket figures
    bool throttledByLink = false; ///< the delivery throttle is on, and the link engaged it
    Cause throttleCause = Cause::None; ///< who engaged the running delivery throttle
    bool headroom = false;
    bool rwndLimited = false; ///< the receiver's window limited the sender this interval
    bool clientLimited = false; ///< held (ClientLimitedAfter)
    bool clientLimitedChanged = false;
    QString why; ///< the evidence, for logs and the `client-limited` event
};

struct State {
    std::optional<Socket> previous;
    bool previousBackedUp = false;
    int clientSamples = 0; ///< in a row with the client-limited condition
    int otherSamples = 0; ///< in a row without it
    bool clientLimited = false;
    Cause throttleCause = Cause::None; ///< who engaged the running delivery throttle
};

/// Whether \a socket's round trip is inflated over its minimum (queueing on the path).
inline bool rttInflated(const Socket &socket)
{
    if (socket.rttUs <= 0 || socket.minRttUs <= 0) return false;
    const qint64 margin = std::chrono::duration_cast<std::chrono::microseconds>(RttInflationMin).count();
    return double(socket.rttUs) >= RttInflationFactor * double(socket.minRttUs) && socket.rttUs - socket.minRttUs >= margin;
}

inline Verdict judge(State &state, const Signals &in)
{
    Verdict v;
    if (!in.socket) {
        state.previous.reset();
        state.previousBackedUp = false;
    } else {
        const Socket &now = *in.socket;
        const Socket *before = state.previous ? &*state.previous : nullptr;
        QStringList network;
        QStringList room;
        if (before && now.busyUs && before->busyUs && now.rwndLimitedUs && before->rwndLimitedUs && *now.busyUs > *before->busyUs) {
            const double busy = double(*now.busyUs - *before->busyUs);
            const double rwnd = double(*now.rwndLimitedUs >= *before->rwndLimitedUs ? *now.rwndLimitedUs - *before->rwndLimitedUs : 0);
            v.rwndLimited = rwnd >= RwndLimitedShare * busy;
        }
        const quint64 retransmits = before && now.totalRetransmits >= before->totalRetransmits ? now.totalRetransmits - before->totalRetransmits : 0;
        if (retransmits >= RetransmitsPerInterval) {
            network << QStringLiteral("%1 retransmits").arg(retransmits);
        }
        if (!v.rwndLimited) {
            if (in.capacityKbps && double(*in.capacityKbps) < in.slowBelowKbps) {
                network << QStringLiteral("TCP capacity %1 kbit/s").arg(*in.capacityKbps);
            }
            if (rttInflated(now)) {
                network << QStringLiteral("TCP round trip %1 ms (min %2)").arg(now.rttUs / 1000).arg(now.minRttUs / 1000);
            }
        }
        const bool backedUp = now.queuedBytes >= SendQueueBackedUp;
        if (backedUp && state.previousBackedUp && before && now.queuedBytes >= before->queuedBytes && !v.rwndLimited) {
            network << QStringLiteral("send queue %1 KiB and growing").arg(now.queuedBytes / 1024);
        }
        state.previousBackedUp = backedUp;
        state.previous = now;
        v.network = !network.isEmpty();
        if (!*v.network) {
            const double sent = in.sentKbps.value_or(0);
            if (in.capacityKbps && double(*in.capacityKbps) >= std::max(in.slowBelowKbps, HeadroomFactor * sent)) {
                room << QStringLiteral("TCP capacity %1 kbit/s for %2 sent").arg(*in.capacityKbps).arg(quint32(sent));
            }
            if (now.appLimited) {
                room << QStringLiteral("TCP application-limited");
            }
            if (v.rwndLimited) {
                room << QStringLiteral("the client's receive window is full");
            }
            v.headroom = !room.isEmpty();
            if (!room.isEmpty()) {
                room << QStringLiteral("TCP round trip %1 ms").arg(double(now.rttUs) / 1000.0, 0, 'f', 1);
                if (retransmits == 0) room << QStringLiteral("no retransmits");
            }
            v.why = room.join(QStringLiteral(", "));
        } else {
            v.why = network.join(QStringLiteral(", "));
        }
    }
    if (!in.throttled) {
        state.throttleCause = Cause::None;
    } else if (in.congested && v.network) {
        state.throttleCause = *v.network ? Cause::Link : v.headroom ? Cause::Client : state.throttleCause;
    } else if (state.throttleCause == Cause::Link && v.network == false && in.capacityKbps && double(*in.capacityKbps) >= in.slowBelowKbps) {
        // TCP now measures the path above the slow threshold: the throttle the link engaged is
        // stale (it is still climbing back, e.g. after a tbf went away).
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
    if (v.network.value_or(false) || !v.network) {
        state.clientLimited = false; // network evidence (or none at all) ends it at once
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
 * The `policy.limit` of a stats sample: the link when the codec policy sees a slow link, this
 * interval's congestion has network evidence, or the link engaged the delivery throttle; else the encoder when the software encoder is
 * over its CPU budget; else the client when it is client-limited; else none.
 */
inline Limit classify(bool slowLink, bool congested, const Verdict &v, bool encoderOverBudget)
{
    if (slowLink || (congested && v.network.value_or(false)) || v.throttledByLink) return Limit::Link;
    if (encoderOverBudget) return Limit::Encoder;
    if (v.clientLimited) return Limit::Client;
    return Limit::None;
}
}
