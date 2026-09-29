// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionTransport.h"
#include "CursorTracker.h"
#include "LayoutControl.h"
#include "DeviceControl.h"
#include "AudioPriority.h"
#include "VirtualResizeProtocol.h"
#include "VirtualResize.h"
#include "RemoteMonitorGeometry.h"
#include "CodecRequest.h"
#include "StatsRequest.h"
#include <InputHandler.h>
#include <VideoStream.h>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QUuid>
#include <limits>

using namespace Qt::StringLiterals;

namespace KRdp
{
namespace
{
/** What `device` can do on a virtual desktop (DEVICES-DESIGN.md §3): no camera yet (risk 5). */
constexpr LayoutControl::DeviceCapabilities VirtualDeviceCapabilities{
    .playbackToggle = true,
    .playbackSilenceHost = true,
    .microphoneToggle = true,
    .cameraToggle = false,
    .cameraReselect = false,
};

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
    // Source-only private-compositor switch. Never enabled by a client record
    // or by the installed service's default environment.
    m_experimentalMultiResize = qEnvironmentVariable("FARSIDE_EXPERIMENTAL_MULTI_RESIZE") == u"1"_s;
    m_experimentalPrimary = qEnvironmentVariable("FARSIDE_EXPERIMENTAL_MULTI_PRIMARY") == u"1"_s;
    m_experimentalMixed = qEnvironmentVariable("FARSIDE_EXPERIMENTAL_MULTI_MIXED") == u"1"_s;
    connection->setAudioPriorityDefault(false);
    connection->clearAudioPriorityOverride();
    connection->videoStream()->setCodecPreference(CodecPreference::Avc420);
    connection->videoStream()->setQualityCap(80);
    connection->videoStream()->setAdaptiveQuality(false);
    connection->videoStream()->setEnabled(false);
    connection->setExternalAudioPlayback(true); // broker must never open host PipeWire
    m_externalMicrophone = connection->enableExternalMicrophone();
    connection->setDeviceEnabled(MediaDevice::Playback, false);
    connection->setDeviceEnabled(MediaDevice::Microphone, false);
    // A standard client resizes the desktop through MS-RDPEDISP (its window
    // size); ours uses `virtual-resize` and never joins that channel.
    connection->setDisplayControlEnabled(true);
    connect(connection, &RdpConnection::displayLayoutRequested, this, [this](const QList<VideoMonitor> &monitors) {
        if (m_connection) displayLayout(monitors, m_connection->authenticatedPamUid());
    }, Qt::QueuedConnection);
    // AUD-D4: the same 3 s first-record gate krdpserver has. A client that
    // sends no `virtual-session` record by then gets the broker's choice.
    m_stockGate.setSingleShot(true);
    m_stockGate.setInterval(3000);
    connect(&m_stockGate, &QTimer::timeout, this, [this] {
        if (m_krdpctlClient && !m_virtualSessionSeen) {
            // AUD-FIX2 F2: a KRDPCTL v2 client chooses its desktop itself (`virtual-session`
            // list, then attach or create); it is never bound for it, however long it takes.
            qInfo() << "Virtual client" << m_client << "speaks KRDPCTL but has not chosen a desktop yet; waiting for its virtual-session record";
            return;
        }
        if (!m_virtualSessionSeen) stockClientBind(m_stockUid);
    });
    m_displacedClose.setSingleShot(true);
    m_displacedClose.setInterval(250); // the session thread flushes the `session-end` record first
    connect(&m_displacedClose, &QTimer::timeout, this, [this] { refuseStockClient(VirtualStockClient::Refusal::Displaced); });
    m_stockWait.setInterval(250);
    connect(&m_stockWait, &QTimer::timeout, this, &VirtualSessionTransport::stockWaitTick);
    m_microphoneDeadline.setSingleShot(true);
    m_microphoneDeadline.setInterval(4000);
    connect(&m_microphoneDeadline, &QTimer::timeout, this, [this] {
        const QPointer<VirtualSessionTransport> alive(this);
        const auto reply = microphoneTimeout();
        if (alive && m_connection && !reply.isEmpty()) sendReply(reply);
    });
    m_microphonePump.setInterval(20);
    connect(&m_microphonePump, &QTimer::timeout, this, &VirtualSessionTransport::pumpMicrophone);
    m_resizeDeadline.setSingleShot(true);
    m_resizeDeadline.setInterval(60000);
    connect(&m_resizeDeadline, &QTimer::timeout, this, [this] {
        const auto response = resizeResult({m_resizeWorkerId, m_resizeGeneration,
            u"virtual resize timed out; refresh before retrying"_s},
            m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (m_connection && !response.isEmpty()) sendReply(response);
    });
    m_topologyDeadline.setSingleShot(true);
    m_topologyDeadline.setInterval(5000);
    connect(&m_topologyDeadline, &QTimer::timeout, this, [this] {
        const QString id = m_topologyId;
        clearTopology();
        if (m_connection && !id.isEmpty())
            sendReply(RemoteTopologyProtocol::error(id, u"timeout"_s));
    });
    m_positionDeadline.setSingleShot(true);
    m_positionDeadline.setInterval(15000);
    connect(&m_positionDeadline, &QTimer::timeout, this, [this] {
        if (m_positionId.isEmpty()) return;
        const QString id = m_positionId;
        clearPosition();
        if (m_connection) sendReply(RemoteTopologyProtocol::error(id, u"capture-failed"_s));
    });
    m_addDeadline.setSingleShot(true);
    m_addDeadline.setInterval(20000);
    connect(&m_addDeadline, &QTimer::timeout, this, [this] {
        if (m_addId.isEmpty()) return;
        const QString id = m_addId;
        clearAdd();
        if (m_connection) sendReply(RemoteTopologyProtocol::error(id, u"capture-failed"_s));
    });
    m_removeDeadline.setSingleShot(true);
    m_removeDeadline.setInterval(20000);
    connect(&m_removeDeadline, &QTimer::timeout, this, [this] {
        if (m_removeId.isEmpty()) return;
        const QString id = m_removeId;
        clearRemove();
        if (m_connection) sendReply(RemoteTopologyProtocol::error(id, u"capture-failed"_s));
    });
    m_topologyResizeDeadline.setSingleShot(true);
    m_topologyResizeDeadline.setInterval(20000);
    connect(&m_topologyResizeDeadline, &QTimer::timeout, this, [this] {
        if (m_topologyResizeId.isEmpty()) return;
        const QString id = m_topologyResizeId;
        clearTopologyResize();
        if (m_connection) sendReply(RemoteTopologyProtocol::error(id, u"capture-failed"_s));
    });
    connect(&m_session, &AbstractSession::frameReceived, connection->videoStream(), &VideoStream::queueFrame);
    // AUD-FIX7: the worker encodes what this connection's VideoStream decides (codec, settings,
    // frame rate - which also carries the delivery throttle for stock clients).
    m_codec = std::make_unique<WorkerCodecBridge>(connection->videoStream(), &m_session);
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
    // Emitted once the client authenticated (AUD-S1), with KRDPCTL open if it joined it.
    connect(connection, &RdpConnection::clientDisplayInfoReceived, this, [this] {
        const QPointer<VirtualSessionTransport> alive(this);
        sendCapabilities();
        if (alive && m_connection && m_connection->isAuthenticated())
            startStockGate(m_connection->hasControlChannel(), m_connection->authenticatedPamUid());
    }, Qt::QueuedConnection);
    // The connection's own channel work fails after the broker said `on`: the
    // client refused AUDIN, or never joined RDPSND. Pass it on.
    connect(connection, &RdpConnection::deviceState, this, [this](MediaDevice device, const DeviceStatus &status, const QString &) {
        if (status.state != DeviceStatus::State::Error || !m_connection) return;
        const QPointer<VirtualSessionTransport> alive(this);
        if (device == MediaDevice::Microphone && m_microphonePolicy.enabled) {
            const QString requestId = std::exchange(m_microphoneRequestId, {});
            stopMicrophone();
            if (alive) pushRecord(LayoutControl::withRequestId(deviceReply(device, status), requestId));
        } else if (device == MediaDevice::Playback && m_playback) {
            pushRecord(deviceReply(device, status));
        }
    }, Qt::QueuedConnection);
    connect(connection, &RdpConnection::stateChanged, this, [this](RdpConnection::State state) {
        if (state == RdpConnection::State::Closed) closed();
    }, Qt::QueuedConnection);
    connect(connection, &QObject::destroyed, this, [this] { closed(); });
}

VirtualSessionTransport::~VirtualSessionTransport() { closed(); }

void VirtualSessionTransport::setVideoCodecHost(const VideoCodecHost &host)
{
    m_videoHost = host;
    if (m_connection) {
        m_connection->videoStream()->setEncoderPolicy(host.probe.encoders, host.mode);
        m_connection->videoStream()->setAv1TilesSetting(host.av1Tiles);
    }
}

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
    return bind(m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
}

bool VirtualSessionTransport::bind(std::optional<quint32> uid)
{
    const QPointer<VirtualSessionTransport> alive(this);
    if (m_revoking || !m_control) return false;
    const auto handle = m_control->attachment(m_client);
    if (!handle) return false;
    const auto stillAttached = [alive, this, handle] {
        return alive && m_connection && attachmentMatches(*handle);
    };
    if (m_handle && m_handle->id == handle->id && m_handle->manager == handle->manager
        && m_handle->generation == handle->generation) return authorized(uid);
    // Resolution is host code and may destroy this (including its callback).
    const auto resolve = m_resolve;
    const QPointer<ConsoleWorkerEndpoint> endpoint = resolve ? resolve(*handle) : nullptr;
    if (!stillAttached()) return false;
    if (!endpoint || !endpoint->ready() || !uid || !*uid || endpoint->target().uid != *uid
        || endpoint->target().adapter != ConsoleSeat::Adapter::VirtualUser || endpoint->target().sessionId != handle->id
        || m_sequence == std::numeric_limits<quint64>::max()) return false;
    return activateBinding(*handle, endpoint, uid);
}

bool VirtualSessionTransport::activateBinding(const VirtualSessionRegistry::Handle &expected, QPointer<ConsoleWorkerEndpoint> endpoint)
{
    return activateBinding(expected, endpoint, m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
}

bool VirtualSessionTransport::activateBinding(const VirtualSessionRegistry::Handle &expected, QPointer<ConsoleWorkerEndpoint> endpoint,
    std::optional<quint32> uid)
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
        if (m_displaySize && frame.isKeyFrame) {
            applyDisplayLayout(m_connection->authenticatedPamUid());
            if (!alive || !m_connection) return;
        }
        const auto reply = topologyFrame(frame, m_connection->authenticatedPamUid());
        if (alive && m_connection && !reply.isEmpty()) sendReply(reply);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::audioReceived, this, [this](const auto &audio) {
        if (m_playback && authorized()) m_connection->submitExternalAudio(audio.pcm);
    }));
    // FIX-CURSOR: the desktop's cursor shape as RDP pointer updates; the current one right away
    // (a reattach, or a client arriving after the last change).
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::cursorShapeReceived, this, [this](const ConsoleWorkerWire::CursorShape &shape) {
        if (authorized()) CursorTracker::apply(*m_connection->cursor(), shape);
    }));
    if (const auto shape = endpoint->cursorShape(); shape && authorized()) CursorTracker::apply(*m_connection->cursor(), *shape);
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::workerStopped, this, [this] {
        unavailable(); // desktop remains supervised independently
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::microphoneFinished, this, [this](const auto &result) {
        const QPointer<VirtualSessionTransport> alive(this);
        const auto reply = microphoneResult(result, m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (alive && m_connection && !reply.isEmpty()) sendReply(reply);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::resizeFinished, this, [this](const auto &result) {
        const auto uid = m_connection ? m_connection->authenticatedPamUid() : std::nullopt;
        const auto response = m_topologyResizeId.isEmpty() ? resizeResult(result, uid) : topologyResizeResult(result, uid);
        if (m_connection && !response.isEmpty()) sendReply(response);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::positionFinished, this, [this](const auto &result) {
        const auto response = positionResult(result, m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (m_connection && !response.isEmpty()) sendReply(response);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::positionBatchFinished, this, [this](const auto &result) {
        const auto response = positionResult({result.requestId, result.generation, result.error},
            m_connection ? m_connection->authenticatedPamUid() : std::nullopt, true);
        if (m_connection && !response.isEmpty()) sendReply(response);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::managedFitFinished, this, [this](const auto &result) {
        const auto response = topologyResizeResult({result.requestId, result.generation, result.error},
            m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (m_connection && !response.isEmpty()) sendReply(response);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::primaryFinished, this, [this](const auto &result) {
        if (!m_topologyPrimaryPending) return;
        const auto response = topologyResizeResult({result.requestId, result.generation, result.error},
            m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (m_connection && !response.isEmpty()) sendReply(response);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::mixedFinished, this, [this](const auto &result) {
        if (!m_topologyMixedPending) return;
        const auto response = topologyResizeResult({result.requestId, result.generation, result.error},
            m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (m_connection && !response.isEmpty()) sendReply(response);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::mixedCreateFinished, this, [this](const auto &result) {
        if (!m_topologyMixedCreatePending) return;
        const auto response = topologyResizeResult({result.requestId, result.generation, result.error},
            m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (m_connection && !response.isEmpty()) sendReply(response);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::addVirtualFinished, this, [this](const auto &result) {
        const auto response = addVirtualResult(result, m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (m_connection && !response.isEmpty()) sendReply(response);
    }));
    m_workerConnections.append(connect(endpoint, &ConsoleWorkerEndpoint::removeVirtualFinished, this, [this](const auto &result) {
        const auto response = removeVirtualResult(result, m_connection ? m_connection->authenticatedPamUid() : std::nullopt);
        if (m_connection && !response.isEmpty()) sendReply(response);
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
    m_codec->bind(endpoint, generation); // the worker's own encoders, then this connection's config
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
    if (!bindingCurrent() || !authorized(uid)) return false;
    applyStandardMedia(uid);
    return bindingCurrent() && authorized(uid);
}

void VirtualSessionTransport::applyStandardMedia(std::optional<quint32> uid)
{
    if (m_deviceRecordSeen || m_spokeKrdpctl || !m_connection || !authorized(uid) || m_mediaDispatch) return;
    const auto channels = m_standardChannels ? m_standardChannels(m_connection) : m_connection->standardMediaChannels();
    if (!channels || !m_connection || !authorized(uid)) return;
    qInfo() << "StandardClientMedia: virtual client" << m_client << "playback" << channels->playback << "microphone" << channels->dynamic;
    const QPointer<VirtualSessionTransport> alive(this);
    if (channels->playback && !m_playback) {
        m_playback = true;
        m_silenceHost = false; // never silence the host without a request
        m_connection->setExternalAudioPlayback(true);
        m_connection->setDeviceEnabled(MediaDevice::Playback, true);
        m_endpoint->setMedia({m_playback, m_silenceHost});
        if (!alive || !m_connection || !authorized(uid)) return;
    }
    if (!channels->dynamic || m_microphonePolicy.enabled || !m_externalMicrophone || !m_controlGeneration
        || m_nextMicrophoneId >= std::numeric_limits<quint64>::max() - 1) return;
    m_microphonePolicy = {m_controlGeneration, ++m_nextMicrophoneId, true};
    m_microphoneRequestId.clear(); // nobody to answer: the client has no request
    m_microphoneDeadline.start();
    const bool dispatched = m_endpoint->setMicrophone(m_microphonePolicy);
    if (alive && !dispatched) stopMicrophone();
}

void VirtualSessionTransport::revoke()
{
    if (m_revoking) return;
    const QPointer<VirtualSessionTransport> alive(this);
    m_revoking = true;
    clearResize(); // Invalidate before any teardown callback can rebind.
    clearTopology();
    clearPosition();
    clearAdd();
    clearRemove();
    clearTopologyResize();
    m_preview.reset();
    // Clear preference before any teardown signal can destroy this transport.
    if (m_connection) {
        m_connection->setAudioPriorityDefault(false);
        m_connection->clearAudioPriorityOverride();
    }
    // A device that was on is pushed as `off`/`detached` once the teardown is
    // done; a microphone start still pending is answered with it (echo once).
    const bool microphoneWasOn = m_microphonePolicy.enabled;
    const QString microphoneRequestId = std::exchange(m_microphoneRequestId, {});
    const bool playbackWasOn = m_playback;
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
    if (m_codec) m_codec->unbind(); // the worker resets its encoders with the control change below
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
        m_connection->setDeviceEnabled(MediaDevice::Playback, false);
        m_connection->setDeviceEnabled(MediaDevice::Microphone, false);
        if (!alive) return;
        if (m_connection) m_connection->videoStream()->setEnabled(false); // discard old desktop frames
    }
    if (!alive) return;
    if (m_connection && m_connection->state() != RdpConnection::State::Closed) {
        const DeviceStatus revoked{DeviceStatus::State::Off, false, DeviceControl::Detached, u"the virtual desktop is no longer attached to this connection"_s};
        if (microphoneWasOn) pushRecord(LayoutControl::withRequestId(DeviceControl::stateRecord(MediaDevice::Microphone, revoked), microphoneRequestId));
        if (!alive) return;
        if (playbackWasOn) pushRecord(DeviceControl::stateRecord(MediaDevice::Playback, revoked));
        if (!alive) return;
    }
    m_revoking = false;
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

QJsonObject VirtualSessionTransport::deviceReply(MediaDevice device, const DeviceStatus &status) const
{
    return DeviceControl::stateRecord(device, status);
}

DeviceStatus VirtualSessionTransport::deviceStatus(MediaDevice device) const
{
    using State = DeviceStatus::State;
    switch (device) {
    case MediaDevice::Playback:
        return {m_playback ? State::On : State::Off, false, {}, {}};
    case MediaDevice::Microphone:
        return {m_microphoneReady ? State::On : m_microphonePolicy.enabled ? State::Starting : State::Off, false, {}, {}};
    case MediaDevice::Camera:
        break;
    }
    return {};
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
    // setDeviceEnabled updates consent atomics and clears the generation-bound
    // AUDIN queue; it does not emit signals. Playback remains independent.
    if (m_connection) m_connection->setDeviceEnabled(MediaDevice::Microphone, false);
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
    // The first result answers the pending `device` request; a later failure is unsolicited.
    if (!authorized(uid) || !result.error.isEmpty()) {
        const DeviceStatus status = result.error.isEmpty()
            ? DeviceStatus{DeviceStatus::State::Off, false, DeviceControl::Detached, u"this connection no longer holds the virtual desktop"_s}
            : DeviceStatus{DeviceStatus::State::Error, false, DeviceControl::Unavailable, result.error};
        const QString requestId = std::exchange(m_microphoneRequestId, {});
        stopMicrophone(uid);
        return alive ? LayoutControl::withRequestId(deviceReply(MediaDevice::Microphone, status), requestId) : QJsonObject{};
    }
    if (m_microphoneReady) return {};
    m_microphoneDeadline.stop();
    m_connection->setDeviceEnabled(MediaDevice::Microphone, true);
    m_microphoneReady = true;
    m_microphonePump.start();
    return LayoutControl::withRequestId(deviceReply(MediaDevice::Microphone, {DeviceStatus::State::On, false, {}, {}}),
                                        std::exchange(m_microphoneRequestId, {}));
}

QJsonObject VirtualSessionTransport::microphoneTimeout()
{
    if (!m_microphonePolicy.enabled || m_microphoneReady) return {};
    const QPointer<VirtualSessionTransport> alive(this);
    const QString requestId = std::exchange(m_microphoneRequestId, {});
    stopMicrophone();
    return alive ? LayoutControl::withRequestId(deviceReply(MediaDevice::Microphone,
                       {DeviceStatus::State::Error, false, DeviceControl::Timeout, u"virtual microphone worker startup timed out"_s}), requestId)
                 : QJsonObject{};
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
    if (!authorized()) {
        const QPointer<VirtualSessionTransport> alive(this);
        stopMicrophone();
        if (alive)
            pushRecord(deviceReply(MediaDevice::Microphone,
                {DeviceStatus::State::Off, false, DeviceControl::Detached, u"this connection no longer holds the virtual desktop"_s}));
        return;
    }
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

void VirtualSessionTransport::deliverControlRecord(const QJsonObject &incoming, std::optional<quint32> uid)
{
    const QPointer<VirtualSessionTransport> alive(this);
    const auto connection = m_connection;
    // KRDPCTL v2: the requestId comes off before the strict v1 parsers see the record and
    // goes back on the synchronous reply; an invalid one executes nothing.
    QJsonObject record = incoming;
    const auto requestId = LayoutControl::takeRequestId(record);
    if (requestId.invalid) {
        if (connection) connection->sendControlRecord(LayoutControl::invalidRequestIdRecord());
        return;
    }
    // Only a KRDPCTL v2 client sends a requestId (the contract requires one on every request).
    if (!requestId.value.isEmpty()) noteKrdpctlClient();
    const QString outerRequestId = std::exchange(m_replyRequestId, requestId.value);
    const auto response = request(record, uid);
    if (alive) m_replyRequestId = outerRequestId;
    if (alive && connection && m_connection == connection && !response.isEmpty())
        connection->sendControlRecord(LayoutControl::withRequestId(response, requestId.value));
}

void VirtualSessionTransport::noteKrdpctlClient()
{
    if (m_krdpctlClient) return;
    m_krdpctlClient = true;
    m_spokeKrdpctl = true; // StandardClientMedia is for stock clients only
    if (m_stockGate.isActive()) {
        qInfo() << "Virtual client" << m_client << "speaks KRDPCTL v2: no automatic desktop; it chooses one itself";
    }
}

void VirtualSessionTransport::startStockGate(bool controlChannel, std::optional<quint32> uid)
{
    if (m_stockStarted) return;
    m_stockStarted = true;
    m_stockUid = uid;
    if (m_virtualSessionSeen) return;
    if (controlChannel) {
        m_stockGate.start();
        return;
    }
    stockClientBind(uid);
}

void VirtualSessionTransport::stockClientBind(std::optional<quint32> uid)
{
    if (m_virtualSessionSeen || m_krdpctlClient || m_revoking || !m_control || !m_connection || !uid || !*uid) return;
    const QPointer<VirtualSessionTransport> alive(this);
    const auto policyOf = m_stockPolicy;
    const auto policy = policyOf ? policyOf(*uid) : VirtualStockClient::Policy::AttachOrCreate;
    if (!alive || !m_control || !m_connection) return;
    if (policy == VirtualStockClient::Policy::Refuse) {
        qInfo() << "Virtual stock client" << m_client << "(uid" << *uid << "): refused by VirtualStockClientPolicy";
        refuseStockClient(VirtualStockClient::Refusal::Policy);
        return;
    }
    const auto info = m_clientDisplayInfo ? m_clientDisplayInfo() : m_connection->clientDisplayInfo();
    const auto caps = m_control->initialLayoutCapabilities();
    VirtualStockClient::Limits limits{.maxOutputs = caps.maxOutputs};
    if (caps.maxOutputDimension > 0) limits.maxOutputDimension = caps.maxOutputDimension;
    if (caps.maxAtlasDimension > 0) limits.maxAtlasDimension = caps.maxAtlasDimension;
    const auto outputs = VirtualStockClient::initialOutputs(info, limits);
    const auto result = m_control->attachStockClient(uid, m_client, outputs);
    if (!alive) return;
    qInfo() << "Virtual stock client" << m_client << "(uid" << *uid << "): desktop" << result.session
            << (result.kind == VirtualSessionControl::StockResult::Kind::Attached ? "attached"
                : result.kind == VirtualSessionControl::StockResult::Kind::Starting ? "starting" : "refused")
            << "first layout" << outputs.size() << "output(s)" << (outputs.isEmpty() ? QSize() : outputs.first().pixels);
    stockResult(result, uid);
}

void VirtualSessionTransport::stockResult(const VirtualSessionControl::StockResult &result, std::optional<quint32> uid)
{
    using Kind = VirtualSessionControl::StockResult::Kind;
    using Reason = VirtualSessionControl::StockResult::Reason;
    switch (result.kind) {
    case Kind::Attached:
        m_stockWait.stop();
        stockAttached(uid);
        return;
    case Kind::Starting:
        m_stockSession = result.session;
        if (!m_stockWait.isActive()) {
            m_stockWaitAge.start();
            m_stockWait.start();
        }
        return;
    case Kind::Refused:
        m_stockWait.stop();
        refuseStockClient(result.reason == Reason::Limit ? VirtualStockClient::Refusal::NoFreeSlot
                          : result.reason == Reason::Maintenance ? VirtualStockClient::Refusal::Policy
                                                                 : VirtualStockClient::Refusal::StartFailed);
        return;
    }
}

void VirtualSessionTransport::stockWaitTick()
{
    if (m_virtualSessionSeen || !m_control || !m_connection || m_stockSession.isEmpty()) {
        m_stockWait.stop();
        return;
    }
    if (m_stockWaitAge.isValid() && m_stockWaitAge.elapsed() > m_stockWaitLimitMs) {
        m_stockWait.stop();
        qWarning() << "Virtual stock client" << m_client << ": desktop" << m_stockSession << "did not become ready in time";
        refuseStockClient(VirtualStockClient::Refusal::StartTimeout);
        return;
    }
    const QPointer<VirtualSessionTransport> alive(this);
    const auto result = m_control->attachStarted(m_stockUid, m_client, m_stockSession);
    if (alive) stockResult(result, m_stockUid);
}

void VirtualSessionTransport::stockAttached(std::optional<quint32> uid)
{
    const QPointer<VirtualSessionTransport> alive(this);
    const bool bound = bind(uid);
    if (!alive || !m_connection) return;
    if (!bound) {
        qWarning() << "Virtual stock client" << m_client << ": authenticated virtual capture unavailable";
        closed();
        if (alive) refuseStockClient(VirtualStockClient::Refusal::StartFailed);
        return;
    }
    applyDisplayLayout(uid);
}

void VirtualSessionTransport::refuseStockClient(VirtualStockClient::Refusal refusal)
{
    m_stockGate.stop();
    m_stockWait.stop();
    const quint32 code = VirtualStockClient::errorInfo(refusal);
    qWarning().noquote() << "Virtual client" << m_client << "closed with ERRINFO" << QStringLiteral("0x%1").arg(code, 8, 16, QLatin1Char('0'));
    const QPointer<VirtualSessionTransport> alive(this);
    if (m_refused) m_refused(code);
    if (alive && m_connection) m_connection->closeWithErrorInfo(code);
}

void VirtualSessionTransport::displaced()
{
    // Queued: the takeover that caused this is still on the stack.
    QTimer::singleShot(0, this, [this] {
        // AUD-FIX2 F4: the standard code (0x5, which FreeRDP words as "another user connected")
        // plus, for a KRDPCTL client, what really happened: the same user opened this desktop
        // from another device. The record goes out first; the close follows once it is sent.
        if (m_capabilitiesSent && m_connection) { // capabilities went out: it joined KRDPCTL
            pushRecord(LayoutControl::sessionEndRecord(u"opened-elsewhere"_s, VirtualStockClient::errorInfo(VirtualStockClient::Refusal::Displaced),
                                                       u"This desktop was opened from another device."_s));
            m_displacedClose.start();
            return;
        }
        refuseStockClient(VirtualStockClient::Refusal::Displaced);
    });
}

void VirtualSessionTransport::displayLayout(const QList<VideoMonitor> &monitors, std::optional<quint32> uid)
{
    const auto size = VirtualStockClient::displayControlSize(monitors);
    if (!size) {
        qInfo() << "Display Control: ignoring a layout of" << monitors.size() << "monitors (one-output desktops only)";
        return;
    }
    m_displaySize = size; // The latest window size wins; applied once the desktop is bound and idle.
    applyDisplayLayout(uid);
}

void VirtualSessionTransport::applyDisplayLayout(std::optional<quint32> uid)
{
    if (!m_displaySize || !authorized(uid) || !m_endpoint || !m_handle) return;
    if (!m_resizeId.isEmpty() || !m_topologyResizeId.isEmpty()) return; // retried when that one finishes
    double scale = 1.0;
    if (m_topologyResolve) {
        const QPointer<VirtualSessionTransport> alive(this);
        const auto topology = m_topologyResolve(*m_handle);
        if (!alive || !authorized(uid)) return;
        if (!topology) return; // not published yet: retried on the next keyframe
        if (topology->outputs.size() != 1) {
            qInfo() << "Display Control: the desktop has" << topology->outputs.size() << "outputs; resize it from the KRDP client";
            m_displaySize.reset();
            return;
        }
        const auto &output = topology->outputs.first().output;
        if (output.nativePixels == *m_displaySize) {
            m_displaySize.reset();
            return;
        }
        scale = VirtualResize::normalizedScale(output.scale);
    }
    const QSize target = *std::exchange(m_displaySize, std::nullopt);
    if (!VirtualResize::validRequest(target, scale) || m_nextResizeId == std::numeric_limits<quint64>::max()) return;
    m_resizeId = u"display-control-%1"_s.arg(++m_displayResizes);
    m_resizeFromDisplay = true;
    m_resizeWorkerId = ++m_nextResizeId;
    m_resizeGeneration = m_controlGeneration;
    const auto generation = m_resizeGeneration;
    const auto workerId = m_resizeWorkerId;
    m_resizeDeadline.start();
    qInfo() << "Display Control: resizing the virtual desktop to" << target << "scale" << scale;
    const QPointer<VirtualSessionTransport> alive(this);
    const bool dispatched = m_endpoint->resize({workerId, generation, u"virtual-desktop"_s, target, scale});
    if (!alive) return;
    if (!dispatched && m_resizeWorkerId == workerId) clearResize();
}

void VirtualSessionTransport::sendReply(const QJsonObject &record)
{
    if (!m_connection) return;
    // Asynchronous results carry the `id` of the request they answer; v2 clients send
    // requestId == id for those types, so it is echoed from there.
    const QJsonValue id = record.value(u"id"_s);
    m_connection->sendControlRecord(record.contains(u"requestId"_s) || !id.isString()
        ? record : LayoutControl::withRequestId(record, id.toString()));
}

void VirtualSessionTransport::pushRecord(const QJsonObject &record)
{
    if (!m_connection) return;
    if (m_recordPushed) m_recordPushed(record);
    if (m_connection) m_connection->sendControlRecord(record);
}

void VirtualSessionTransport::sendCapabilities()
{
    if (m_capabilitiesSent || !m_connection || !m_connection->isAuthenticated() || !m_connection->hasControlChannel()) return;
    m_capabilitiesSent = true;
    LayoutControl::ChannelCapabilities capabilities;
    capabilities.host = u"virtual"_s;
    capabilities.virtualList = true;
    capabilities.virtualCreate = true;
    capabilities.virtualSelectedCreate = m_control && m_control->selectedCreateAvailable();
    capabilities.topologyQuery = true;
    capabilities.topologyPreview = true;
    capabilities.topologyApply = true;
    capabilities.devices = VirtualDeviceCapabilities;
    if (m_videoHost) capabilities.video = EncoderSupport::videoCapabilities(m_videoHost->probe, m_videoHost->mode);
    capabilities.stats = LayoutControl::StatsCapabilities{};
    m_connection->sendControlRecord(LayoutControl::capabilitiesRecord(capabilities));
}

void VirtualSessionTransport::clearResize()
{
    m_resizeDeadline.stop();
    m_resizeId.clear();
    m_resizeWorkerId = 0;
    m_resizeGeneration = 0;
    m_resizeFromDisplay = false;
}

void VirtualSessionTransport::clearTopology()
{
    m_topologyDeadline.stop();
    m_topologyId.clear();
    m_topologyBinding = 0;
}

void VirtualSessionTransport::clearPosition()
{
    m_positionDeadline.stop();
    m_positionId.clear();
    m_positionWorkerId = 0;
    m_positionBinding = 0;
    m_positionGeneration.clear();
    m_positionRevision = 0;
    m_positionOutputId.clear();
    m_positionTarget = {};
    m_positionExpectedAfter.clear();
    m_positionExpectedChange = false;
    m_positionBatch = false;
}

void VirtualSessionTransport::clearAdd()
{
    m_addDeadline.stop();
    m_addId.clear();
    m_addWorkerId = 0;
    m_addBinding = 0;
    m_addGeneration.clear();
    m_addRevision = 0;
    m_addBackendKey.clear();
    m_addBefore.clear();
    m_addExpected = {};
}

void VirtualSessionTransport::clearRemove()
{
    m_removeDeadline.stop();
    m_removeId.clear();
    m_removeWorkerId = 0;
    m_removeBinding = 0;
    m_removeGeneration.clear();
    m_removeRevision = 0;
    m_removeBackendKey.clear();
    m_removeExpectedAfter.clear();
}

void VirtualSessionTransport::clearTopologyResize()
{
    m_topologyResizeDeadline.stop();
    m_topologyResizeDeadline.setInterval(20000);
    m_topologyResizeId.clear();
    m_topologyResizeWorkerId = 0;
    m_topologyResizeBinding = 0;
    m_topologyResizeGeneration.clear();
    m_topologyResizeRevision = 0;
    m_topologyResizeOutputId.clear();
    m_topologyResizeBackendKey.clear();
    m_topologyResizeExpectedAfter.clear();
    m_topologyResizeExpectedChange = false;
    m_topologyPrimaryPending = false;
    m_topologyMixedPending = false;
    m_topologyMixedCreatePending = false;
}

QJsonObject VirtualSessionTransport::topologyPreview(const QJsonObject &record, std::optional<quint32> uid)
{
    const auto parsed = RemoteTopologyProtocol::previewRequest(record);
    const auto id = record.value(u"id"_s).toString().left(64);
    if (!parsed) return RemoteTopologyProtocol::error(id, u"invalid"_s);
    if (!authorized(uid) || !m_handle) return RemoteTopologyProtocol::error(parsed->id, u"not-owner"_s);
    if (!m_topologyResolve || !m_endpoint) return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
    if (!m_positionId.isEmpty() || !m_addId.isEmpty() || !m_removeId.isEmpty()
        || !m_topologyResizeId.isEmpty() || !m_resizeId.isEmpty())
        return RemoteTopologyProtocol::error(parsed->id, u"busy"_s);
    const auto snapshot = m_topologyResolve(*m_handle);
    if (!snapshot) return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
    if (parsed->draft.generation != snapshot->generation) return RemoteTopologyProtocol::error(parsed->id, u"stale-generation"_s);
    if (parsed->draft.expectedRevision != snapshot->revision) return RemoteTopologyProtocol::error(parsed->id, u"stale-revision"_s);
    if (snapshot->outputs.size() < 2 || parsed->draft.operations.isEmpty()
        || parsed->draft.operations.size() > 16)
        return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
    const bool multiple = parsed->draft.operations.size() > 1;
    const bool movingBatch = multiple && std::all_of(parsed->draft.operations.cbegin(),
        parsed->draft.operations.cend(), [](const auto &candidate) {
            return candidate.kind == RemoteTopologyDraft::Operation::Kind::Move;
        });
    const bool mixed = multiple && !movingBatch;
    const bool mixedCreate = mixed && std::any_of(parsed->draft.operations.cbegin(),
        parsed->draft.operations.cend(), [](const auto &candidate) {
            return candidate.kind == RemoteTopologyDraft::Operation::Kind::AddVirtual;
        });
    const auto &operation = parsed->draft.operations.first();
    if (movingBatch) {
        if (parsed->draft.operations.size() > snapshot->outputs.size())
            return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
        QSet<QString> ids;
        for (const auto &move : parsed->draft.operations) {
            if (move.kind != RemoteTopologyDraft::Operation::Kind::Move || ids.contains(move.id)
                || move.position.x() < 0 || move.position.y() < 0)
                return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
            ids.insert(move.id);
        }
    }
    if (mixed) {
        if (!m_experimentalMixed || parsed->draft.allowRemoval || parsed->draft.allowPhysicalChange)
            return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
        if (mixedCreate && (snapshot->outputs.size() >= 16
            || std::any_of(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [this](const auto &candidate) {
                return candidate.output.physical || !m_handle || candidate.output.owner != m_handle->id;
            }))) return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
        QSet<QString> operationKeys;
        bool primarySeen = false;
        QString temporaryId;
        for (const auto &change : parsed->draft.operations) {
            if (change.kind == RemoteTopologyDraft::Operation::Kind::AddVirtual) {
                if (!mixedCreate || !temporaryId.isEmpty() || !change.id.startsWith(u"new:"_s)
                    || !VirtualResize::validRequest(change.pixels, change.scale)
                    || change.position.x() < 0 || change.position.y() < 0)
                    return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
                temporaryId = change.id;
                continue;
            }
            const bool newPrimary = mixedCreate && change.kind == RemoteTopologyDraft::Operation::Kind::SetPrimary
                && change.id == temporaryId;
            const auto target = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&change](const auto &candidate) {
                return candidate.id == change.id;
            });
            if (!newPrimary && (target == snapshot->outputs.cend() || target->output.physical
                || target->output.owner != m_handle->id || target->output.backendKey.isEmpty()))
                return RemoteTopologyProtocol::error(parsed->id, u"stale-output"_s);
            const QString key = QString::number(int(change.kind)) + u":"_s + change.id;
            if (operationKeys.contains(key)) return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
            operationKeys.insert(key);
            switch (change.kind) {
            case RemoteTopologyDraft::Operation::Kind::Move:
                if (change.position.x() < 0 || change.position.y() < 0)
                    return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
                break;
            case RemoteTopologyDraft::Operation::Kind::Resize:
                if (!m_experimentalMultiResize || !VirtualResize::validRequest(change.pixels, change.scale))
                    return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
                break;
            case RemoteTopologyDraft::Operation::Kind::SetPrimary:
                if (!m_experimentalPrimary || primarySeen)
                    return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
                primarySeen = true;
                break;
            case RemoteTopologyDraft::Operation::Kind::AddVirtual:
            case RemoteTopologyDraft::Operation::Kind::Remove:
                return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
            }
        }
        if (mixedCreate && temporaryId.isEmpty()) return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
    }
    const bool adding = operation.kind == RemoteTopologyDraft::Operation::Kind::AddVirtual;
    const bool removing = operation.kind == RemoteTopologyDraft::Operation::Kind::Remove;
    const bool resizing = operation.kind == RemoteTopologyDraft::Operation::Kind::Resize;
    const bool selectingPrimary = operation.kind == RemoteTopologyDraft::Operation::Kind::SetPrimary;
    if (!adding && !removing && !resizing && !selectingPrimary
        && operation.kind != RemoteTopologyDraft::Operation::Kind::Move)
        return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
    if (selectingPrimary && (movingBatch || !m_experimentalPrimary))
        return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
    if (resizing && (!m_experimentalMultiResize || !VirtualResize::validRequest(operation.pixels, operation.scale)))
        return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
    // This KWin backend normalizes negative workspace origins. A preview may
    // not promise a literal negative position it cannot read back afterward.
    if (!resizing && (operation.position.x() < 0 || operation.position.y() < 0))
        return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
    auto draft = parsed->draft;
    draft.owner = m_handle->id; // Never trust caller-provided ownership.
    const RemoteTopologyDraft::Capabilities caps{.addVirtual = true, .removeVirtual = true, .moveVirtual = true,
        .resizeVirtual = m_experimentalMultiResize, .changePrimary = m_experimentalPrimary, .maxOutputs = 16,
        .maxOutputDimension = 4096, .maxAtlasDimension = 8192};
    const auto entry = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&operation](const auto &candidate) {
        return candidate.id == operation.id;
    });
    if (removing && (!parsed->draft.allowRemoval || snapshot->outputs.size() <= 2
        || entry == snapshot->outputs.cend() || !entry->output.backendKey.startsWith(u"Virtual-krdp-added-"_s)))
        return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
    const auto proposal = RemoteTopologyDraft::preview(*snapshot, caps, draft);
    if (!proposal.valid()) return RemoteTopologyProtocol::error(parsed->id, proposal.error);
    if (adding && (operation.pixels.width() < 320 || operation.pixels.height() < 200
        || operation.scale < 1.0 || operation.scale > 4.0))
        return RemoteTopologyProtocol::error(parsed->id, u"limit"_s);
    if (!adding && (entry == snapshot->outputs.cend() || entry->output.backendKey.isEmpty()))
        return RemoteTopologyProtocol::error(parsed->id, u"stale-output"_s);
    if (selectingPrimary && std::any_of(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [this](const auto &candidate) {
        return candidate.output.physical || !m_handle || candidate.output.owner != m_handle->id;
    })) return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
    if (m_preview && m_preview->id == parsed->id && m_preview->generation == snapshot->generation
        && m_preview->revision == snapshot->revision && m_preview->before == snapshot->outputs
        && m_preview->operations == parsed->draft.operations
        && m_preview->age.isValid() && m_preview->age.elapsed() < RemoteTopologyProtocol::PreviewLifetimeMs)
        return RemoteTopologyProtocol::previewReply(parsed->id, m_preview->token,
            {m_preview->before, m_preview->after, {}}, *snapshot);
    PendingPreview next;
    next.id = parsed->id;
    next.token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    next.generation = snapshot->generation;
    next.revision = snapshot->revision;
    const auto newOperation = mixedCreate
        ? std::find_if(parsed->draft.operations.cbegin(), parsed->draft.operations.cend(), [](const auto &candidate) {
            return candidate.kind == RemoteTopologyDraft::Operation::Kind::AddVirtual;
        }) : parsed->draft.operations.cend();
    const auto &selected = newOperation == parsed->draft.operations.cend() ? operation : *newOperation;
    next.outputId = selected.id;
    next.backendKey = (adding || mixedCreate)
        ? u"Virtual-krdp-added-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces)
        : entry->output.backendKey;
    next.kind = selected.kind;
    next.position = selected.position;
    next.pixels = selected.pixels;
    next.scale = selected.scale;
    next.operations = parsed->draft.operations;
    next.before = proposal.before;
    next.after = proposal.after;
    next.mixedCreate = mixedCreate;
    if (movingBatch) {
        for (const auto &move : parsed->draft.operations) {
            const auto target = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&move](const auto &candidate) {
                return candidate.id == move.id;
            });
            if (target == snapshot->outputs.cend() || target->output.physical
                || target->output.owner != m_handle->id || target->output.backendKey.isEmpty())
                return RemoteTopologyProtocol::error(parsed->id, u"stale-output"_s);
            next.batchTargets.append({target->output.backendKey, move.position});
        }
    }
    if (mixed) {
        for (const auto &change : parsed->draft.operations) {
            if (change.kind == RemoteTopologyDraft::Operation::Kind::AddVirtual) continue;
            const bool newPrimary = mixedCreate && change.kind == RemoteTopologyDraft::Operation::Kind::SetPrimary
                && change.id == next.outputId;
            const auto target = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&change](const auto &candidate) {
                return candidate.id == change.id;
            });
            if (!newPrimary && target == snapshot->outputs.cend())
                return RemoteTopologyProtocol::error(parsed->id, u"stale-output"_s);
            ConsoleWorkerWire::MixedOperation wire;
            wire.output = newPrimary ? next.backendKey : target->output.backendKey;
            switch (change.kind) {
            case RemoteTopologyDraft::Operation::Kind::Move:
                wire.kind = ConsoleWorkerWire::MixedOperation::Kind::Move;
                wire.globalLogical = change.position;
                break;
            case RemoteTopologyDraft::Operation::Kind::Resize:
                wire.kind = ConsoleWorkerWire::MixedOperation::Kind::Resize;
                wire.pixels = change.pixels;
                wire.scale = change.scale;
                break;
            case RemoteTopologyDraft::Operation::Kind::SetPrimary:
                wire.kind = ConsoleWorkerWire::MixedOperation::Kind::Primary;
                break;
            case RemoteTopologyDraft::Operation::Kind::AddVirtual:
            case RemoteTopologyDraft::Operation::Kind::Remove:
                return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
            }
            next.mixedOperations.append(wire);
        }
    }
    if (adding || mixedCreate) {
        const auto created = std::find_if(next.after.begin(), next.after.end(), [&next](const auto &candidate) {
            return candidate.id == next.outputId;
        });
        if (created == next.after.end()) return RemoteTopologyProtocol::error(parsed->id, u"invalid"_s);
        created->output.backendKey = next.backendKey;
        created->output.name = next.backendKey;
    }
    next.age.start();
    m_preview = std::move(next);
    return RemoteTopologyProtocol::previewReply(parsed->id, m_preview->token,
        {m_preview->before, m_preview->after, {}}, *snapshot);
}

QJsonObject VirtualSessionTransport::topologyFitPreview(const QJsonObject &record, std::optional<quint32> uid)
{
    const auto parsed = RemoteTopologyProtocol::fitPreviewRequest(record);
    const auto id = record.value(u"id"_s).toString().left(64);
    if (!parsed) return RemoteTopologyProtocol::error(id, u"invalid"_s);
    if (!authorized(uid) || !m_handle) return RemoteTopologyProtocol::error(parsed->id, u"not-owner"_s);
    if (!m_experimentalMultiResize || !m_topologyResolve || !m_endpoint)
        return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
    if (!m_positionId.isEmpty() || !m_addId.isEmpty() || !m_removeId.isEmpty()
        || !m_topologyResizeId.isEmpty() || !m_resizeId.isEmpty())
        return RemoteTopologyProtocol::error(parsed->id, u"busy"_s);
    const auto snapshot = m_topologyResolve(*m_handle);
    if (!snapshot) return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
    if (parsed->generation != snapshot->generation) return RemoteTopologyProtocol::error(parsed->id, u"stale-generation"_s);
    if (parsed->expectedRevision != snapshot->revision) return RemoteTopologyProtocol::error(parsed->id, u"stale-revision"_s);
    RemoteTopologyDraft::Request base;
    base.generation = snapshot->generation;
    base.expectedRevision = snapshot->revision;
    base.owner = m_handle->id;
    const RemoteTopologyDraft::Capabilities caps{.moveVirtual = true, .resizeVirtual = true,
        .maxOutputs = 16, .maxOutputDimension = 4096, .maxAtlasDimension = 8192};
    const auto fit = RemoteTopologyFit::plan(*snapshot, caps, base, parsed->output,
        parsed->pixels, parsed->scale, parsed->relations);
    if (!fit.valid()) return RemoteTopologyProtocol::error(parsed->id, fit.error);
    const auto selected = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&parsed](const auto &entry) {
        return entry.id == parsed->output;
    });
    if (selected == snapshot->outputs.cend() || selected->output.physical || selected->output.owner != m_handle->id
        || selected->output.backendKey.isEmpty()) return RemoteTopologyProtocol::error(parsed->id, u"stale-output"_s);
    PendingPreview next;
    next.id = parsed->id;
    next.token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    next.generation = snapshot->generation;
    next.revision = snapshot->revision;
    next.outputId = parsed->output;
    next.backendKey = selected->output.backendKey;
    next.kind = RemoteTopologyDraft::Operation::Kind::Resize;
    next.pixels = parsed->pixels;
    next.scale = parsed->scale;
    next.operations = fit.request.operations;
    next.before = fit.proposal.before;
    next.after = fit.proposal.after;
    next.managedFit = true;
    const auto backendKey = [snapshot](const QString &stableId) -> QString {
        const auto found = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&stableId](const auto &entry) {
            return entry.id == stableId;
        });
        return found == snapshot->outputs.cend() ? QString{} : found->output.backendKey;
    };
    for (const auto &relation : parsed->relations) {
        const auto parent = backendKey(relation.parent);
        const auto child = backendKey(relation.child);
        if (parent.isEmpty() || child.isEmpty()) return RemoteTopologyProtocol::error(parsed->id, u"stale-output"_s);
        next.fitRelations.append({parent, child, quint8(relation.edge), relation.offset});
    }
    next.age.start();
    m_preview = std::move(next);
    return RemoteTopologyProtocol::previewReply(parsed->id, m_preview->token,
        {m_preview->before, m_preview->after, {}}, *snapshot);
}

QJsonObject VirtualSessionTransport::topologyCommit(const QJsonObject &record, std::optional<quint32> uid)
{
    const auto parsed = RemoteTopologyProtocol::commitRequest(record);
    const auto id = record.value(u"id"_s).toString().left(64);
    if (!parsed) return RemoteTopologyProtocol::error(id, u"invalid"_s);
    if (!authorized(uid) || !m_handle) return RemoteTopologyProtocol::error(parsed->id, u"not-owner"_s);
    if (m_positionId == parsed->id || m_addId == parsed->id || m_removeId == parsed->id
        || m_topologyResizeId == parsed->id) return {};
    if (!m_positionId.isEmpty() || !m_addId.isEmpty() || !m_removeId.isEmpty()
        || !m_topologyResizeId.isEmpty() || !m_resizeId.isEmpty())
        return RemoteTopologyProtocol::error(parsed->id, u"busy"_s);
    if (!m_preview || !m_preview->age.isValid() || m_preview->age.elapsed() >= RemoteTopologyProtocol::PreviewLifetimeMs
        || parsed->id != m_preview->id || parsed->token != m_preview->token)
        return RemoteTopologyProtocol::error(parsed->id, u"invalid"_s);
    const auto proposal = *m_preview;
    m_preview.reset(); // One-use token, consumed before any dispatch/reentrant callback.
    if (parsed->generation != proposal.generation) return RemoteTopologyProtocol::error(parsed->id, u"stale-generation"_s);
    if (parsed->expectedRevision != proposal.revision) return RemoteTopologyProtocol::error(parsed->id, u"stale-revision"_s);
    const auto snapshot = m_topologyResolve ? m_topologyResolve(*m_handle) : std::nullopt;
    if (!snapshot) return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
    if (snapshot->generation != proposal.generation) return RemoteTopologyProtocol::error(parsed->id, u"stale-generation"_s);
    if (snapshot->revision != proposal.revision) return RemoteTopologyProtocol::error(parsed->id, u"stale-revision"_s);
    if (snapshot->outputs != proposal.before) return RemoteTopologyProtocol::error(parsed->id, u"stale-revision"_s);
    if (proposal.mixedCreate) {
        const auto created = std::find_if(proposal.after.cbegin(), proposal.after.cend(), [&proposal](const auto &entry) {
            return entry.id == proposal.outputId;
        });
        if (!m_experimentalMixed || !m_endpoint || m_nextResizeId == std::numeric_limits<quint64>::max()
            || proposal.after.size() != proposal.before.size() + 1
            || proposal.mixedOperations.isEmpty() || proposal.mixedOperations.size() > 15
            || created == proposal.after.cend() || created->output.backendKey != proposal.backendKey
            || created->output.owner != m_handle->id || created->output.physical
            || !VirtualResize::validRequest(proposal.pixels, proposal.scale))
            return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
        m_topologyResizeId = parsed->id;
        m_topologyResizeWorkerId = ++m_nextResizeId;
        m_topologyResizeBinding = m_controlGeneration;
        m_topologyResizeGeneration = proposal.generation;
        m_topologyResizeRevision = proposal.revision;
        m_topologyResizeOutputId = proposal.outputId;
        m_topologyResizeBackendKey = proposal.backendKey;
        m_topologyResizeExpectedAfter = proposal.after;
        m_topologyResizeExpectedChange = true;
        m_topologyMixedCreatePending = true;
        m_topologyResizeDeadline.start(40000); // Worker has 30 s for creation, apply and recapture.
        const QPointer<VirtualSessionTransport> alive(this);
        const bool sent = m_endpoint->mixedCreate({m_topologyResizeWorkerId, m_topologyResizeBinding,
            proposal.backendKey, proposal.pixels, proposal.scale, proposal.position, proposal.mixedOperations});
        if (!alive) return {};
        if (!sent && m_topologyResizeId == parsed->id) {
            clearTopologyResize();
            return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
        }
        return {};
    }
    if (!proposal.mixedOperations.isEmpty()) {
        if (!m_experimentalMixed || !m_endpoint || m_nextResizeId == std::numeric_limits<quint64>::max()
            || proposal.mixedOperations.size() < 2 || proposal.mixedOperations.size() > 16
            || proposal.after.size() != proposal.before.size())
            return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
        m_topologyResizeId = parsed->id;
        m_topologyResizeWorkerId = ++m_nextResizeId;
        m_topologyResizeBinding = m_controlGeneration;
        m_topologyResizeGeneration = proposal.generation;
        m_topologyResizeRevision = proposal.revision;
        m_topologyResizeOutputId = proposal.outputId;
        m_topologyResizeExpectedAfter = proposal.after;
        m_topologyResizeExpectedChange = proposal.before != proposal.after;
        m_topologyMixedPending = true;
        m_topologyResizeDeadline.start();
        const QPointer<VirtualSessionTransport> alive(this);
        const bool sent = m_endpoint->mixed({m_topologyResizeWorkerId, m_topologyResizeBinding,
            proposal.mixedOperations});
        if (!alive) return {};
        if (!sent && m_topologyResizeId == parsed->id) {
            clearTopologyResize();
            return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
        }
        return {};
    }
    if (proposal.managedFit) {
        if (!m_experimentalMultiResize || !m_endpoint || m_nextResizeId == std::numeric_limits<quint64>::max()
            || proposal.after.size() != proposal.before.size() || !VirtualResize::validRequest(proposal.pixels, proposal.scale))
            return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
        m_topologyResizeId = parsed->id;
        m_topologyResizeWorkerId = ++m_nextResizeId;
        m_topologyResizeBinding = m_controlGeneration;
        m_topologyResizeGeneration = proposal.generation;
        m_topologyResizeRevision = proposal.revision;
        m_topologyResizeOutputId = proposal.outputId;
        m_topologyResizeExpectedAfter = proposal.after;
        m_topologyResizeExpectedChange = proposal.before != proposal.after;
        m_topologyResizeDeadline.start();
        const QPointer<VirtualSessionTransport> alive(this);
        const bool sent = m_endpoint->managedFit({m_topologyResizeWorkerId, m_topologyResizeBinding,
            proposal.backendKey, proposal.pixels, proposal.scale, proposal.fitRelations});
        if (!alive) return {};
        if (!sent && m_topologyResizeId == parsed->id) {
            clearTopologyResize();
            return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
        }
        return {};
    }
    if (proposal.kind == RemoteTopologyDraft::Operation::Kind::SetPrimary) {
        if (!m_experimentalPrimary || !m_endpoint || m_nextResizeId == std::numeric_limits<quint64>::max()
            || proposal.after.size() != proposal.before.size())
            return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
        m_topologyResizeId = parsed->id;
        m_topologyResizeWorkerId = ++m_nextResizeId;
        m_topologyResizeBinding = m_controlGeneration;
        m_topologyResizeGeneration = proposal.generation;
        m_topologyResizeRevision = proposal.revision;
        m_topologyResizeOutputId = proposal.outputId;
        m_topologyResizeExpectedAfter = proposal.after;
        m_topologyResizeExpectedChange = proposal.before != proposal.after;
        m_topologyPrimaryPending = true;
        m_topologyResizeDeadline.start();
        const QPointer<VirtualSessionTransport> alive(this);
        const bool sent = m_endpoint->primary({m_topologyResizeWorkerId, m_topologyResizeBinding, proposal.backendKey});
        if (!alive) return {};
        if (!sent && m_topologyResizeId == parsed->id) {
            clearTopologyResize();
            return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
        }
        return {};
    }
    if (!proposal.batchTargets.isEmpty()) {
        if (proposal.kind != RemoteTopologyDraft::Operation::Kind::Move || !m_endpoint
            || proposal.batchTargets.size() < 2 || proposal.batchTargets.size() > snapshot->outputs.size()
            || proposal.after.size() != proposal.before.size()
            || m_nextPositionId == std::numeric_limits<quint64>::max())
            return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
        m_positionId = parsed->id;
        m_positionWorkerId = ++m_nextPositionId;
        m_positionBinding = m_controlGeneration;
        m_positionGeneration = proposal.generation;
        m_positionRevision = proposal.revision;
        m_positionExpectedAfter = proposal.after;
        m_positionExpectedChange = proposal.before != proposal.after;
        m_positionBatch = true;
        m_positionDeadline.start();
        const QPointer<VirtualSessionTransport> alive(this);
        const bool sent = m_endpoint->positionBatch({m_positionWorkerId, m_positionBinding, proposal.batchTargets});
        if (!alive) return {};
        if (!sent && m_positionId == parsed->id) {
            clearPosition();
            return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
        }
        return {};
    }
    if (proposal.kind == RemoteTopologyDraft::Operation::Kind::AddVirtual) {
        if (!m_endpoint || m_nextAddId == std::numeric_limits<quint64>::max()
            || proposal.after.size() != proposal.before.size() + 1)
            return RemoteTopologyProtocol::error(parsed->id, u"limit"_s);
        const auto &created = proposal.after.last().output;
        if (created.backendKey != proposal.backendKey || created.owner != m_handle->id || created.physical)
            return RemoteTopologyProtocol::error(parsed->id, u"invalid"_s);
        m_addId = parsed->id;
        m_addWorkerId = ++m_nextAddId;
        m_addBinding = m_controlGeneration;
        m_addGeneration = proposal.generation;
        m_addRevision = proposal.revision;
        m_addBackendKey = proposal.backendKey;
        m_addBefore = proposal.before;
        m_addExpected = created;
        m_addDeadline.start();
        const QPointer<VirtualSessionTransport> alive(this);
        const bool sent = m_endpoint->addVirtual({m_addWorkerId, m_addBinding, proposal.backendKey,
            proposal.pixels, proposal.scale, proposal.position});
        if (!alive) return {};
        if (!sent && m_addId == parsed->id) {
            clearAdd();
            return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
        }
        return {};
    }
    if (proposal.kind == RemoteTopologyDraft::Operation::Kind::Remove) {
        if (!m_endpoint || m_nextRemoveId == std::numeric_limits<quint64>::max()
            || proposal.before.size() <= 2 || proposal.after.size() + 1 != proposal.before.size()
            || !proposal.backendKey.startsWith(u"Virtual-krdp-added-"_s))
            return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
        const auto removed = std::find_if(proposal.before.cbegin(), proposal.before.cend(), [&proposal](const auto &entry) {
            return entry.id == proposal.outputId && entry.output.backendKey == proposal.backendKey;
        });
        if (removed == proposal.before.cend() || removed->output.physical || removed->output.owner != m_handle->id)
            return RemoteTopologyProtocol::error(parsed->id, u"stale-output"_s);
        m_removeId = parsed->id;
        m_removeWorkerId = ++m_nextRemoveId;
        m_removeBinding = m_controlGeneration;
        m_removeGeneration = proposal.generation;
        m_removeRevision = proposal.revision;
        m_removeBackendKey = proposal.backendKey;
        m_removeExpectedAfter = proposal.after;
        m_removeDeadline.start();
        const QPointer<VirtualSessionTransport> alive(this);
        const bool sent = m_endpoint->removeVirtual({m_removeWorkerId, m_removeBinding, proposal.backendKey});
        if (!alive) return {};
        if (!sent && m_removeId == parsed->id) {
            clearRemove();
            return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
        }
        return {};
    }
    if (proposal.kind == RemoteTopologyDraft::Operation::Kind::Resize) {
        if (!m_experimentalMultiResize || !m_endpoint || m_nextResizeId == std::numeric_limits<quint64>::max()
            || proposal.after.size() != proposal.before.size()
            || !VirtualResize::validRequest(proposal.pixels, proposal.scale))
            return RemoteTopologyProtocol::error(parsed->id, u"unsupported"_s);
        const auto found = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&proposal](const auto &entry) {
            return entry.id == proposal.outputId;
        });
        if (found == snapshot->outputs.cend() || found->output.backendKey != proposal.backendKey
            || found->output.owner != m_handle->id || found->output.physical)
            return RemoteTopologyProtocol::error(parsed->id, u"stale-output"_s);
        m_topologyResizeId = parsed->id;
        m_topologyResizeWorkerId = ++m_nextResizeId;
        m_topologyResizeBinding = m_controlGeneration;
        m_topologyResizeGeneration = proposal.generation;
        m_topologyResizeRevision = proposal.revision;
        m_topologyResizeOutputId = proposal.outputId;
        m_topologyResizeExpectedAfter = proposal.after;
        m_topologyResizeExpectedChange = proposal.before != proposal.after;
        m_topologyResizeDeadline.start();
        const QPointer<VirtualSessionTransport> alive(this);
        const bool sent = m_endpoint->resize({m_topologyResizeWorkerId, m_topologyResizeBinding,
            proposal.backendKey, proposal.pixels, proposal.scale});
        if (!alive) return {};
        if (!sent && m_topologyResizeId == parsed->id) {
            clearTopologyResize();
            return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
        }
        return {};
    }
    const auto found = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&proposal](const auto &entry) {
        return entry.id == proposal.outputId;
    });
    if (found == snapshot->outputs.cend() || found->output.backendKey != proposal.backendKey
        || found->output.owner != m_handle->id || found->output.physical)
        return RemoteTopologyProtocol::error(parsed->id, u"stale-output"_s);
    if (m_nextPositionId == std::numeric_limits<quint64>::max())
        return RemoteTopologyProtocol::error(parsed->id, u"limit"_s);
    m_positionId = parsed->id;
    m_positionWorkerId = ++m_nextPositionId;
    m_positionBinding = m_controlGeneration;
    m_positionGeneration = proposal.generation;
    m_positionRevision = proposal.revision;
    m_positionOutputId = proposal.outputId;
    m_positionTarget = proposal.position;
    m_positionExpectedAfter = proposal.after;
    m_positionExpectedChange = proposal.before != proposal.after;
    m_positionDeadline.start();
    const QPointer<VirtualSessionTransport> alive(this);
    const bool sent = m_endpoint && m_endpoint->position({m_positionWorkerId, m_positionBinding,
        proposal.backendKey, proposal.position});
    if (!alive) return {};
    if (!sent && m_positionId == parsed->id) {
        clearPosition();
        return RemoteTopologyProtocol::error(parsed->id, u"capture-failed"_s);
    }
    return {};
}

QJsonObject VirtualSessionTransport::addVirtualResult(const ConsoleWorkerWire::AddVirtualResult &result,
    std::optional<quint32> uid)
{
    if (m_addId.isEmpty() || result.requestId != m_addWorkerId || result.generation != m_addBinding) return {};
    const QString id = m_addId;
    const QString generation = m_addGeneration;
    const quint64 revision = m_addRevision;
    const QString backendKey = m_addBackendKey;
    const auto before = m_addBefore;
    const auto expected = m_addExpected;
    const bool current = authorized(uid) && m_handle && m_controlGeneration == result.generation;
    clearAdd();
    if (!current) return {};
    if (!result.error.isEmpty()) return RemoteTopologyProtocol::error(id, u"partial"_s);
    const auto snapshot = m_topologyResolve ? m_topologyResolve(*m_handle) : std::nullopt;
    if (!snapshot || snapshot->generation != generation) return RemoteTopologyProtocol::error(id, u"capture-failed"_s);
    if (snapshot->revision != revision + 1 || snapshot->outputs.size() != before.size() + 1)
        return RemoteTopologyProtocol::error(id, u"partial"_s);
    QSet<QString> oldIds;
    for (const auto &entry : before) {
        oldIds.insert(entry.id);
        const auto found = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&entry](const auto &candidate) {
            return candidate.id == entry.id;
        });
        if (found == snapshot->outputs.cend() || *found != entry)
            return RemoteTopologyProtocol::error(id, u"partial"_s);
    }
    const auto added = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&backendKey](const auto &entry) {
        return entry.output.backendKey == backendKey;
    });
    if (added == snapshot->outputs.cend() || oldIds.contains(added->id) || added->output != expected)
        return RemoteTopologyProtocol::error(id, u"partial"_s);
    return {{u"type"_s, u"topology-result"_s}, {u"v"_s, 1}, {u"id"_s, id}, {u"ok"_s, true},
        {u"topology"_s, RemoteTopologyProtocol::retainedReadOnly(id, *snapshot, m_experimentalMultiResize, m_experimentalPrimary, m_experimentalMixed)}};
}

