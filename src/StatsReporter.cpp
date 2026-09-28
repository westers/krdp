// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "StatsReporter.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QJsonArray>
#include <QTimer>

#include "LayoutControl.h"

namespace KRdp
{
namespace Stats
{
const char *eventKindName(EventKind kind)
{
    switch (kind) {
    case EventKind::Codec:
        return "codec";
    case EventKind::Settings:
        return "settings";
    case EventKind::KeyFrame:
        return "keyframe";
    case EventKind::Coalesce:
        return "coalesce";
    case EventKind::SlowLink:
        return "slow-link";
    case EventKind::CpuGuard:
        return "cpu-guard";
    case EventKind::Throttle:
        return "throttle";
    case EventKind::Quality:
        return "quality";
    }
    return "?";
}

QString encoderPresetName(const QString &family, int preset)
{
    if (family == QLatin1String("hevc")) {
        switch (preset) {
        case 0:
            return QStringLiteral("veryfast");
        case 1:
            return QStringLiteral("superfast");
        default:
            return QStringLiteral("ultrafast");
        }
    }
    if (family == QLatin1String("av1")) {
        // SVT-AV1 2.3 maps preset 12 to 11 in low-delay mode (CodecPolicy::presetLevel()).
        return preset == 0 ? QStringLiteral("M10") : QStringLiteral("M11");
    }
    return QStringLiteral("ultrafast"); // libx264 always runs at ultrafast
}

namespace
{
/// A running total's growth; a total that went down (a counter restarted) counts from zero.
quint64 delta(quint64 current, quint64 previous)
{
    return current >= previous ? current - previous : current;
}
double oneDecimal(double value)
{
    return std::round(value * 10.0) / 10.0;
}
double twoDecimals(double value)
{
    return std::round(value * 100.0) / 100.0;
}
qint64 kib(qint64 bytes)
{
    return (std::max<qint64>(bytes, 0) + 512) / 1024;
}
}

QJsonObject sampleRecord(const Snapshot &current, const Snapshot &previous, qint64 intervalMs, quint64 seq)
{
    const qint64 interval = std::max<qint64>(intervalMs, 1);

    QJsonObject video;
    if (!current.codec.isEmpty()) {
        video.insert(QStringLiteral("codec"), current.codec);
    }
    if (current.hardware) {
        video.insert(QStringLiteral("backend"), *current.hardware ? QStringLiteral("hardware") : QStringLiteral("software"));
    }
    if (!current.preset.isEmpty()) {
        video.insert(QStringLiteral("preset"), current.preset);
    }
    if (!current.chroma.isEmpty()) {
        video.insert(QStringLiteral("chroma"), current.chroma);
    }
    if (!current.size.isEmpty()) {
        video.insert(QStringLiteral("width"), current.size.width());
        video.insert(QStringLiteral("height"), current.size.height());
    }
    video.insert(QStringLiteral("frameRate"), current.frameRate);
    video.insert(QStringLiteral("frameRateCap"), current.frameRateCap);
    video.insert(QStringLiteral("quality"), current.quality);
    video.insert(QStringLiteral("qualityCap"), current.qualityCap);
    if (current.targetKbps) {
        video.insert(QStringLiteral("targetKbps"), qint64(*current.targetKbps));
    }
    // bits per millisecond = kbit/s
    video.insert(QStringLiteral("sentKbps"), qint64(delta(current.bytesSent, previous.bytesSent) * 8 / quint64(interval)));
    video.insert(QStringLiteral("framesSent"), qint64(delta(current.framesSent, previous.framesSent)));
    video.insert(QStringLiteral("framesAcked"), qint64(delta(current.framesAcked, previous.framesAcked)));
    video.insert(QStringLiteral("coalesced"), qint64(delta(current.coalesced, previous.coalesced)));
    video.insert(QStringLiteral("keyframes"), qint64(delta(current.keyframes, previous.keyframes)));
    video.insert(QStringLiteral("keyframeRequests"), qint64(delta(current.keyframeRequests, previous.keyframeRequests)));
    if (current.encoderRestarts) {
        video.insert(QStringLiteral("encoderRestarts"), qint64(delta(*current.encoderRestarts, previous.encoderRestarts.value_or(0))));
    }
    video.insert(QStringLiteral("framesEncoded"), qint64(delta(current.framesEncoded, previous.framesEncoded)));
    if (current.framesSkipped) {
        video.insert(QStringLiteral("framesSkipped"), qint64(delta(*current.framesSkipped, previous.framesSkipped.value_or(0))));
    }
    if (current.encodeLoadP95) {
        video.insert(QStringLiteral("encodeLoadP95"), twoDecimals(*current.encodeLoadP95));
    }
    if (current.encodeMs) {
        video.insert(QStringLiteral("encodeMs"), oneDecimal(*current.encodeMs));
    }

    QJsonObject flow{
        {QStringLiteral("inFlight"), current.inFlight},
        {QStringLiteral("window"), current.window},
        {QStringLiteral("windowKiB"), kib(current.windowBytes)},
        {QStringLiteral("acksSuspended"), current.acksSuspended},
        {QStringLiteral("throttled"), current.throttled},
    };
    if (current.ackLatencyMs) {
        flow.insert(QStringLiteral("ackLatencyMs"), oneDecimal(*current.ackLatencyMs));
    }
    // Stage 7: the client's own decode and render times, from the standard RDPGFX QoE
    // acknowledgements, averaged over the interval; absent when it sent none.
    if (const quint64 frames = delta(current.qoeFrames, previous.qoeFrames); frames > 0) {
        flow.insert(QStringLiteral("clientDecodeMs"), oneDecimal(double(delta(current.qoeDecodeMs, previous.qoeDecodeMs)) / double(frames)));
        flow.insert(QStringLiteral("clientRenderMs"), oneDecimal(double(delta(current.qoeRenderMs, previous.qoeRenderMs)) / double(frames)));
    }

    QJsonObject link{
        {QStringLiteral("sentSamples"), current.sentSamples},
        {QStringLiteral("congested"), current.congested},
        {QStringLiteral("slow"), current.slow},
    };
    if (current.rttMs) {
        link.insert(QStringLiteral("rttMs"), oneDecimal(*current.rttMs));
    }
    if (current.rttMinMs) {
        link.insert(QStringLiteral("rttMinMs"), oneDecimal(*current.rttMinMs));
    }
    if (current.rttVarMs) {
        link.insert(QStringLiteral("rttVarMs"), oneDecimal(*current.rttVarMs));
    }
    if (current.sentKbps) {
        link.insert(QStringLiteral("sentKbps"), qint64(*current.sentKbps));
    }
    if (current.capacityKbps && !current.capacitySource.isEmpty()) {
        link.insert(QStringLiteral("capacityKbps"), qint64(*current.capacityKbps));
        link.insert(QStringLiteral("capacitySource"), current.capacitySource);
    }
    if (current.appLimited) {
        link.insert(QStringLiteral("appLimited"), *current.appLimited);
    }
    if (current.retransmits) {
        // The first sample after a subscription has a previous snapshot too (taken at subscribe).
        link.insert(QStringLiteral("retransmits"), qint64(previous.retransmits ? delta(*current.retransmits, *previous.retransmits) : 0));
    }
    if (current.sendQueueBytes) {
        link.insert(QStringLiteral("sendQueueKiB"), kib(*current.sendQueueBytes));
    }

    QJsonObject guard{{QStringLiteral("state"), current.guardState}};
    if (!current.heldBack.isEmpty()) {
        guard.insert(QStringLiteral("heldBack"), QJsonArray::fromStringList(current.heldBack));
    }
    if (current.retryInS) {
        guard.insert(QStringLiteral("retryInS"), *current.retryInS);
    }
    QJsonObject policy{
        {QStringLiteral("mode"), current.mode},
        {QStringLiteral("cpuGuard"), guard},
    };
    if (current.adaptive) {
        policy.insert(QStringLiteral("adaptive"), *current.adaptive);
    }

    QJsonArray surfaces;
    for (qsizetype i = 0; i < current.surfaces.size(); ++i) {
        const auto &now = current.surfaces.at(i);
        const auto before = i < previous.surfaces.size() ? previous.surfaces.at(i) : SurfaceCounters{};
        surfaces.append(QJsonObject{
            {QStringLiteral("index"), int(i)},
            {QStringLiteral("framesSent"), qint64(delta(now.framesSent, before.framesSent))},
            {QStringLiteral("coalesced"), qint64(delta(now.coalesced, before.coalesced))},
            {QStringLiteral("keyframes"), qint64(delta(now.keyframes, before.keyframes))},
        });
    }

    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("stats-sample")},
        {QStringLiteral("v"), LayoutControl::ProtocolVersion},
        {QStringLiteral("seq"), qint64(seq)},
        {QStringLiteral("intervalMs"), interval},
        {QStringLiteral("video"), video},
        {QStringLiteral("flow"), flow},
        {QStringLiteral("link"), link},
        {QStringLiteral("policy"), policy},
        {QStringLiteral("surfaces"), surfaces},
    };
}

