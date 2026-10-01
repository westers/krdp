// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "WorkerCodecBridge.h"

#include <QDebug>

#include <VideoStream.h>

#include "ConsoleWorkerEndpoint.h"
#include "ConsoleWorkerSession.h"

namespace KRdp
{
WorkerCodecBridge::WorkerCodecBridge(VideoStream *stream, ConsoleWorkerSession *session, QObject *parent)
    : QObject(parent)
    , m_stream(stream)
    , m_session(session)
{
    Q_ASSERT(stream && session);
    // negotiatedCodecChanged comes from the FreeRDP peer thread; the others from the main thread.
    connect(stream, &VideoStream::negotiatedCodecChanged, this, [this] { send(false); }, Qt::QueuedConnection);
    connect(stream, &VideoStream::encoderSettingsChanged, this, [this] { send(false); }, Qt::QueuedConnection);
    connect(stream, &VideoStream::requestedFrameRateChanged, this, [this] { send(false); }, Qt::QueuedConnection);
    connect(stream, &VideoStream::requestedChromaChanged, this, [this] { send(false); }, Qt::QueuedConnection);
    // STATS-S6: the worker reports EncoderStats only while this connection's client is subscribed.
    connect(stream, &VideoStream::statsSubscriptionChanged, this, [this] { send(false); });
    // The worker's encoder events, re-emitted by the proxy session, go where krdpserver sends a
    // local session's (SessionController::setSessions()).
    connect(session, &AbstractSession::encoderUnavailable, stream, &VideoStream::privateCodecUnavailable);
    connect(session, &AbstractSession::encoderBackendReported, stream, &VideoStream::encoderBackendReported);
    connect(session, &AbstractSession::chromaCapabilityChanged, stream, &VideoStream::setChromaCapable);
    stream->setEncoderCpuTimeSource([this]() -> qint64 {
        return m_endpoint && m_generation ? m_endpoint->workerCpuNs() : -1;
    });
}

WorkerCodecBridge::~WorkerCodecBridge()
{
    if (m_stream) {
        m_stream->setEncoderCpuTimeSource({});
    }
    unbind();
}

void WorkerCodecBridge::bind(ConsoleWorkerEndpoint *endpoint, quint64 generation)
{
    unbind();
    if (!endpoint || !generation) {
        return;
    }
    m_endpoint = endpoint;
    m_generation = generation;
    m_endpointConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::encoderCapsReceived, this, &WorkerCodecBridge::applyCaps));
    m_endpointConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::encoderReported, this, [this](const ConsoleWorkerWire::EncoderReport &report) {
        const QPointer<WorkerCodecBridge> alive(this);
        if (m_stream && report.codec == m_stream->codecForSessions()) {
            const bool avcSoftware = report.event == ConsoleWorkerWire::EncoderReport::Event::Backend
                && !report.hardware && report.codec != VideoCodec::Hevc && report.codec != VideoCodec::Av1;
            const bool avc444Lost = VideoCodecSupport::isAvc444(report.codec)
                && (report.event == ConsoleWorkerWire::EncoderReport::Event::Unavailable
                    || (report.event == ConsoleWorkerWire::EncoderReport::Event::ChromaCapability && !report.chromaCapable));
            if (avcSoftware || avc444Lost) m_stream->setAvc444Available(false);
        }
        if (!alive) return;
        if (m_session) {
            m_session->reportEncoder(report);
        }
    }));
    m_endpointConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::encoderStatsReceived, this, [this](const ConsoleWorkerWire::EncoderStats &stats) {
        if (m_stream && m_stream->statsSubscribed()) {
            m_stream->addWorkerEncoderStats(stats.framesEncoded, stats.framesSkipped,
                                            stats.encodeUs >= 0 ? std::optional<double>(stats.encodeUs / 1000.0) : std::nullopt);
        }
    }));
    m_endpointConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::workerReady, this, [this] {
        resend(); // a replacement worker behind the same endpoint starts from its defaults
    }));
    if (const auto caps = endpoint->encoderCaps()) {
        applyCaps(*caps);
    }
    send(true);
}

void WorkerCodecBridge::unbind()
{
    for (const auto &connection : std::as_const(m_endpointConnections)) {
        disconnect(connection);
    }
    m_endpointConnections.clear();
    m_endpoint = nullptr;
    m_generation = 0;
    m_sent.reset();
    if (m_stream) {
        const QPointer<WorkerCodecBridge> alive(this);
        m_stream->setChromaCapable(false);
        if (alive && m_stream) m_stream->setAvc444Available(false);
    }
}

bool WorkerCodecBridge::setChromaPolicy(const ChromaPolicy &policy)
{
    if (!policy.isValid()) return false;
    if (m_chromaPolicy == policy) return true;
    m_chromaPolicy = policy;
    send(false);
    return true;
}

bool WorkerCodecBridge::bound() const
{
    return m_endpoint && m_generation;
}

void WorkerCodecBridge::resend()
{
    send(true);
}

std::optional<ConsoleWorkerWire::EncoderConfig> WorkerCodecBridge::config() const
{
    if (!m_stream || !m_generation) {
        return std::nullopt;
    }
    ConsoleWorkerWire::EncoderConfig config;
    config.generation = m_generation;
    config.codec = m_stream->codecForSessions();
    config.settings = m_stream->encoderSettings();
    config.frameRate = std::clamp<quint32>(m_stream->requestedFrameRate(), 1, 240);
    config.statsWanted = m_stream->statsSubscribed();
    config.chroma = m_chromaPolicy;
    config.chromaEnabled = m_stream->requestedChroma();
    return config;
}

void WorkerCodecBridge::send(bool force)
{
    const auto next = config();
    if (!next || !m_endpoint || !m_endpoint->ready()) {
        return;
    }
    if (!force && m_sent == next) {
        return;
    }
    if (m_endpoint->setEncoderConfig(*next)) {
        m_sent = next;
    }
}

void WorkerCodecBridge::applyCaps(const ConsoleWorkerWire::EncoderCaps &caps)
{
    const QPointer<WorkerCodecBridge> alive(this);
    if (!m_stream) {
        return;
    }
    const auto one = [](const char *name, const CodecPolicy::Backends &b) {
        return QStringLiteral("%1 %2").arg(QLatin1String(name),
                                           b.hardware && b.software ? QStringLiteral("hw+sw")
                                               : b.hardware         ? QStringLiteral("hw")
                                               : b.software         ? QStringLiteral("sw")
                                                                    : QStringLiteral("none"));
    };
    qInfo().noquote() << QStringLiteral("Worker video encoders: %1, %2, %3%4; AVC444 %5")
                             .arg(one("avc", caps.encoders.avc), one("hevc", caps.encoders.hevc), one("av1", caps.encoders.av1),
                                  caps.renderNode.isEmpty() ? QString() : QStringLiteral(" on ") + caps.renderNode,
                                  caps.avc444Hardware ? QStringLiteral("available") : QStringLiteral("unavailable"));
    m_stream->updateEncoderPolicy(caps.encoders);
    if (alive && m_stream) m_stream->setAvc444Available(caps.avc444Hardware);
}
}