QJsonObject VirtualSessionTransport::removeVirtualResult(const ConsoleWorkerWire::RemoveVirtualResult &result,
    std::optional<quint32> uid)
{
    if (m_removeId.isEmpty() || result.requestId != m_removeWorkerId || result.generation != m_removeBinding) return {};
    const QString id = m_removeId;
    const QString generation = m_removeGeneration;
    const quint64 revision = m_removeRevision;
    const QString backendKey = m_removeBackendKey;
    const auto expectedAfter = m_removeExpectedAfter;
    const bool current = authorized(uid) && m_handle && m_controlGeneration == result.generation;
    clearRemove();
    if (!current) return {};
    if (!result.error.isEmpty()) return RemoteTopologyProtocol::error(id, u"partial"_s);
    const auto snapshot = m_topologyResolve ? m_topologyResolve(*m_handle) : std::nullopt;
    if (!snapshot || snapshot->generation != generation) return RemoteTopologyProtocol::error(id, u"capture-failed"_s);
    if (snapshot->revision != revision + 1 || snapshot->outputs != expectedAfter
        || std::any_of(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&backendKey](const auto &entry) {
            return entry.output.backendKey == backendKey;
        })) return RemoteTopologyProtocol::error(id, u"partial"_s);
    return {{u"type"_s, u"topology-result"_s}, {u"v"_s, 1}, {u"id"_s, id}, {u"ok"_s, true},
        {u"topology"_s, RemoteTopologyProtocol::retainedReadOnly(id, *snapshot, m_experimentalMultiResize, m_experimentalPrimary, m_experimentalMixed)}};
}

