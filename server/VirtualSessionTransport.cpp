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
    : QObject(parent), m_client(client), m_connection(connection), m_control(&control), m_resolve(std::move(resolve)),
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
        if (m_connection) deliverControlRecord(record, m_connection->authenticatedPamUid());
    }, Qt::QueuedConnection);
    connect(connection, &RdpConnection::stateChanged, this, [this](RdpConnection::State state) {
        if (state == RdpConnection::State::Closed) closed();
    }, Qt::QueuedConnection);
    connect(connection, &QObject::destroyed, this, [this] { closed(); });
}

VirtualSessionTransport::~VirtualSessionTransport() { closed(); }

bool VirtualSessionTransport::authorized() const
{
    if (!m_control || !m_connection || !m_endpoint || !m_endpoint->ready() || !m_handle) return false;
    const auto uid = m_connection->authenticatedPamUid();
    const auto attached = m_control->attachment(m_client);
    return uid && *uid && *uid == m_endpoint->target().uid && attached
        && attached->id == m_handle->id && attached->generation == m_handle->generation
        && attached->manager == m_handle->manager;
}

bool VirtualSessionTransport::attachmentMatches(const VirtualSessionRegistry::Handle &handle) const
{
    const auto attached = m_control ? m_control->attachment(m_client) : std::nullopt;
    return attached && attached->id == handle.id && attached->manager == handle.manager
        && attached->generation == handle.generation;
}

bool VirtualSessionTransport::bind()
{
    const QPointer<VirtualSessionTransport> alive(this);
    if (!m_control) return false;
    const auto handle = m_control->attachment(m_client);
    if (!handle) return false;
    const auto stillAttached = [alive, this, handle] {
        return alive && m_connection && attachmentMatches(*handle);
    };
    if (m_handle && m_handle->id == handle->id && m_handle->manager == handle->manager
        && m_handle->generation == handle->generation) return authorized();
    // Resolution is host code and may destroy this (including its callback).
    const auto resolve = m_resolve;
    const QPointer<ConsoleWorkerEndpoint> endpoint = resolve ? resolve(*handle) : nullptr;
    if (!stillAttached()) return false;
    const auto uid = m_connection ? m_connection->authenticatedPamUid() : std::nullopt;
    if (!endpoint || !endpoint->ready() || !uid || !*uid || endpoint->target().uid != *uid
        || endpoint->target().adapter != ConsoleSeat::Adapter::VirtualUser || endpoint->target().sessionId != handle->id
        || m_sequence == std::numeric_limits<quint64>::max()) return false;
    return activateBinding(*handle, endpoint);
}

bool VirtualSessionTransport::activateBinding(const VirtualSessionRegistry::Handle &expected, QPointer<ConsoleWorkerEndpoint> endpoint)
{
    // bind() has validated endpoint and PAM identity. Keep the continuation
    // separate so callback invalidation can be tested without fabricating PAM.
    const auto handle = expected;
    const QPointer<VirtualSessionTransport> alive(this);
    const auto stillAttached = [alive, this, handle] {
        return alive && m_connection && attachmentMatches(handle);
    };
    if (!stillAttached() || !endpoint) return false;
    revoke();
    if (!stillAttached() || !endpoint) return false;
    // A revoke signal may have installed a replacement binding reentrantly.
    if (m_handle || m_endpoint || m_sequence == std::numeric_limits<quint64>::max()) return false;
    m_endpoint = endpoint;
    m_handle = handle;
    const auto bindingCurrent = [alive, this, handle, endpoint, stillAttached] {
        return stillAttached() && endpoint && m_endpoint == endpoint && m_handle
            && m_handle->id == handle.id && m_handle->manager == handle.manager
            && m_handle->generation == handle.generation;
    };
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [this](const VideoFrame &frame) {
        if (authorized()) m_session.submitFrame(frame);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::audioReceived, this, [this](const auto &audio) {
        if (m_playback && authorized()) m_connection->submitExternalAudio(audio.pcm);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::workerStopped, this, [this] {
        unavailable(); // desktop remains supervised independently
    }));
    endpoint->setControlState({++m_sequence, true});
    if (!bindingCurrent()) return false;
    m_session.setWorkerActive(true);
    if (!bindingCurrent()) return false;
    // reset() only sets pendingReset; setEnabled() emits enabledChanged.
    m_connection->videoStream()->reset();
    m_connection->videoStream()->setEnabled(true);
    if (!bindingCurrent()) return false;
    endpoint->requestKeyFrame();
    return bindingCurrent() && authorized();
}

