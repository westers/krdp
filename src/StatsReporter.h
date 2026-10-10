// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <chrono>
#include <optional>

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QSize>
#include <QStringList>
#include <QVector>

#include "CodecPolicy.h"
#include "krdp_export.h"

class QTimer;

namespace KRdp
{
/**
 * The KRDPCTL `stats` records (STATS-PANEL-DESIGN.md §5, KRDPCTL-V2-CONTRACT.md (g)): what a
 * `stats-sample` and a `stats-event` carry, and the pure functions that build them. Shared by
 * krdpserver and both brokers (each runs a VideoStream per connection, which owns the reporter).
 */
namespace Stats
{
/// The `kind` of a `stats-event`.
enum class EventKind { Codec, Settings, KeyFrame, Coalesce, SlowLink, CpuGuard, Throttle, Quality, ClientLimited };
constexpr int EventKindCount = 9;
KRDP_EXPORT const char *eventKindName(EventKind kind);

/// Sets \a snapshot's tcpRttMs / tcpRttMinMs from TCP_INFO's tcpi_rtt / tcpi_min_rtt in microseconds
/// (<= 0 = unknown, left unset; an implausible minute-long round trip is dropped like rttMs).
inline void applyTcpRtt(struct Snapshot &snapshot, qint64 rttUs, qint64 minRttUs);

/// One RDPGFX surface's running totals (index = VideoFrame::monitorIndex).
struct SurfaceCounters {
    quint64 framesSent = 0;
    quint64 coalesced = 0;
    quint64 keyframes = 0;
    bool operator==(const SurfaceCounters &) const = default;
};

/**
 * Everything one sample reads. Counters are running totals since the stream started (the
 * sample sends the difference to the previous snapshot); nullopt/empty = unknown, so the field
 * is left out of the record.
 */
struct Snapshot {
    // video
    QString codec; ///< "avc" | "hevc" | "av1"; empty before the caps are known
    std::optional<bool> hardware; ///< the backend of the encoder that runs
    /// OPT-062 S2: the concrete encoder behind `hardware` ("vaapi", "nvenc", "libx264", ...), the hardware device's PCI id and name when known
    QString encoder;
    QString device;
    QString deviceName;
    QString preset; ///< the software encoder's preset ("veryfast", "M10", ...); empty for hardware
    QString chroma; ///< "420" | "444"
    QSize size; ///< the RDP desktop (all surfaces)
    int frameRate = 0; ///< asked of the source (the delivery throttle included)
    int frameRateCap = 0; ///< the codec policy's cap
    int quality = 0;
    int qualityCap = 0;
    std::optional<quint32> targetKbps; ///< software bitrate mode only
    quint64 bytesSent = 0; ///< RDPGFX payload bytes handed to the channel
    quint64 framesSent = 0;
    quint64 framesAcked = 0;
    quint64 coalesced = 0;
    quint64 keyframes = 0;
    quint64 keyframeRequests = 0;
    std::optional<quint64> encoderRestarts;
    quint64 framesEncoded = 0; ///< frames the encoder(s) produced
    std::optional<quint64> framesSkipped; ///< a worker's frames not forwarded (EncoderStats)
    std::optional<double> encodeLoadP95; ///< CPU guard load, software only
    std::optional<double> encodeMs; ///< per frame: the software estimate or a measured time
    // flow
    int inFlight = 0;
    int window = 0;
    qint64 windowBytes = 0;
    std::optional<double> ackLatencyMs;
    quint64 qoeFrames = 0; ///< RDPGFX QoE frame acknowledgements received (stage 7)
    quint64 qoeDecodeMs = 0; ///< their timeDiffSE, summed
    quint64 qoeRenderMs = 0; ///< their timeDiffEDR, summed
    bool acksSuspended = false;
    bool throttled = false;
    // link
    std::optional<double> rttMs;
    std::optional<double> rttMinMs;
    std::optional<double> rttVarMs;
    /// The kernel's TCP round trip (smoothed) and its minimum (tcpi_rtt, tcpi_min_rtt), unlike
    /// rttMs which is RDP's round trip and includes a busy client's delay. Absent when unknown.
    std::optional<double> tcpRttMs;
    std::optional<double> tcpRttMinMs;
    /// AUD-FIX12: NetworkDetection's bandwidth measurement: what the server sent, as the client
    /// received it (demand-limited: never more than the session had to send). Was goodputKbps.
    std::optional<quint32> sentKbps;
    int sentSamples = 0;
    /// AUD-FIX12: an estimate of what the path can carry, only when there is real evidence
    /// (CapacityEstimate: network-limited TCP delivery-rate samples; or the slow-link probe's
    /// highest rate held without congestion). Absent when unknown.
    std::optional<quint32> capacityKbps;
    QString capacitySource; ///< "tcp" | "probe" (with capacityKbps)
    std::optional<bool> appLimited; ///< the latest TCP delivery-rate sample was application-limited
    std::optional<quint64> retransmits; ///< TCP_INFO total retransmits (running total)
    std::optional<qint64> sendQueueBytes;
    bool congested = false;
    bool slow = false;
    // policy
    QString mode; ///< SoftwareEncoding: "auto" | "never" | "prefer"
    std::optional<bool> adaptive; ///< the client's `codec` `adaptive`; absent without a `codec` request
    CodecPolicy::ClientDecode decode; ///< AV1-Q: the client's `codec` `decode` (Unknown = not said)
    /// OPT-062 S2: the codec request the policy runs on (empty without a private codec request) and the host's software ceiling
    QStringList order;
    QString encodeMode; ///< "hardware" | "software" | "any"
    QString decodeMode;
    std::optional<CodecPolicy::SoftwareAllowance> allowance;
    /// AV1-Q: the AV1 tile count the encoder was told (0 = KPipeWire's per-resolution rule); AV1 only
    std::optional<int> av1Tiles;
    QString guardState = QStringLiteral("ok"); ///< "ok" | "stepped" | "holding"
    QStringList heldBack; ///< codec families the CPU guard holds back
    /// AUD-FIX13: what holds the stream back (LinkEvidence::Limit): "link" | "encoder" | "client" | "none"
    QString limit = QStringLiteral("none");
    std::optional<int> retryInS; ///< until the first held-back codec may be tried again
    QVector<SurfaceCounters> surfaces;
};

/// Sets \a snapshot's tcpRttMs / tcpRttMinMs from TCP_INFO's tcpi_rtt / tcpi_min_rtt in
/// microseconds. <= 0 is unknown and an implausible minute-long round trip is dropped (as rttMs).
inline void applyTcpRtt(Snapshot &snapshot, qint64 rttUs, qint64 minRttUs)
{
    const auto ms = [](qint64 us) -> std::optional<double> {
        return us > 0 && us < 60'000'000 ? std::optional<double>(double(us) / 1000.0) : std::nullopt;
    };
    snapshot.tcpRttMs = ms(rttUs);
    snapshot.tcpRttMinMs = ms(minRttUs);
}

/**
 * AUD-FIX12: the path capacity TCP itself measured. The kernel keeps a delivery-rate sample per
 * ACK (tcpi_delivery_rate) and flags the ones taken while the sender had less to send than the
 * path could take (tcpi_delivery_rate_app_limited). Only the others say something about the
 * path: the highest of them over the last Window is the estimate (like BBR's bottleneck
 * filter). A session that never fills its path (most of them: a LAN, a still desktop) has none,
 * and then there is no estimate. Not thread-safe.
 */
class KRDP_EXPORT CapacityEstimate
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto Window = std::chrono::seconds(10);
    void sample(Clock::time_point now, quint64 bytesPerSecond, bool appLimited);
    /// The estimate in kbit/s, if a network-limited sample lies within Window of \a now.
    std::optional<quint32> kbps(Clock::time_point now) const;
    void clear();

private:
    struct Entry {
        Clock::time_point at;
        quint64 bytesPerSecond = 0;
    };
    QVector<Entry> m_samples; ///< network-limited samples of the last Window
};

/// The `stats-sample` for \a current, with counters as deltas against \a previous.
KRDP_EXPORT QJsonObject sampleRecord(const Snapshot &current, const Snapshot &previous, qint64 intervalMs, quint64 seq);

struct EventDetail {
    QString reason;
    QString codec; ///< empty: left out
    std::optional<bool> hardware;
};
KRDP_EXPORT QJsonObject eventRecord(EventKind kind, quint64 seq, const EventDetail &detail);

/// The preset name the running software encoder of \a family ("avc", "hevc", "av1") uses at
/// CodecPolicy::Preset \a preset: x264 ultrafast; x265 veryfast/superfast/ultrafast; SVT-AV1 M10/M11.
KRDP_EXPORT QString encoderPresetName(const QString &family, int preset);
}