void CapacityEstimate::sample(Clock::time_point now, quint64 bytesPerSecond, bool appLimited)
{
    m_samples.removeIf([now](const Entry &e) {
        return now - e.at > Window;
    });
    if (!appLimited && bytesPerSecond > 0) {
        m_samples.append({now, bytesPerSecond});
    }
}

std::optional<quint32> CapacityEstimate::kbps(Clock::time_point now) const
{
    quint64 best = 0;
    for (const auto &e : m_samples) {
        if (now - e.at <= Window) {
            best = std::max(best, e.bytesPerSecond);
        }
    }
    if (best == 0) {
        return std::nullopt;
    }
    return quint32(std::min<quint64>(best * 8 / 1000, std::numeric_limits<quint32>::max()));
}

void CapacityEstimate::clear()
{
    m_samples.clear();
}

QJsonObject eventRecord(EventKind kind, quint64 seq, const EventDetail &detail)
{
    QJsonObject record{
        {QStringLiteral("type"), QStringLiteral("stats-event")},
        {QStringLiteral("v"), LayoutControl::ProtocolVersion},
        {QStringLiteral("seq"), qint64(seq)},
        {QStringLiteral("kind"), QString::fromLatin1(eventKindName(kind))},
        {QStringLiteral("reason"), detail.reason.left(512)},
    };
    if (!detail.codec.isEmpty()) {
        record.insert(QStringLiteral("codec"), detail.codec);
    }
    if (detail.hardware) {
        record.insert(QStringLiteral("backend"), *detail.hardware ? QStringLiteral("hardware") : QStringLiteral("software"));
    }
    return record;
}
}