QJsonObject VirtualSessionTransport::positionResult(const ConsoleWorkerWire::PositionResult &result,
    std::optional<quint32> uid, bool batch)
{
    if (m_positionId.isEmpty() || result.requestId != m_positionWorkerId || result.generation != m_positionBinding
        || batch != m_positionBatch) return {};
    const QString id = m_positionId;
    const QString generation = m_positionGeneration;
    const quint64 revision = m_positionRevision;
    const QString outputId = m_positionOutputId;
    const QPoint target = m_positionTarget;
    const auto expectedAfter = m_positionExpectedAfter;
    const bool expectedChange = m_positionExpectedChange;
    const bool current = authorized(uid) && m_handle && m_controlGeneration == result.generation;
    clearPosition();
    if (!current) return {};
    if (!result.error.isEmpty()) return RemoteTopologyProtocol::error(id, u"partial"_s);
    const auto snapshot = m_topologyResolve ? m_topologyResolve(*m_handle) : std::nullopt;
    if (!snapshot || snapshot->generation != generation || snapshot->revision < revision)
        return RemoteTopologyProtocol::error(id, u"capture-failed"_s);
    if (snapshot->outputs != expectedAfter || snapshot->revision != revision + (expectedChange ? 1 : 0)) {
        // A no-op keeps the revision; an actual layout change bumps exactly
        // once. The full after-state must match the preview, not only target.
        return RemoteTopologyProtocol::error(id, u"partial"_s);
    }
    const auto found = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&outputId](const auto &entry) {
        return entry.id == outputId;
    });
    if (!batch && (found == snapshot->outputs.cend() || found->output.logicalGeometry.topLeft() != target))
        return RemoteTopologyProtocol::error(id, u"partial"_s);
    return {{u"type"_s, u"topology-result"_s}, {u"v"_s, 1}, {u"id"_s, id}, {u"ok"_s, true},
        {u"topology"_s, RemoteTopologyProtocol::retainedReadOnly(id, *snapshot, m_experimentalMultiResize, m_experimentalPrimary, m_experimentalMixed)}};
}