/**
 * The server side of a KRDPCTL `stats` subscription, one per connection (owned by VideoStream).
 *
 * Unsubscribed it costs nothing: no timer exists, and event() is one relaxed atomic load and a
 * branch (its detail callback is never called). Subscribed, a timer sends a `stats-sample` at the
 * subscribed rate (1-4 Hz; 1 Hz while the link is slow), and event() sends a `stats-event` at
 * once. Coalesce and keyframe events are sent at most once per EventMinInterval each (a window
 * that stays full would otherwise send one per 10 ms poll).
 *
 * subscribe()/unsubscribe()/setSlowLink() and the timer run on the owner's (main) thread;
 * event() may be called from any thread (the sink must be thread-safe, as
 * RdpConnection::sendControlRecord() is).
 */
class KRDP_EXPORT StatsReporter : public QObject
{
    Q_OBJECT
public:
    static constexpr int MinRateHz = 1;
    static constexpr int MaxRateHz = 4;
    static constexpr int SlowLinkRateHz = 1;
    static constexpr qint64 EventMinIntervalMs = 1000;

    using Source = std::function<Stats::Snapshot()>;
    /// Sends one record; false = the channel is gone (the subscription ends).
    using Sink = std::function<bool(const QJsonObject &)>;

    StatsReporter(Source source, Sink sink, QObject *parent = nullptr);
    ~StatsReporter() override;

