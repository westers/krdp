// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionTransport.h"
#include "AudioPriority.h"
#include "VirtualResizeProtocol.h"
#include "RemoteMonitorGeometry.h"
#include <InputHandler.h>
#include <VideoStream.h>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <limits>

using namespace Qt::StringLiterals;

namespace KRdp
{
namespace
{
bool frameMatchesTopology(const VideoFrame &frame, const RemoteTopologyCatalog::Snapshot &snapshot)
{
    if (snapshot.outputs.isEmpty() || snapshot.outputs.size() != frame.monitors.size()
        || frame.monitorIndex < 0 || frame.monitorIndex >= frame.monitors.size()
        || frame.size != frame.monitors[frame.monitorIndex].geometry.size()) return false;
    QVector<RemoteMonitorGeometry::Output> projection;
    for (const auto &entry : snapshot.outputs) {
        const auto &output = entry.output;
        if (!output.enabled || output.physical || output.nativePixels.isEmpty()) return false;
        projection.append({output.logicalGeometry.topLeft(), output.nativePixels, output.scale, output.primary});
    }
    return RemoteMonitorGeometry::projectToWire(projection) == frame.monitors;
}
}

VirtualSessionTransport::VirtualSessionTransport(quint64 client, RdpConnection *connection,
        VirtualSessionControl &control, Resolve resolve, quint64 &sequence, QObject *parent)
    : QObject(parent), m_client(client), m_connection(connection), m_control(&control), m_resolve(std::move(resolve)),
      m_sequence(sequence), m_session([this](const auto &input) {
          if (authorized()) m_endpoint->sendInput(input);
      })
{
    Q_ASSERT(client && connection);
    connection->setAudioPriorityDefault(false);
    connection->clearAudioPriorityOverride();
    connection->videoStream()->setCodecPreference(CodecPreference::Avc420);
    connection->videoStream()->setQualityCap(80);
    connection->videoStream()->setAdaptiveQuality(false);
    connection->videoStream()->setEnabled(false);
    connection->setExternalAudioPlayback(true); // broker must never open host PipeWire
    m_externalMicrophone = connection->enableExternalMicrophone();
    connection->setMediaPolicy(false, false, false);
    m_microphoneDeadline.setSingleShot(true);
    m_microphoneDeadline.setInterval(4000);
    connect(&m_microphoneDeadline, &QTimer::timeout, this, [this] {
        const QPointer<VirtualSessionTransport> alive(this);
        const auto reply = microphoneTimeout();
        if (alive && m_connection && !reply.isEmpty()) m_connection->sendControlRecord(reply);
    });
    m_microphonePump.setInterval(20);
    connect(&m_microphonePump, &QTimer::timeout, this, &VirtualSessionTransport::pumpMicrophone);
    m_resizeDeadline.setSingleShot(true);
    m_resizeDeadline.setInterval(60000);
    connect(&m_resizeDeadline, &QTimer::timeout, this, [this] {
        const auto response = resizeResult({m_resizeWorkerId, m_resizeGeneration,
            u"virtual resize timed out; refresh before retrying"_s},
            m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (m_connection && !response.isEmpty()) m_connection->sendControlRecord(response);
    });
    m_topologyDeadline.setSingleShot(true);
    m_topologyDeadline.setInterval(5000);
    connect(&m_topologyDeadline, &QTimer::timeout, this, [this] {
        const QString id = m_topologyId;
        clearTopology();
        if (m_connection && !id.isEmpty())
            m_connection->sendControlRecord(RemoteTopologyProtocol::error(id, u"timeout"_s));
    });
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
    return authorized(m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
}

bool VirtualSessionTransport::authorized(std::optional<quint32> uid) const
{
    // The explicit identity is private to socket-free tests. Production callers
    // supply only RdpConnection's PAM identity, never a control-record field.
    if (m_revoking || !m_control || !m_connection || !m_endpoint || !m_endpoint->ready() || !m_handle) return false;
    const auto attached = m_control->attachment(m_client);
    const auto target = m_endpoint->target();
    return uid && *uid && *uid == target.uid && target.adapter == ConsoleSeat::Adapter::VirtualUser
        && target.sessionId == m_handle->id && attached
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
    if (m_revoking || !m_control) return false;
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
    if (m_revoking || !stillAttached() || !endpoint) return false;
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
        if (!authorized()) return;
        const QPointer<VirtualSessionTransport> alive(this);
        if (frame.monitors.size() > 1 || !m_wireLayout.isEmpty()) {
            if (!m_topologyResolve || !m_handle) return;
            const auto topology = m_topologyResolve(*m_handle);
            if (!alive || !topology || !authorized() || !frameMatchesTopology(frame, *topology)) return;
            if (m_wireLayout != frame.monitors) {
                if (!frame.isKeyFrame) return;
                m_connection->videoStream()->setMonitorLayout(frame.monitors.size() > 1 ? frame.monitors : QVector<VideoMonitor>{});
                m_wireLayout = frame.monitors.size() > 1 ? frame.monitors : QVector<VideoMonitor>{};
            }
        }
        m_session.submitFrame(frame);
        if (!alive || !m_connection) return;
        const auto reply = topologyFrame(frame, m_connection->authenticatedPamUid());
        if (alive && m_connection && !reply.isEmpty()) m_connection->sendControlRecord(reply);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::audioReceived, this, [this](const auto &audio) {
        if (m_playback && authorized()) m_connection->submitExternalAudio(audio.pcm);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::workerStopped, this, [this] {
        unavailable(); // desktop remains supervised independently
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::microphoneFinished, this, [this](const auto &result) {
        const QPointer<VirtualSessionTransport> alive(this);
        const auto reply = microphoneResult(result, m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (alive && m_connection && !reply.isEmpty()) m_connection->sendControlRecord(reply);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::resizeFinished, this, [this](const auto &result) {
        const auto response = resizeResult(result, m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (m_connection && !response.isEmpty()) m_connection->sendControlRecord(response);
    }));
    m_controlGeneration = ++m_sequence;
    // Capture the binding's generation at connection time, NOT delivery time:
    // disconnect does not retract already queued signals from the RDP thread.
    const auto generation = m_controlGeneration;
    m_workerConnections.append(connect(m_connection->videoStream(), &VideoStream::requestedQualityChanged,
        this, [this, generation](quint8 quality) {
            forwardVideoQuality(generation, quality, m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        }, Qt::QueuedConnection));
    endpoint->setControlState({m_controlGeneration, true});
    if (!bindingCurrent()) return false;
    endpoint->setVideoQuality({generation, 80});
    if (!bindingCurrent()) return false;
    m_session.setWorkerActive(true);
    if (!bindingCurrent()) return false;
    // reset() only sets pendingReset; setEnabled() emits enabledChanged.
    m_connection->videoStream()->reset();
    m_wireLayout.clear();
    m_connection->videoStream()->setMonitorLayout({});
    m_connection->videoStream()->setEnabled(true);
    if (!bindingCurrent()) return false;
    endpoint->requestKeyFrame();
    return bindingCurrent() && authorized();
}

void VirtualSessionTransport::revoke()
{
    if (m_revoking) return;
    const QPointer<VirtualSessionTransport> alive(this);
    m_revoking = true;
    clearResize(); // Invalidate before any teardown callback can rebind.
    clearTopology();
    // Clear preference before any teardown signal can destroy this transport.
    if (m_connection) {
        m_connection->setAudioPriorityDefault(false);
        m_connection->clearAudioPriorityOverride();
    }
    // Invalidate local consent before dispatch, while the old endpoint is still
    // pinned. Nested requests/binds cannot acquire a replacement during revoke.
    stopMicrophone();
    if (!alive) return;
    const auto endpoint = m_endpoint;
    const auto generation = m_controlGeneration;
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
    m_wireLayout.clear();
    m_playback = false;
    m_silenceHost = false;
    m_controlGeneration = 0;
    if (endpoint) {
        // Reset the retained desktop encoder before withdrawing the old grant.
        // Its generation prevents this reset affecting a replacement owner.
        if (generation) endpoint->setVideoQuality({generation, 80});
        if (!alive) return;
        if (endpoint) endpoint->setControlState({sequence, false});
        if (!alive) return;
        if (endpoint) endpoint->setMedia({false, false});
        if (!alive) return;
    }
    m_session.setWorkerActive(false);
    if (!alive) return;
    if (m_connection) {
        // The old worker was reset explicitly. Do not re-emit quality while
        // tearing down (possibly from a quality callback deleting this object).
        {
            const QSignalBlocker quiet(m_connection->videoStream());
            m_connection->videoStream()->setQualityCap(80);
        }
        if (!alive) return;
        if (!m_connection) { m_revoking = false; return; }
        m_connection->videoStream()->setMonitorLayout({});
        m_connection->setMediaPolicy(false, false, false);
        if (!alive) return;
        if (m_connection) m_connection->videoStream()->setEnabled(false); // discard old desktop frames
    }
    if (alive) m_revoking = false;
}

bool VirtualSessionTransport::forwardVideoQuality(quint64 generation, quint8 quality, std::optional<quint32> uid)
{
    if (!generation || generation != m_controlGeneration || !authorized(uid) || quality < 10 || quality > 100) return false;
    // Off/final-audio-off also neutralizes a queued reduction from the same
    // binding. Virtual desktops retain their fixed 80 cap outside priority.
    const quint8 bounded = m_connection->audioPriorityActive() ? std::min<quint8>(quality, 80) : 80;
    return m_endpoint->setVideoQuality({generation, bounded});
}

void VirtualSessionTransport::restoreFixedVideoQuality(std::optional<quint32> uid)
{
    if (!m_connection || m_connection->audioPriorityActive()) return;
    const QPointer<VirtualSessionTransport> alive(this);
    // Worker writes require current ownership. Local reset remains necessary
    // after ownership/endpoint loss, but must never reset a replacement worker.
    forwardVideoQuality(m_controlGeneration, 80, uid);
    if (!alive || !m_connection || m_connection->audioPriorityActive()) return;
    m_connection->videoStream()->setQualityCap(80); // May synchronously destroy/reenter this transport.
}

QJsonObject VirtualSessionTransport::mediaReply(bool ok, const QString &error) const
{
    QJsonObject reply{{u"type"_s, u"media"_s}, {u"v"_s, 1}, {u"ok"_s, ok},
        {u"playback"_s, m_playback}, {u"microphone"_s, m_microphoneReady}, {u"camera"_s, false},
        {u"silenceHost"_s, m_silenceHost}};
    if (!error.isEmpty()) reply.insert(u"message"_s, error);
    return reply;
}

void VirtualSessionTransport::stopMicrophone()
{
    stopMicrophone(m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
}

void VirtualSessionTransport::stopMicrophone(std::optional<quint32> uid)
{
    const QPointer<VirtualSessionTransport> alive(this);
    const bool wasPriority = m_connection && m_connection->audioPriorityActive();
    m_microphoneDeadline.stop();
    m_microphonePump.stop();
    const auto policy = m_microphonePolicy;
    m_microphonePolicy = {};
    m_microphoneReady = false;
    // setMediaPolicy updates consent atomics and clears the generation-bound
    // AUDIN queue; it does not emit signals. Playback remains independent.
    if (m_connection) m_connection->setMediaPolicy(m_playback, false, false, false);
    if (policy.enabled && m_endpoint && m_nextMicrophoneId != std::numeric_limits<quint64>::max()) {
        m_endpoint->setMicrophone({policy.generation, ++m_nextMicrophoneId, false});
    }
    if (alive && wasPriority) restoreFixedVideoQuality(uid);
}

QJsonObject VirtualSessionTransport::microphoneResult(const ConsoleWorkerWire::MicrophoneResult &result,
                                                     std::optional<quint32> uid)
{
    if (!m_microphonePolicy.enabled || result.generation != m_microphonePolicy.generation
        || result.requestId != m_microphonePolicy.requestId) return {};
    const QPointer<VirtualSessionTransport> alive(this);
    if (!authorized(uid) || !result.error.isEmpty()) {
        const QString error = result.error.isEmpty() ? u"virtual microphone authority changed"_s : result.error;
        stopMicrophone(uid);
        return alive ? mediaReply(false, error) : QJsonObject{};
    }
    if (m_microphoneReady) return {};
    m_microphoneDeadline.stop();
    m_connection->setMediaPolicy(m_playback, true, false, false);
    m_microphoneReady = true;
    m_microphonePump.start();
    return mediaReply(true);
}

QJsonObject VirtualSessionTransport::microphoneTimeout()
{
    if (!m_microphonePolicy.enabled || m_microphoneReady) return {};
    const QPointer<VirtualSessionTransport> alive(this);
    stopMicrophone();
    return alive ? mediaReply(false, u"virtual microphone worker startup timed out"_s) : QJsonObject{};
}

bool VirtualSessionTransport::forwardMicrophone(const QByteArray &pcm, std::optional<quint32> uid)
{
    if (!m_microphoneReady || !m_microphonePolicy.enabled || !authorized(uid)) return false;
    if (pcm.isEmpty() || pcm.size() > 3840 || pcm.size() % 4) return false;
    return m_endpoint->sendMicrophoneAudio({m_microphonePolicy.generation, m_microphonePolicy.requestId, pcm});
}

void VirtualSessionTransport::pumpMicrophone()
{
    if (!m_microphoneReady) return;
    if (!authorized()) { stopMicrophone(); return; }
    // Queue drains at most 20ms, expires old speech, and never retries backlog
    // rejected by the worker socket. No PipeWire object exists in this broker.
    const auto pcm = m_connection->takeExternalMicrophone();
    forwardMicrophone(pcm, m_connection->authenticatedPamUid());
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

void VirtualSessionTransport::clearResize()
{
    m_resizeDeadline.stop();
    m_resizeId.clear();
    m_resizeWorkerId = 0;
    m_resizeGeneration = 0;
}

void VirtualSessionTransport::clearTopology()
{
    m_topologyDeadline.stop();
    m_topologyId.clear();
    m_topologyBinding = 0;
}

QJsonObject VirtualSessionTransport::topologyFrame(const VideoFrame &frame, std::optional<quint32> uid)
{
    if (m_topologyId.isEmpty() || !m_topologyBinding || m_topologyBinding != m_controlGeneration
        || !authorized(uid) || !m_handle || !m_topologyResolve
        || !frame.isKeyFrame || frame.data.isEmpty() || frame.size.isEmpty()) return {};
    const QPointer<VirtualSessionTransport> alive(this);
    const auto snapshot = m_topologyResolve(*m_handle);
    if (!alive || !snapshot || !authorized(uid) || !frameMatchesTopology(frame, *snapshot)) return {};
    if (snapshot->generation.isEmpty() || !snapshot->revision) return {};
    for (const auto &entry : snapshot->outputs) if (entry.output.owner != m_handle->id) return {};
    const QString id = m_topologyId;
    clearTopology();
    return RemoteTopologyProtocol::retainedReadOnly(id, *snapshot);
}

QJsonObject VirtualSessionTransport::requestResize(const QJsonObject &record, std::optional<quint32> uid)
{
    const auto parsed = VirtualResizeProtocol::parse(record);
    const auto id = record.value(u"id"_s).toString().left(64);
    if (!parsed) return VirtualResizeProtocol::reply(id, u"invalid virtual resize request"_s);
    if (!authorized(uid)) return VirtualResizeProtocol::reply(id, u"attach to your virtual desktop before resizing"_s);
    // An in-flight ID denotes the original operation. Retrying it must not
    // deliver an early failure followed by a contradictory eventual success.
    if (m_resizeId == id) return {};
    if (!m_resizeId.isEmpty()) return VirtualResizeProtocol::reply(id, u"virtual resize already pending"_s);
    if (m_nextResizeId == std::numeric_limits<quint64>::max())
        return VirtualResizeProtocol::reply(id, u"virtual resize request sequence exhausted"_s);
    m_resizeId = id;
    m_resizeWorkerId = ++m_nextResizeId;
    m_resizeGeneration = m_controlGeneration;
    const auto generation = m_resizeGeneration;
    const auto workerId = m_resizeWorkerId;
    m_resizeDeadline.start();
    const QPointer<VirtualSessionTransport> alive(this);
    const bool dispatched = m_endpoint->resize({workerId, generation, u"virtual-desktop"_s, parsed->pixels, parsed->scale});
    if (!alive) return {};
    if (m_resizeGeneration != generation || m_resizeWorkerId != workerId || !authorized(uid)) return {};
    if (!dispatched) {
        clearResize();
        return VirtualResizeProtocol::reply(id, u"cannot dispatch virtual resize"_s);
    }
    return {}; // Worker acknowledges only after readback and matching capture.
}

QJsonObject VirtualSessionTransport::resizeResult(const ConsoleWorkerWire::ResizeResult &result,
                                                 std::optional<quint32> uid)
{
    if (m_resizeId.isEmpty() || result.requestId != m_resizeWorkerId || result.generation != m_resizeGeneration) return {};
    const QString id = m_resizeId;
    const bool current = m_controlGeneration == result.generation && authorized(uid);
    clearResize();
    return current ? VirtualResizeProtocol::reply(id, result.error) : QJsonObject{};
}

QJsonObject VirtualSessionTransport::request(const QJsonObject &record, std::optional<quint32> uid)
{
    if (m_revoking) return {};
    if (record.value(u"type"_s) == u"topology-query"_s) {
        const auto id = RemoteTopologyProtocol::queryId(record);
        if (!id) return RemoteTopologyProtocol::error(record.value(u"id"_s).toString().left(64), u"invalid"_s);
        if (!authorized(uid)) return RemoteTopologyProtocol::error(*id, u"not-owner"_s);
        if (!m_topologyResolve) return RemoteTopologyProtocol::error(*id, u"unsupported"_s);
        if (!m_topologyId.isEmpty()) return m_topologyId == *id ? QJsonObject{} : RemoteTopologyProtocol::error(*id, u"busy"_s);
        m_topologyId = *id;
        m_topologyBinding = m_controlGeneration;
        m_topologyDeadline.start();
        m_endpoint->requestKeyFrame();
        return {};
    }
    if (record.value(u"type"_s) == u"virtual-resize"_s) return requestResize(record, uid);
    if (record.value(u"type"_s) == u"audio-priority"_s) {
        const auto parsed = AudioPriority::parse(record);
        if (!parsed) return AudioPriority::reply(record, false, u"invalid audio-priority request"_s);
        if (!authorized(uid)) return AudioPriority::reply(record, false, u"only the authenticated attached virtual owner may change audio priority"_s);
        const QPointer<VirtualSessionTransport> alive(this);
        const auto generation = m_controlGeneration;
        m_connection->setAudioPriority(parsed->enabled);
        const bool effective = m_connection->audioPriorityActive();
        if (!effective) {
            // Immediate restore; do not wait for another desktop frame.
            forwardVideoQuality(m_controlGeneration, 80, uid);
            if (!alive || !m_connection) return {};
            m_connection->videoStream()->setQualityCap(80);
            if (!alive || !m_connection) return {};
        }
        if (generation != m_controlGeneration || !authorized(uid))
            return AudioPriority::reply(record, false, u"virtual desktop authority changed"_s);
        return AudioPriority::reply(record, effective);
    }
    if (record.value(u"type"_s) == u"virtual-session"_s) {
        const QPointer<VirtualSessionTransport> alive(this);
        if (!m_control) return {};
        auto response = m_control->request(uid, m_client, record);
        if (!alive || !m_control || !m_connection) return {};
        if (response.value(u"ok"_s).toBool() && record.value(u"action"_s) == u"attach"_s) {
            const auto handle = m_control->attachment(m_client);
            const bool bound = bind();
            if (!alive || !m_control || !m_connection) return {};
            if (bound && handle && attachmentMatches(*handle) && authorized()) {
                response.insert(u"audioPriority"_s, true);
                const auto topology = m_topologyResolve ? m_topologyResolve(*handle) : std::nullopt;
                if (!alive || !m_connection || !attachmentMatches(*handle)) return {};
                response.insert(u"virtualResize"_s, !m_topologyResolve || (topology && topology->outputs.size() == 1));
                return response;
            }
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
        if (m_mediaDispatch) return {};
        m_mediaDispatch = true;
        const auto endDispatch = qScopeGuard([alive] { if (alive) alive->m_mediaDispatch = false; });
        const auto endpoint = m_endpoint;
        const auto handle = m_handle;
        const auto generation = m_controlGeneration;
        const auto bindingCurrent = [alive, this, endpoint, handle, generation, uid] {
            return alive && endpoint && m_endpoint == endpoint && handle && m_handle
                && m_handle->id == handle->id && m_handle->manager == handle->manager
                && m_handle->generation == handle->generation && m_controlGeneration == generation
                && authorized(uid);
        };
        const auto playback = record.value(u"playback"_s);
        const auto microphone = record.value(u"microphone"_s);
        const auto camera = record.value(u"camera"_s);
        const auto silence = record.value(u"silenceHost"_s);
        const bool ownsDesktop = authorized(uid);
        const bool accepted = ownsDesktop && record.value(u"v"_s).isDouble() && record.value(u"v"_s).toDouble() == 1
            && playback.isBool() && microphone.isBool() && camera.isBool()
            && (silence.isUndefined() || silence.isBool()) && !camera.toBool();
        // A refused update must not report playback off while continuing the
        // previous stream. Revoke this connection even after ownership loss,
        // but never change a worker now belonging to another attachment.
        stopMicrophone(uid);
        if (!alive || !m_connection) return {};
        // Socket failure during source-off can synchronously revoke the binding.
        if (ownsDesktop && !bindingCurrent()) return mediaReply(false, u"virtual desktop authority changed"_s);
        const bool wasPriority = m_connection->audioPriorityActive();
        m_playback = accepted && playback.toBool();
        m_silenceHost = m_playback && silence.toBool();
        if (m_connection) {
            // setExternalAudioPlayback only updates atomics/queue state.
            m_connection->setExternalAudioPlayback(true);
            m_connection->setMediaPolicy(m_playback, false, false);
            if (!alive || !m_connection) return {};
        }
        if (ownsDesktop && m_endpoint) {
            m_endpoint->setMedia({m_playback, m_silenceHost});
            if (!alive || !m_connection) return {};
        }
        if (wasPriority) {
            restoreFixedVideoQuality(uid);
            if (!alive || !m_connection) return {};
        }
        if (!accepted) return mediaReply(false, u"media requires an authenticated attached owner; camera unavailable"_s);
        if (!bindingCurrent()) return mediaReply(false, u"virtual desktop authority changed"_s);
        if (!microphone.toBool()) return mediaReply(true);
        // Reserve one request ID for source-off; never wrap on enable/disable.
        if (!m_externalMicrophone || !m_controlGeneration
            || m_nextMicrophoneId >= std::numeric_limits<quint64>::max() - 1)
            return mediaReply(false, u"virtual microphone unavailable"_s);
        m_microphonePolicy = {m_controlGeneration, ++m_nextMicrophoneId, true};
        m_microphoneDeadline.start();
        const bool dispatched = m_endpoint->setMicrophone(m_microphonePolicy);
        if (!alive || !m_connection) return {};
        if (!bindingCurrent()) return {}; // A replaced binding cannot acknowledge this consent.
        if (!dispatched) {
            stopMicrophone();
            return alive ? mediaReply(false, u"cannot dispatch virtual microphone startup"_s) : QJsonObject{};
        }
        return {}; // Success is acknowledged only after correlated source readiness.
    }
    return {{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"code"_s, u"unsupported"_s},
            {u"message"_s, u"virtual-session transport does not support this request"_s}};
}
}