QJsonObject VirtualSessionTransport::topologyResizeResult(const ConsoleWorkerWire::ResizeResult &result,
    std::optional<quint32> uid)
{
    if (m_topologyResizeId.isEmpty() || result.requestId != m_topologyResizeWorkerId
        || result.generation != m_topologyResizeBinding) return {};
    const QString id = m_topologyResizeId;
    const QString generation = m_topologyResizeGeneration;
    const quint64 revision = m_topologyResizeRevision;
    const QString outputId = m_topologyResizeOutputId;
    const QString backendKey = m_topologyResizeBackendKey;
    const auto expectedAfter = m_topologyResizeExpectedAfter;
    const bool expectedChange = m_topologyResizeExpectedChange;
    const bool mixedCreate = m_topologyMixedCreatePending;
    const bool current = authorized(uid) && m_handle && m_controlGeneration == result.generation;
    clearTopologyResize();
    if (!current) return {};
    if (!result.error.isEmpty()) return RemoteTopologyProtocol::error(id, u"partial"_s);
    const auto snapshot = m_topologyResolve ? m_topologyResolve(*m_handle) : std::nullopt;
    if (!snapshot || snapshot->generation != generation || snapshot->revision < revision)
        return RemoteTopologyProtocol::error(id, u"capture-failed"_s);
    if (mixedCreate) {
        if (snapshot->revision != revision + 1 || snapshot->outputs.size() != expectedAfter.size())
            return RemoteTopologyProtocol::error(id, u"partial"_s);
        QSet<QString> oldIds;
        const RemoteTopologyCatalog::Entry *expectedNew = nullptr;
        for (const auto &entry : expectedAfter) {
            if (entry.id == outputId) {
                if (expectedNew) return RemoteTopologyProtocol::error(id, u"partial"_s);
                expectedNew = &entry;
                continue;
            }
            oldIds.insert(entry.id);
            const auto found = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&entry](const auto &candidate) {
                return candidate.id == entry.id;
            });
            if (found == snapshot->outputs.cend() || *found != entry)
                return RemoteTopologyProtocol::error(id, u"partial"_s);
        }
        const auto newOutput = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&backendKey](const auto &entry) {
            return entry.output.backendKey == backendKey;
        });
        if (!expectedNew || expectedNew->output.backendKey != backendKey
            || newOutput == snapshot->outputs.cend() || oldIds.contains(newOutput->id)
            || newOutput->output != expectedNew->output)
            return RemoteTopologyProtocol::error(id, u"partial"_s);
        return {{u"type"_s, u"topology-result"_s}, {u"v"_s, 1}, {u"id"_s, id}, {u"ok"_s, true},
            {u"topology"_s, RemoteTopologyProtocol::retainedReadOnly(id, *snapshot,
                m_experimentalMultiResize, m_experimentalPrimary, m_experimentalMixed)}};
    }
    if (snapshot->outputs != expectedAfter || snapshot->revision != revision + (expectedChange ? 1 : 0))
        return RemoteTopologyProtocol::error(id, u"partial"_s);
    const auto found = std::find_if(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [&outputId](const auto &entry) {
        return entry.id == outputId;
    });
    if (found == snapshot->outputs.cend() || found->output.physical)
        return RemoteTopologyProtocol::error(id, u"partial"_s);
    return {{u"type"_s, u"topology-result"_s}, {u"v"_s, 1}, {u"id"_s, id}, {u"ok"_s, true},
        {u"topology"_s, RemoteTopologyProtocol::retainedReadOnly(id, *snapshot, m_experimentalMultiResize, m_experimentalPrimary, m_experimentalMixed)}};
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
    return RemoteTopologyProtocol::retainedReadOnly(id, *snapshot, m_experimentalMultiResize, m_experimentalPrimary, m_experimentalMixed);
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
    if (!m_resizeId.isEmpty() || !m_topologyResizeId.isEmpty())
        return VirtualResizeProtocol::reply(id, u"virtual resize already pending"_s);
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
    const bool fromDisplay = m_resizeFromDisplay;
    clearResize();
    if (fromDisplay) {
        // Nobody asked over KRDPCTL: no reply. A newer window size may be waiting.
        if (!result.error.isEmpty()) qWarning().noquote() << "Display Control resize failed:" << result.error;
        if (current) applyDisplayLayout(uid);
        return {};
    }
    return current ? VirtualResizeProtocol::reply(id, result.error) : QJsonObject{};
}

