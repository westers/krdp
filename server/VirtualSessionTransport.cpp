// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionTransport.h"
#include <InputHandler.h>
#include <VideoStream.h>
#include <limits>

using namespace Qt::StringLiterals;

namespace KRdp
{
VirtualSessionTransport::VirtualSessionTransport(quint64 client, RdpConnection *connection,
        VirtualSessionControl &control, Resolve resolve, quint64 &sequence, QObject *parent)
    : QObject(parent), m_client(client), m_connection(connection), m_control(control), m_resolve(std::move(resolve)),
      m_sequence(sequence), m_session([this](const auto &input) {
          if (authorized()) m_endpoint->sendInput(input);
      })
{
    Q_ASSERT(client && connection);
    connection->videoStream()->setCodecPreference(CodecPreference::Avc420);
    connection->videoStream()->setQualityCap(80);
    connection->videoStream()->setAdaptiveQuality(false);
    connection->videoStream()->setEnabled(false);
    connection->setExternalAudioPlayback(true); // broker must never open host PipeWire
    connection->setMediaPolicy(false, false, false);
    connect(&m_session, &AbstractSession::frameReceived, connection->videoStream(), &VideoStream::queueFrame);
    connect(connection->inputHandler(), &InputHandler::inputEvent, &m_session, &AbstractSession::sendEvent);
    connect(&m_session, &ConsoleWorkerSession::keyFrameRequested, this, [this] {
        if (authorized()) m_endpoint->requestKeyFrame();
    });
    connect(connection->videoStream(), &VideoStream::keyFrameRequested, this, [this](int) {
        if (authorized()) m_endpoint->requestKeyFrame();
    }, Qt::QueuedConnection);
    connect(connection, &RdpConnection::controlRecordReceived, this, [this](const QJsonObject &record) {
        if (m_connection) m_connection->sendControlRecord(request(record));
    }, Qt::QueuedConnection);
    connect(connection, &RdpConnection::stateChanged, this, [this](RdpConnection::State state) {
        if (state == RdpConnection::State::Closed) closed();
    }, Qt::QueuedConnection);
    connect(connection, &QObject::destroyed, this, [this] { closed(); });
}

VirtualSessionTransport::~VirtualSessionTransport() { closed(); }

bool VirtualSessionTransport::authorized() const
{
    if (!m_connection || !m_endpoint || !m_endpoint->ready() || !m_handle) return false;
    const auto uid = m_connection->authenticatedPamUid();
    const auto attached = m_control.attachment(m_client);
    return uid && *uid && *uid == m_endpoint->target().uid && attached
        && attached->id == m_handle->id && attached->generation == m_handle->generation
        && attached->manager == m_handle->manager;
}

bool VirtualSessionTransport::bind()
{
    const auto handle = m_control.attachment(m_client);
    if (!handle) return false;
    if (m_handle && m_handle->id == handle->id && m_handle->generation == handle->generation) return authorized();
    auto *endpoint = m_resolve ? m_resolve(*handle) : nullptr;
    const auto uid = m_connection ? m_connection->authenticatedPamUid() : std::nullopt;
    if (!endpoint || !endpoint->ready() || !uid || !*uid || endpoint->target().uid != *uid
        || endpoint->target().adapter != ConsoleSeat::Adapter::VirtualUser || endpoint->target().sessionId != handle->id
        || m_sequence == std::numeric_limits<quint64>::max()) return false;
    revoke();
    m_endpoint = endpoint;
    m_handle = handle;
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [this](const VideoFrame &frame) {
        if (authorized()) m_session.submitFrame(frame);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::audioReceived, this, [this](const auto &audio) {
        if (m_playback && authorized()) m_connection->submitExternalAudio(audio.pcm);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::workerStopped, this, [this] {
        closed();
        if (m_connection) m_connection->close(); // desktop remains supervised independently
    }));
    endpoint->setControlState({++m_sequence, true});
    m_session.setWorkerActive(true);
    m_connection->videoStream()->reset();
    m_connection->videoStream()->setEnabled(true);
    endpoint->requestKeyFrame();
    return true;
}

void VirtualSessionTransport::revoke()
{
    m_session.setWorkerActive(false);
    m_playback = false;
    if (m_connection) {
        m_connection->setMediaPolicy(false, false, false);
        m_connection->videoStream()->setEnabled(false); // discard old desktop frames
    }
    if (m_endpoint) {
        // Same authenticated socket ordering releases held input before any
        // later owner's grant, even if the transport itself has already died.
        if (m_sequence != std::numeric_limits<quint64>::max()) ++m_sequence;
        m_endpoint->setControlState({m_sequence, false});
        m_endpoint->setMedia({false, false});
    }
    for (const auto &connection : m_workerConnections) QObject::disconnect(connection);
    m_workerConnections.clear();
    m_endpoint = nullptr;
    m_handle.reset();
}

void VirtualSessionTransport::closed()
{
    revoke();
    m_control.disconnected(m_client);
}

void VirtualSessionTransport::unavailable()
{
    closed();
    if (m_connection) m_connection->close();
}

QJsonObject VirtualSessionTransport::request(const QJsonObject &record)
{
    if (record.value(u"type"_s) == u"virtual-session"_s) {
        auto response = m_control.request(m_connection ? m_connection->authenticatedPamUid() : std::nullopt, m_client, record);
        if (response.value(u"ok"_s).toBool() && record.value(u"action"_s) == u"attach"_s && !bind()) {
            closed();
            response.insert(u"ok"_s, false);
            response.insert(u"message"_s, u"authenticated virtual capture unavailable"_s);
        }
        return response;
    }
    if (record.value(u"type"_s) == u"media"_s) {
        const auto playback = record.value(u"playback"_s);
        const auto microphone = record.value(u"microphone"_s);
        const auto camera = record.value(u"camera"_s);
        const auto silence = record.value(u"silenceHost"_s);
        const bool accepted = authorized() && record.value(u"v"_s).isDouble() && record.value(u"v"_s).toDouble() == 1
            && playback.isBool() && microphone.isBool() && camera.isBool()
            && (silence.isUndefined() || silence.isBool()) && !microphone.toBool() && !camera.toBool();
        if (accepted) {
            m_playback = playback.toBool();
            m_connection->setExternalAudioPlayback(true);
            m_connection->setMediaPolicy(m_playback, false, false);
            m_endpoint->setMedia({m_playback, m_playback && silence.toBool()});
        }
        QJsonObject response{{u"type"_s, u"media"_s}, {u"v"_s, 1}, {u"ok"_s, accepted},
                             {u"playback"_s, accepted && m_playback}, {u"microphone"_s, false}, {u"camera"_s, false},
                             {u"silenceHost"_s, accepted && m_playback && silence.toBool()}};
        if (!accepted) response.insert(u"message"_s, u"attached owner playback only; microphone/camera unavailable in this transport"_s);
        return response;
    }
    return {{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"code"_s, u"unsupported"_s},
            {u"message"_s, u"virtual-session transport does not support this request"_s}};
}
}