    /// \a rateHz clamped to MinRateHz..MaxRateHz (SlowLinkRateHz while the link is slow).
    static int clampRate(int rateHz, bool slowLink);

    /// Starts (or re-rates) the subscription; returns the rate samples are sent at.
    int subscribe(int requested);
    void unsubscribe();
    bool subscribed() const
    {
        return m_subscribed.load(std::memory_order_relaxed);
    }
    /// The rate samples go out at now (0 = unsubscribed).
    int rateHz() const;
    /// Whether a sample timer exists and runs (only while subscribed).
    bool timerActive() const;
    /// The link turned slow or recovered: a subscription is re-rated.
    void setSlowLink(bool slow);
    /// Replaces the sink (tests capture the records).
    void setSink(Sink sink);

    /**
     * A `stats-event` of \a kind, with the detail \a makeDetail returns (an EventDetail). Nothing
     * at all happens without a subscriber: \a makeDetail is not called.
     */
    template<typename MakeDetail>
    void event(Stats::EventKind kind, MakeDetail &&makeDetail)
    {
        if (!m_subscribed.load(std::memory_order_relaxed)) [[likely]] {
            return;
        }
        sendEvent(kind, std::forward<MakeDetail>(makeDetail)());
    }

    /// The next sample now (tests; the timer calls it).
    void sendSample();

    /// Emitted when a subscription starts or ends (main thread).
    Q_SIGNAL void subscriptionChanged(bool subscribed);

private:
    void sendEvent(Stats::EventKind kind, const Stats::EventDetail &detail);
    void applyRate();

    Source m_source;
    Sink m_sink;
    std::atomic<bool> m_subscribed = false;
    std::atomic<quint64> m_seq = 0;
    int m_requestedRate = 0;
    bool m_slowLink = false;
    std::unique_ptr<QTimer> m_timer;
    Stats::Snapshot m_previous;
    QElapsedTimer m_sinceSample;
    std::array<std::atomic<qint64>, Stats::EventKindCount> m_lastEventMs{};
    QElapsedTimer m_clock;
};
}