void VirtualSessionTransport::revoke()
{
    const QPointer<VirtualSessionTransport> alive(this);
    const auto endpoint = m_endpoint;
    quint64 sequence = 0;
    if (endpoint) {
        if (m_sequence != std::numeric_limits<quint64>::max()) ++m_sequence;
        sequence = m_sequence;
    }
    // Clear local binding before any signal/callback can reenter teardown.
    for (const auto &connection : m_workerConnections) QObject::disconnect(connection);
    m_workerConnections.clear();
    m_endpoint = nullptr;
    m_handle.reset();
    m_playback = false;
    if (endpoint) {
        endpoint->setControlState({sequence, false});
        if (!alive) return;
        if (endpoint) endpoint->setMedia({false, false});
        if (!alive) return;
    }
    m_session.setWorkerActive(false);
    if (!alive) return;
    if (m_connection) {
        m_connection->setMediaPolicy(false, false, false);
        if (!alive) return;
        if (m_connection) m_connection->videoStream()->setEnabled(false); // discard old desktop frames
    }
}

void VirtualSessionTransport::closed()
{
    if (m_closing) return;
    m_closing = true;
    const QPointer<VirtualSessionTransport> alive(this);
    const auto control = m_control;
    const auto client = m_client;
    // Control guards the whole revocation sequence, including synchronous
    // signals: nested commands cannot attach a replacement under this client.
    // Its captured registry state completes disconnect even if revoke kills us.
    if (control) control->disconnected(client, [alive] { if (alive) alive->revoke(); });
    else revoke();
    if (alive) m_closing = false;
}

void VirtualSessionTransport::unavailable()
{
    const QPointer<VirtualSessionTransport> alive(this);
    closed();
    if (alive && m_connection) m_connection->close();
}

QJsonObject VirtualSessionTransport::request(const QJsonObject &record)
{
    return request(record, m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
}

void VirtualSessionTransport::deliverControlRecord(const QJsonObject &record, std::optional<quint32> uid)
{
    const QPointer<VirtualSessionTransport> alive(this);
    const auto connection = m_connection;
    const auto response = request(record, uid);
    if (alive && connection && m_connection == connection && !response.isEmpty()) connection->sendControlRecord(response);
}

QJsonObject VirtualSessionTransport::request(const QJsonObject &record, std::optional<quint32> uid)
{
    if (record.value(u"type"_s) == u"virtual-session"_s) {
        const QPointer<VirtualSessionTransport> alive(this);
        if (!m_control) return {};
        auto response = m_control->request(uid, m_client, record);
        if (!alive || !m_control || !m_connection) return {};
        if (response.value(u"ok"_s).toBool() && record.value(u"action"_s) == u"attach"_s) {
            const auto handle = m_control->attachment(m_client);
            const bool bound = bind();
            if (!alive || !m_control || !m_connection) return {};
            if (bound && handle && attachmentMatches(*handle) && authorized()) return response;
            // A callback may have attached a different desktop. Do not detach
            // that replacement while refusing the stale attach acknowledgement.
            if (handle && attachmentMatches(*handle)) closed();
            if (!alive || !m_control || !m_connection) return {};
            response.insert(u"ok"_s, false);
            response.insert(u"message"_s, u"authenticated virtual capture unavailable"_s);
        }
        return response;
    }
    if (record.value(u"type"_s) == u"media"_s) {
        const QPointer<VirtualSessionTransport> alive(this);
        const auto playback = record.value(u"playback"_s);
        const auto microphone = record.value(u"microphone"_s);
        const auto camera = record.value(u"camera"_s);
        const auto silence = record.value(u"silenceHost"_s);
        const bool ownsDesktop = authorized();
        const bool accepted = ownsDesktop && record.value(u"v"_s).isDouble() && record.value(u"v"_s).toDouble() == 1
            && playback.isBool() && microphone.isBool() && camera.isBool()
            && (silence.isUndefined() || silence.isBool()) && !microphone.toBool() && !camera.toBool();
        // A refused update must not report playback off while continuing the
        // previous stream. Revoke this connection even after ownership loss,
        // but never change a worker now belonging to another attachment.
        m_playback = accepted && playback.toBool();
        if (m_connection) {
            // setExternalAudioPlayback only updates atomics/queue state.
            m_connection->setExternalAudioPlayback(true);
            m_connection->setMediaPolicy(m_playback, false, false);
            if (!alive || !m_connection) return {};
        }
        if (ownsDesktop && m_endpoint) {
            m_endpoint->setMedia({m_playback, m_playback && silence.toBool()});
            if (!alive || !m_connection) return {};
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
