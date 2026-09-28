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
    // The worker's encoder events, re-emitted by the proxy session, go where krdpserver sends a
    // local session's (SessionController::setSessions()).
    connect(session, &AbstractSession::encoderUnavailable, stream, &VideoStream::privateCodecUnavailable);
    connect(session, &AbstractSession::encoderBackendReported, stream, &VideoStream::encoderBackendReported);
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
        if (m_session) {
            m_session->reportEncoder(report);
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
    qInfo().noquote() << QStringLiteral("Worker video encoders: %1, %2, %3%4")
                             .arg(one("avc", caps.encoders.avc), one("hevc", caps.encoders.hevc), one("av1", caps.encoders.av1),
                                  caps.renderNode.isEmpty() ? QString() : QStringLiteral(" on ") + caps.renderNode);
    m_stream->updateEncoderPolicy(caps.encoders);
}
}