QJsonObject VirtualSessionTransport::request(const QJsonObject &record, std::optional<quint32> uid)
{
    {
        // StandardClientMedia is for clients that do not speak KRDPCTL: a
        // `device` record, or any other record this broker knows (not
        // audio-priority, which never closes krdpserver's gate either).
        const QString type = record.value(u"type"_s).toString();
        if (type == u"device"_s) m_deviceRecordSeen = true;
        else if (type == u"virtual-session"_s || type == u"virtual-resize"_s || type.startsWith(u"topology-"_s) || type == u"codec"_s || type == u"stats"_s)
            m_spokeKrdpctl = true;
        // Any record this broker knows (not audio-priority, which our client may send first
        // without a desktop in mind) marks a KRDPCTL client: never bound automatically.
        if (type == u"device"_s || type == u"virtual-session"_s || type == u"virtual-resize"_s || type.startsWith(u"topology-"_s) || type == u"codec"_s
            || type == u"stats"_s)
            noteKrdpctlClient();
        if (type == u"virtual-session"_s) {
            // Our own client chooses its desktop itself (AUD-D4 applies to stock clients only).
            m_virtualSessionSeen = true;
            m_stockGate.stop();
            m_stockWait.stop();
        }
    }
    if (m_revoking) return {};
    if (record.value(u"type"_s) == u"topology-query"_s) {
        const auto id = RemoteTopologyProtocol::queryId(record);
        if (!id) return RemoteTopologyProtocol::error(record.value(u"id"_s).toString().left(64), u"invalid"_s);
        if (!authorized(uid)) return RemoteTopologyProtocol::error(*id, u"not-owner"_s);
        if (!m_topologyResolve) return RemoteTopologyProtocol::error(*id, u"unsupported"_s);
        if (!m_topologyId.isEmpty()) return m_topologyId == *id ? QJsonObject{} : RemoteTopologyProtocol::error(*id, u"busy"_s);
        // The host publishes a snapshot only after decoded keyframes prove
        // every captured output (and multi-output KScreen agrees). An idle compositor may
        // not produce another frame merely because we request an IDR, so a
        // later read-only query can use that already verified publication.
        if (m_handle) {
            const auto snapshot = m_topologyResolve(*m_handle);
            if (snapshot && !snapshot->generation.isEmpty() && snapshot->revision && !snapshot->outputs.isEmpty()
                && std::all_of(snapshot->outputs.cbegin(), snapshot->outputs.cend(), [this](const auto &entry) {
                    return entry.output.owner == m_handle->id;
                }))
                return RemoteTopologyProtocol::retainedReadOnly(*id, *snapshot,
                    m_experimentalMultiResize, m_experimentalPrimary, m_experimentalMixed);
        }
        m_topologyId = *id;
        m_topologyBinding = m_controlGeneration;
        m_topologyDeadline.start();
        m_endpoint->requestKeyFrame();
        return {};
    }
    if (record.value(u"type"_s) == u"topology-preview"_s) {
        return topologyPreview(record, uid);
    }
    if (record.value(u"type"_s) == u"topology-fit-preview"_s) return topologyFitPreview(record, uid);
    if (record.value(u"type"_s) == u"topology-commit"_s) return topologyCommit(record, uid);
    if (record.value(u"type"_s) == u"virtual-resize"_s) return requestResize(record, uid);
    if (record.value(u"type"_s) == u"codec"_s) {
        // AUD-FIX7: the connection's codec policy, as krdpserver's (CodecRequest). It is the
        // connection's preference, so it may come before any desktop is attached; the worker
        // gets the choice when one is (WorkerCodecBridge). A broker without a VideoCodecHost
        // (no `video` group) still answers AVC only, without `backend` (AUD-FIX2 F2).
        if (!record.value(u"codecs"_s).isArray()) return CodecRequest::invalidRecord();
        const auto parsed = CodecRequest::parse(record);
        if (!parsed) return CodecRequest::invalidRecord();
        if (!m_videoHost || !m_connection) return LayoutControl::codecRecord(u"avc"_s, std::nullopt, u"virtual desktops stream AVC only"_s);
        QString log;
        const auto reply = CodecRequest::apply(*m_connection->videoStream(), *parsed, &log);
        qInfo().noquote() << "Virtual client" << m_client << log;
        return reply;
    }
    if (record.value(u"type"_s) == u"stats"_s) {
        // KRDPCTL stats: this connection's own stream (StatsRequest, as krdpserver). Like `codec`
        // it needs no desktop; while none is attached the samples show an idle stream.
        const auto parsed = StatsRequest::parse(record);
        if (!parsed) return StatsRequest::invalidRecord();
        if (!m_connection) return {};
        QString log;
        const auto reply = StatsRequest::apply(*m_connection->videoStream(), *parsed, &log);
        qDebug().noquote() << "Virtual client" << m_client << log;
        return reply;
    }
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
    if (record.value(u"type"_s) == u"device"_s) {
        // KRDPCTL `device` (KRDPCTL-V2-CONTRACT.md §d). Request-level refusals are
        // `error` records; a device's outcome is a `device` state record. The
        // microphone is answered when the worker acknowledges its source.
        const auto parsed = DeviceControl::parseRequest(record);
        if (const auto *error = std::get_if<LayoutControl::Error>(&parsed)) return LayoutControl::errorRecord(*error);
        const auto device = std::get<DeviceControl::Request>(parsed);
        if (const auto refused = DeviceControl::checkSupported(device, VirtualDeviceCapabilities)) return LayoutControl::errorRecord(*refused);
        if (device.action == DeviceControl::Action::Query) return deviceReply(device.device, deviceStatus(device.device));
        const QPointer<VirtualSessionTransport> alive(this);
        if (m_mediaDispatch) return {};
        if (!authorized(uid)) {
            // A refused request must not leave an earlier consent running: end
            // this connection's devices locally (never another attachment's worker).
            stopMicrophone(uid);
            if (!alive || !m_connection) return {};
            m_playback = m_silenceHost = false;
            m_connection->setDeviceEnabled(MediaDevice::Playback, false);
            return LayoutControl::errorRecord({u"not-owner"_s, u"devices need the authenticated owner of an attached virtual desktop"_s});
        }
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
        const auto revoked = [this] {
            return deviceReply(MediaDevice::Microphone, {DeviceStatus::State::Off, false, DeviceControl::Detached, u"this connection no longer holds the virtual desktop"_s});
        };
        const bool on = device.action == DeviceControl::Action::On;
        if (device.device == MediaDevice::Playback) {
            const bool wasPriority = m_connection->audioPriorityActive();
            m_playback = on;
            m_silenceHost = on && device.silenceHost;
            // Only atomics/queue state: no signals.
            m_connection->setExternalAudioPlayback(true);
            m_connection->setDeviceEnabled(MediaDevice::Playback, m_playback);
            m_endpoint->setMedia({m_playback, m_silenceHost});
            if (!alive || !m_connection) return {};
            if (wasPriority) {
                restoreFixedVideoQuality(uid);
                if (!alive || !m_connection) return {};
            }
            if (!bindingCurrent())
                return deviceReply(MediaDevice::Playback, {DeviceStatus::State::Off, false, DeviceControl::Detached, u"this connection no longer holds the virtual desktop"_s});
            return deviceReply(MediaDevice::Playback, deviceStatus(MediaDevice::Playback));
        }
        // Microphone: any change ends the current source first (a new `on` is a
        // new consent period with a new worker request).
        stopMicrophone(uid);
        if (!alive || !m_connection) return {};
        // Socket failure during source-off can synchronously revoke the binding.
        if (!bindingCurrent()) return revoked();
        if (!on) return deviceReply(MediaDevice::Microphone, deviceStatus(MediaDevice::Microphone));
        // Reserve one request ID for source-off; never wrap on enable/disable.
        if (!m_externalMicrophone || !m_controlGeneration
            || m_nextMicrophoneId >= std::numeric_limits<quint64>::max() - 1)
            return deviceReply(MediaDevice::Microphone, {DeviceStatus::State::Error, false, DeviceControl::Unavailable, u"virtual microphone unavailable"_s});
        m_microphonePolicy = {m_controlGeneration, ++m_nextMicrophoneId, true};
        m_microphoneRequestId = m_replyRequestId; // the worker's acknowledgement answers this request
        m_microphoneDeadline.start();
        const bool dispatched = m_endpoint->setMicrophone(m_microphonePolicy);
        if (!alive || !m_connection) return {};
        if (!bindingCurrent()) return {}; // A replaced binding cannot acknowledge this consent.
        if (!dispatched) {
            m_microphoneRequestId.clear(); // answered synchronously below (echo once)
            stopMicrophone();
            return alive ? deviceReply(MediaDevice::Microphone, {DeviceStatus::State::Error, false, DeviceControl::Unavailable, u"cannot dispatch virtual microphone startup"_s})
                         : QJsonObject{};
        }
        return {}; // Success is acknowledged only after correlated source readiness.
    }
    return {{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"code"_s, u"unsupported"_s},
            {u"message"_s, u"virtual-session transport does not support this request"_s}};
}
}