StatsReporter::StatsReporter(Source source, Sink sink, QObject *parent)
    : QObject(parent)
    , m_source(std::move(source))
    , m_sink(std::move(sink))
{
    for (auto &last : m_lastEventMs) {
        last.store(std::numeric_limits<qint64>::min() / 2, std::memory_order_relaxed);
    }
    m_clock.start();
}

StatsReporter::~StatsReporter() = default;

int StatsReporter::clampRate(int rateHz, bool slowLink)
{
    return slowLink ? SlowLinkRateHz : std::clamp(rateHz, MinRateHz, MaxRateHz);
}

int StatsReporter::subscribe(int requested)
{
    const bool was = subscribed();
    m_requestedRate = std::clamp(requested, MinRateHz, MaxRateHz);
    if (!was) {
        m_previous = m_source ? m_source() : Stats::Snapshot{};
        m_sinceSample.start();
        m_timer = std::make_unique<QTimer>();
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer.get(), &QTimer::timeout, this, &StatsReporter::sendSample);
        m_subscribed.store(true);
    }
    applyRate();
    if (!was) {
        Q_EMIT subscriptionChanged(true);
    }
    return rateHz();
}

void StatsReporter::unsubscribe()
{
    if (!m_subscribed.exchange(false)) {
        return;
    }
    m_timer.reset();
    m_requestedRate = 0;
    m_previous = {};
    Q_EMIT subscriptionChanged(false);
}

int StatsReporter::rateHz() const
{
    return subscribed() ? clampRate(m_requestedRate, m_slowLink) : 0;
}

bool StatsReporter::timerActive() const
{
    return m_timer && m_timer->isActive();
}

void StatsReporter::setSlowLink(bool slow)
{
    if (m_slowLink == slow) {
        return;
    }
    m_slowLink = slow;
    if (subscribed()) {
        applyRate();
    }
}

void StatsReporter::setSink(Sink sink)
{
    m_sink = std::move(sink);
}

void StatsReporter::applyRate()
{
    if (!m_timer) {
        return;
    }
    const int interval = 1000 / rateHz();
    if (m_timer->interval() != interval || !m_timer->isActive()) {
        m_timer->start(interval);
    }
}

void StatsReporter::sendSample()
{
    if (!subscribed() || !m_source) {
        return;
    }
    const auto current = m_source();
    const qint64 interval = m_sinceSample.restart();
    const auto record = Stats::sampleRecord(current, m_previous, interval, m_seq.fetch_add(1, std::memory_order_relaxed));
    m_previous = current;
    if (!m_sink || !m_sink(record)) {
        unsubscribe(); // the channel closed: the subscription ends with it
    }
}

void StatsReporter::sendEvent(Stats::EventKind kind, const Stats::EventDetail &detail)
{
    if (kind == Stats::EventKind::Coalesce || kind == Stats::EventKind::KeyFrame) {
        auto &last = m_lastEventMs[size_t(kind)];
        const qint64 now = m_clock.elapsed();
        qint64 previous = last.load(std::memory_order_relaxed);
        do {
            if (now - previous < EventMinIntervalMs) {
                return;
            }
        } while (!last.compare_exchange_weak(previous, now, std::memory_order_relaxed));
    }
    if (m_sink) {
        m_sink(Stats::eventRecord(kind, m_seq.fetch_add(1, std::memory_order_relaxed), detail));
    }
}
}

#include "moc_StatsReporter.cpp"
