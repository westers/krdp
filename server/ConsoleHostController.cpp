// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleHostController.h"
#include "AudioPriority.h"

#include <algorithm>
#include <utility>

#include <QDir>
#include <QRandomGenerator>
#include <QSet>
#include <QStringList>
#include <QUuid>

#include <RdpConnection.h>
#include <Server.h>
#include <VideoStream.h>
#include <VideoCodecSupport.h>
#include <InputHandler.h>
#include <LayoutControl.h>

#include "ConsoleSeat.h"
#include "ConsoleWorkerSession.h"
#include "ConsoleResize.h"
#include "ConsoleFrameLayout.h"
#include "RemoteTopologyProtocol.h"

using namespace Qt::StringLiterals;

namespace
{
std::optional<KRdp::ConsoleWorkerWire::PhysicalLayout> physicalCommand(
    const KRdp::ConsoleTopologyPlan::Plan &plan,
    const QVector<KRdp::RemoteTopologyDraft::Operation> &operations,
    quint64 requestId, quint64 controlGeneration)
{
    using namespace KRdp;
    ConsoleWorkerWire::PhysicalLayout command;
    command.requestId = requestId;
    command.controlGeneration = controlGeneration;
    command.catalogGeneration = plan.before.generation;
    command.expectedRevision = plan.before.revision;
    command.allowPhysicalChange = true;
    for (const auto &entry : plan.before.outputs) {
        const auto &output = entry.output;
        command.before.append({output.backendKey, output.nativePixels, output.logicalGeometry,
            output.scale, output.primary, quint8(plan.beforePriorities.value(output.backendKey))});
    }
    for (const auto &operation : operations) {
        const auto found = std::find_if(plan.before.outputs.cbegin(), plan.before.outputs.cend(), [&operation](const auto &entry) {
            return entry.id == operation.id;
        });
        if (found == plan.before.outputs.cend()) return {};
        ConsoleWorkerWire::MixedOperation change;
        change.output = found->output.backendKey;
        switch (operation.kind) {
        case RemoteTopologyDraft::Operation::Kind::Move:
            change.kind = ConsoleWorkerWire::MixedOperation::Kind::Move;
            change.globalLogical = operation.position;
            break;
        case RemoteTopologyDraft::Operation::Kind::Resize:
            change.kind = ConsoleWorkerWire::MixedOperation::Kind::Resize;
            change.pixels = operation.pixels;
            change.scale = operation.scale;
            break;
        case RemoteTopologyDraft::Operation::Kind::SetPrimary:
            change.kind = ConsoleWorkerWire::MixedOperation::Kind::Primary;
            break;
        case RemoteTopologyDraft::Operation::Kind::AddVirtual:
        case RemoteTopologyDraft::Operation::Kind::Remove:
            return {};
        }
        command.operations.append(change);
    }
    return command;
}

QStringList priorityOrder(const QMap<QString, int> &priorities, const QSet<QString> &survivors)
{
    QVector<QPair<int, QString>> ordered;
    for (auto it = priorities.cbegin(); it != priorities.cend(); ++it) {
        if (survivors.contains(it.key())) ordered.append({it.value(), it.key()});
    }
    std::sort(ordered.begin(), ordered.end());
    QStringList names;
    for (const auto &entry : ordered) names.append(entry.second);
    return names;
}
}

namespace KRdp
{
ConsoleHostController::ConsoleHostController(Server *server, WorkerLauncher launchWorker, QString runtimeDirectory, QObject *parent)
    : QObject(parent)
    , m_server(server)
    , m_launchWorker(std::move(launchWorker))
    , m_runtimeDirectory(std::move(runtimeDirectory))
{
    Q_ASSERT(m_server);
    m_experimentalPhysicalTopology = qEnvironmentVariableIntValue("KRDP_EXPERIMENTAL_CONSOLE_TOPOLOGY") == 1;
    m_experimentalConsoleVirtual = qEnvironmentVariableIntValue("KRDP_EXPERIMENTAL_CONSOLE_VIRTUAL") == 1;
    m_physicalDeadline.setSingleShot(true);
    m_physicalDeadline.setInterval(45000);
    connect(&m_physicalDeadline, &QTimer::timeout, this, [this] {
        finishPhysicalTopology(u"timeout"_s);
        finishVirtualTopology(u"timeout"_s);
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::physicalLayoutFinished, this, [this](const auto &result) {
        if (!m_pendingPhysical || result.requestId != m_pendingPhysical->serial
            || result.controlGeneration != m_pendingPhysical->controlGeneration) return;
        if (!result.error.isEmpty()) {
            finishPhysicalTopology(u"partial"_s, result.error);
            return;
        }
        // The worker has committed a captured physical change. Keep the
        // broker closed on any later owner change until that worker explicitly
        // verifies conditional release; its socket disappearing is not proof.
        if (m_pendingPhysical->plan.changed) {
            m_physicalLeaseActive = true;
            m_physicalLeaseGeneration = result.controlGeneration;
        }
        m_pendingPhysical->waitingReadback = true;
        if (!m_endpoint.requestTopology()) finishPhysicalTopology(u"capture-failed"_s);
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::physicalLeaseReleased, this, [this](const auto &result) {
        if (!m_physicalLeaseActive || result.controlGeneration != m_physicalLeaseGeneration) return;
        if (!result.verified) {
            qWarning() << "Physical Console lease could not be verified on release; capture remains disabled";
            return;
        }
        m_physicalLeaseActive = false;
        m_consoleCreatorsActive = false;
        m_physicalLeaseGeneration = 0;
    });
    const auto virtualFinished = [this](quint64 requestId, quint64 generation, const QString &error, bool add) {
        if (!m_pendingVirtual || m_pendingVirtual->serial != requestId
            || m_pendingVirtual->controlGeneration != generation || m_pendingVirtual->add != add) return;
        if (!error.isEmpty()) { finishVirtualTopology(u"partial"_s, error); return; }
        m_pendingVirtual->waitingReadback = true;
        if (!m_endpoint.requestTopology()) finishVirtualTopology(u"capture-failed"_s);
    };
    connect(&m_endpoint, &ConsoleWorkerEndpoint::addVirtualFinished, this, [virtualFinished](const auto &result) {
        virtualFinished(result.requestId, result.generation, result.error, true);
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::removeVirtualFinished, this, [virtualFinished](const auto &result) {
        virtualFinished(result.requestId, result.generation, result.error, false);
    });
    m_microphoneDeadline.setSingleShot(true);
    m_microphoneDeadline.setInterval(4000);
    connect(&m_microphoneDeadline, &QTimer::timeout, this, [this] {
        stopMicrophone(u"microphone worker startup timed out"_s);
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::microphoneFinished, this, &ConsoleHostController::microphoneResult);
    m_microphonePump.setInterval(20);
    connect(&m_microphonePump, &QTimer::timeout, this, [this] {
        if (!m_microphoneReady || !m_inputEnabled || !m_control.ownsControl(m_microphoneClient)) return;
        for (const auto &client : m_clients) {
            if (client->id != m_microphoneClient) continue;
            const auto pcm = client->connection->takeExternalMicrophone();
            if (!pcm.isEmpty()) {
                // Never retry stale speech when the worker socket is backed up.
                m_endpoint.sendMicrophoneAudio({m_microphonePolicy.generation, m_microphonePolicy.requestId, pcm});
            }
            break;
        }
    });
    m_resizeDeadline.setSingleShot(true);
    m_resizeDeadline.setInterval(45000);
    connect(&m_resizeDeadline, &QTimer::timeout, this, [this]() {
        finishResize(u"physical resize timed out"_s);
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::resizeFinished, this, [this](const auto &result) {
        if (m_pendingResize && result.requestId == m_pendingResize->requestId && result.generation == m_pendingResize->generation) {
            finishResize(result.error);
        }
    });
    m_seatPoll.setInterval(500);
    connect(&m_seatPoll, &QTimer::timeout, this, &ConsoleHostController::refreshSeat);
    connect(m_server, &Server::newConnectionCreated, this, &ConsoleHostController::addClient);
    connect(&m_endpoint, &ConsoleWorkerEndpoint::workerReady, this, [this](const auto &target) {
        apply(m_handoff.workerReady(target));
        qInfo() << "Console worker ready:" << target.sessionId << "forwarding" << m_inputEnabled;
        m_endpoint.setControlState({m_controlGeneration, m_control.owner() != 0});
        for (const auto &client : m_clients) {
            if (m_control.ownsControl(client->id)) {
                m_endpoint.setVideoQuality({m_controlGeneration, client->videoQuality});
            }
        }
        if (m_mediaConfigured) {
            m_endpoint.setMedia(m_media);
        }
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::workerStopped, this, [this]() {
        setWorkerActive(false);
        finishPhysicalTopology(u"capture-failed"_s);
        finishVirtualTopology(u"capture-failed"_s);
        stopMicrophone(u"console microphone worker stopped"_s);
        finishResize(u"console capture worker stopped during resize"_s);
        apply(m_handoff.workerStopped());
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [this](const VideoFrame &frame) {
        if (!m_inputEnabled || m_pendingPhysical || m_pendingVirtual || m_layoutAwaitingReadback) {
            return;
        }
        if (m_outputs.monitors.size() > 1) {
            if (!m_topologyAvailable || !ConsoleFrameLayout::confirmed(frame, m_outputs, m_topologyCatalog.snapshot())) return;
        } else if (frame.monitors.size() > 1) return;
        for (const auto &client : m_clients) {
            const QVector<VideoMonitor> desired = frame.monitors.size() > 1 ? frame.monitors : QVector<VideoMonitor>{};
            if (client->wireLayout != desired) {
                if (!frame.isKeyFrame || (desired.isEmpty() && !m_topologyAvailable && !client->wireLayout.isEmpty())) continue;
                client->connection->videoStream()->setMonitorLayout(desired);
                client->wireLayout = desired;
            }
            client->session->submitFrame(frame);
        }
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::audioReceived, this, [this](const ConsoleWorkerWire::Audio &audio) {
        for (const auto &client : m_clients) {
            client->connection->submitExternalAudio(audio.pcm);
        }
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [this](const ConsoleWorkerWire::Outputs &outputs) {
        qInfo() << "Console capture outputs:" << outputs.monitors.size() << "forwarding" << m_inputEnabled << "clients" << m_clients.size();
        const bool changed = outputs != m_outputs;
        const bool wasMulti = m_outputs.monitors.size() > 1;
        if (changed) {
            m_topologyAvailable = false;
            m_topologyPriorities.clear();
            m_physicalPreview.reset();
            m_virtualPreview.reset();
            finishTopologyQueries(u"capture-failed"_s);
        }
        m_outputs = outputs;
        // Do not publish an in-flight layout from an Outputs record alone.
        // First require the worker's independently captured KScreen topology.
        if (changed && (outputs.monitors.size() > 1 || wasMulti)
            && !m_pendingPhysical && !m_pendingVirtual) m_layoutAwaitingReadback = true;
        if (!m_pendingPhysical && !m_pendingVirtual && !m_layoutAwaitingReadback) sendLayouts();
        if (changed && (outputs.monitors.size() > 1 || wasMulti)
            && !m_pendingPhysical && !m_pendingVirtual) m_endpoint.requestTopology();
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::topologyReceived, this, [this](const ConsoleWorkerWire::Topology &topology) {
        if (topology.outputs.isEmpty() || topology.outputs.size() != m_outputs.monitors.size()) {
            m_topologyAvailable = false;
            m_topologyPriorities.clear();
            m_topologyCatalog.resetGeneration();
            finishTopologyQueries(u"capture-failed"_s);
            if (m_pendingPhysical && m_pendingPhysical->waitingReadback) finishPhysicalTopology(u"capture-failed"_s);
            if (m_pendingVirtual && m_pendingVirtual->waitingReadback) finishVirtualTopology(u"capture-failed"_s);
            return;
        }
        QVector<RemoteTopologyCatalog::Output> inventory;
        QMap<QString, int> priorities;
        QRect workspace;
        for (const auto &output : topology.outputs) workspace |= output.logical;
        for (const auto &output : topology.outputs) {
            const auto found = std::find_if(m_outputs.monitors.cbegin(), m_outputs.monitors.cend(), [&output](const auto &monitor) {
                return monitor.name == output.name;
            });
            if (found == m_outputs.monitors.cend() || found->geometry != output.logical.translated(-workspace.topLeft())
                || found->primary != output.primary || output.priority < 1 || output.priority > 16
                || priorities.values().contains(output.priority)) {
                m_topologyAvailable = false;
                m_topologyPriorities.clear();
                m_topologyCatalog.resetGeneration();
                finishTopologyQueries(u"capture-failed"_s);
                if (m_pendingPhysical && m_pendingPhysical->waitingReadback) finishPhysicalTopology(u"capture-failed"_s);
                if (m_pendingVirtual && m_pendingVirtual->waitingReadback) finishVirtualTopology(u"capture-failed"_s);
                return;
            }
            priorities.insert(output.name, output.priority);
            inventory.append({.backendKey = output.name, .name = output.name, .nativePixels = output.pixels,
                .logicalGeometry = output.logical, .scale = output.scale, .enabled = true,
                .primary = output.primary, .physical = output.physical,
                .owner = output.physical ? QString{} : QStringLiteral("physical-console")});
        }
        m_topologyAvailable = m_topologyCatalog.observe(inventory, !m_topologyPriorities.isEmpty()
            && priorities != m_topologyPriorities).has_value();
        if (!m_topologyAvailable) {
            m_topologyPriorities.clear();
            m_topologyCatalog.resetGeneration();
        } else m_topologyPriorities = priorities;
        finishTopologyQueries(m_topologyAvailable ? QString() : u"capture-failed"_s);
        const bool republishedLayout = m_topologyAvailable && m_layoutAwaitingReadback
            && !m_pendingPhysical && !m_pendingVirtual;
        if (republishedLayout) {
            m_layoutAwaitingReadback = false;
            sendLayouts();
        }
        if (m_topologyAvailable && !m_pendingPhysical && !m_pendingVirtual
            && (m_outputs.monitors.size() > 1 || republishedLayout))
            m_endpoint.requestKeyFrame();
        if (m_pendingPhysical && m_pendingPhysical->waitingReadback) {
            const auto &pending = *m_pendingPhysical;
            const auto &snapshot = m_topologyCatalog.snapshot();
            if (!m_topologyAvailable) finishPhysicalTopology(u"capture-failed"_s);
            else if (snapshot.generation != pending.plan.before.generation
                || snapshot.revision != pending.plan.before.revision + (pending.plan.changed ? 1 : 0)
                || snapshot.outputs != pending.plan.after || m_topologyPriorities != pending.plan.afterPriorities)
                finishPhysicalTopology(u"partial"_s);
            else finishPhysicalTopology({});
        }
        if (m_pendingVirtual && m_pendingVirtual->waitingReadback) {
            const auto pending = *m_pendingVirtual;
            const auto &snapshot = m_topologyCatalog.snapshot();
            bool matches = m_topologyAvailable && snapshot.generation == pending.before.generation
                && snapshot.revision == pending.before.revision + 1
                && snapshot.outputs.size() == pending.draft.after.size();
            if (matches) {
                QSet<QString> survivors;
                for (const auto &entry : pending.before.outputs) {
                    if (entry.output.backendKey != pending.backendKey) survivors.insert(entry.output.backendKey);
                }
                matches = priorityOrder(pending.priorities, survivors) == priorityOrder(m_topologyPriorities, survivors);
            }
            if (matches && pending.add) {
                const auto added = std::find_if(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&pending](const auto &entry) {
                    return entry.output.backendKey == pending.backendKey;
                });
                matches = added != snapshot.outputs.cend() && !added->output.physical
                    && added->output == pending.draft.after.last().output;
                for (const auto &entry : pending.before.outputs) {
                    const auto old = std::find_if(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&entry](const auto &now) {
                        return now.id == entry.id;
                    });
                    matches = matches && old != snapshot.outputs.cend() && *old == entry;
                }
            } else if (matches) {
                matches = snapshot.outputs == pending.draft.after
                    && std::none_of(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&pending](const auto &entry) {
                        return entry.output.backendKey == pending.backendKey;
                    });
            }
            if (!matches) finishVirtualTopology(u"partial"_s);
            else {
                m_consoleCreatorsActive = std::any_of(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [](const auto &entry) {
                    return !entry.output.physical && entry.output.owner == u"physical-console"_s;
                });
                if (!m_consoleCreatorsActive) {
                    m_physicalLeaseActive = false;
                    m_physicalLeaseGeneration = 0;
                }
                finishVirtualTopology({});
            }
        }
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::localTakeover, this, [this](quint64 generation) {
        if (!m_control.owner() || generation != m_controlGeneration) {
            return; // A late cursor report must not revoke a newer controller.
        }
        qInfo() << "Local console takeover: releasing remote control";
        releaseInput();
        m_control.release(m_control.owner());
        syncControlState();
        updateMedia();
        sendLayouts();
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::protocolError, this, [this](const QString &message) {
        qWarning().noquote() << "Console worker protocol error:" << message;
        setWorkerActive(false);
        m_endpoint.close();
        apply(m_handoff.workerStopped());
    });
}

ConsoleHostController::~ConsoleHostController()
{
    stopMicrophone({});
}

void ConsoleHostController::start()
{
    refreshSeat();
    m_seatPoll.start();
}

void ConsoleHostController::refreshSeat()
{
    QString error;
    const auto sessions = ConsoleSeat::readLogindSessions(&error);
    if (sessions.isEmpty() && !error.isEmpty()) {
        qWarning().noquote() << "Unable to read console seat:" << error;
        return;
    }
    apply(m_handoff.reconcile(sessions));
}

void ConsoleHostController::workerExited(const QString &socketName)
{
    // The socket path identifies this launch, including retries for the same
    // logind session. A late exit from an old worker must not stop its successor.
    if (socketName != m_endpoint.socketName()) {
        return;
    }
    setWorkerActive(false);
    m_endpoint.close();
    apply(m_handoff.workerStopped());
}

void ConsoleHostController::apply(const ConsoleHandoff::Actions &actions)
{
    if (actions.empty()) {
        return;
    }
    if (actions.revokeInput) {
        setWorkerActive(false);
    }
    if (actions.stopWorker) {
        m_endpoint.stopWorker();
    }
    if (actions.startWorker) {
        startWorker(actions.target);
    }
    if (actions.grantInput) {
        setWorkerActive(true);
    }
    if (actions.resetGraphics) {
        for (const auto &client : m_clients) {
            client->connection->videoStream()->reset();
        }
    }
    if (actions.requestKeyFrame) {
        m_endpoint.requestKeyFrame();
    }
}

void ConsoleHostController::startWorker(const ConsoleHandoff::Target &target)
{
    m_physicalPreview.reset();
    m_virtualPreview.reset();
    finishPhysicalTopology(u"capture-failed"_s);
    finishVirtualTopology(u"capture-failed"_s);
    m_topologyAvailable = false;
    m_layoutAwaitingReadback = false;
    m_topologyPriorities.clear();
    m_topologyCatalog.resetGeneration();
    finishTopologyQueries(u"capture-failed"_s);
    m_outputs = {}; // Never describe the prior greeter/user's outputs during handoff.
    if (!QDir().mkpath(m_runtimeDirectory)) {
        qWarning().noquote() << "Cannot create console runtime directory" << m_runtimeDirectory;
        return;
    }
    const QString socketName = QDir(m_runtimeDirectory).filePath(QStringLiteral("seat0-%1-%2.sock").arg(target.sessionId).arg(QRandomGenerator::global()->generate64(), 0, 16));
    QByteArray token;
    token.reserve(32);
    for (int i = 0; i < 4; ++i) {
        const quint64 value = QRandomGenerator::system()->generate64();
        for (int byte = 0; byte < 8; ++byte) {
            token.append(char(value >> (byte * 8)));
        }
    }
    QString error;
    if (!m_endpoint.listen(socketName, target, token, &error)) {
        qWarning().noquote() << "Cannot create console worker endpoint:" << error;
        return;
    }
    if (!m_launchWorker || !m_launchWorker(target, m_endpoint.socketName(), token, &error)) {
        qWarning().noquote() << "Cannot launch console worker for session" << target.sessionId << ':' << error;
        m_endpoint.close();
        // close() is local and therefore has no disconnected signal. Tell the
        // handoff state to retry on its next logind poll.
        apply(m_handoff.workerStopped());
    }
}

void ConsoleHostController::setWorkerActive(bool active)
{
    if (active && m_physicalLeaseActive) {
        qWarning() << "Refusing Console capture while physical layout lease release is unverified";
        active = false;
    }
    qInfo() << "Console capture forwarding:" << active;
    if (!active) {
        m_physicalPreview.reset();
        m_virtualPreview.reset();
        finishPhysicalTopology(u"capture-failed"_s);
        finishVirtualTopology(u"capture-failed"_s);
        m_topologyAvailable = false;
        m_layoutAwaitingReadback = false;
        m_topologyPriorities.clear();
        m_topologyCatalog.resetGeneration();
        finishTopologyQueries(u"capture-failed"_s);
        stopMicrophone(u"console session changed; microphone consent must be renewed"_s);
        finishResize(u"console capture worker changed during resize"_s);
        releaseInput();
    }
    m_inputEnabled = active;
    for (const auto &client : m_clients) {
        client->session->setWorkerActive(active);
    }
}

void ConsoleHostController::addClient(RdpConnection *connection)
{
    // The initial cross-session worker owns a single AVC 4:2:0 encoder. Do
    // not let a capable own client negotiate AVC444/private codecs until the
    // broker can renegotiate and restart that worker atomically.
    connection->videoStream()->setCodecPreference(CodecPreference::Avc420);
    connection->videoStream()->setQualityCap(80);
    // Preserve the console's fixed baseline unless audio priority explicitly
    // enables congestion steering for its controlling connection.
    connection->videoStream()->setAdaptiveQuality(false);
    auto client = std::make_unique<Client>();
    const auto id = client->id = ++m_nextClientId;
    client->connection = connection;
    client->externalMicrophone = connection->enableExternalMicrophone();
    client->session = std::make_unique<ConsoleWorkerSession>([this, id](const ConsoleWorkerWire::Input &input) {
        if (m_inputEnabled && !m_pendingPhysical && !m_pendingVirtual && m_control.ownsControl(id)) {
            m_endpoint.sendInput(input);
            m_inputState.record(input);
        }
    });
    client->session->setWorkerActive(m_inputEnabled);
    client->connections.append(connect(connection->videoStream(), &VideoStream::requestedQualityChanged,
                                       this, [this, id](quint8 quality) {
        for (const auto &entry : m_clients) {
            if (entry->id == id) {
                entry->videoQuality = quality;
                if (m_control.ownsControl(id)) {
                    m_endpoint.setVideoQuality({m_controlGeneration, quality});
                }
                break;
            }
        }
    }));
    client->connections.append(connect(connection->videoStream(), &VideoStream::keyFrameRequested,
                                       &m_endpoint, [this](int) { m_endpoint.requestKeyFrame(); }, Qt::QueuedConnection));
    client->connections.append(connect(connection->videoStream(), &VideoStream::enabledChanged,
                                       &m_endpoint, [this]() { m_endpoint.requestKeyFrame(); }, Qt::QueuedConnection));
    client->connections.append(connect(client->session.get(), &AbstractSession::frameReceived, connection->videoStream(), &VideoStream::queueFrame));
    client->connections.append(connect(client->session.get(), &ConsoleWorkerSession::keyFrameRequested, &m_endpoint, &ConsoleWorkerEndpoint::requestKeyFrame));
    client->connections.append(connect(connection->inputHandler(), &InputHandler::inputEvent, client->session.get(), &AbstractSession::sendEvent));
    client->connections.append(connect(connection, &RdpConnection::controlRecordReceived, this, [this, connection, id](const QJsonObject &record) {
        onControlRecord(connection, id, record);
    }, Qt::QueuedConnection));
    client->connections.append(connect(connection, &RdpConnection::stateChanged, this, [this, connection, id](RdpConnection::State state) {
        if (state == RdpConnection::State::Streaming) {
            // Running is pre-authentication. Media preflight can also arrive
            // before Streaming; defer it without granting any authority.
            m_control.admit(id);
            syncControlState();
            sendLayouts();
            for (const auto &client : m_clients) {
                if (client->id == id && !client->pendingMedia.isEmpty()) {
                    const auto pending = std::exchange(client->pendingMedia, {});
                    onControlRecord(connection, id, pending);
                    break;
                }
            }
        }
        if (state == RdpConnection::State::Closed) {
            removeClient(connection);
        }
    }));
    m_clients.push_back(std::move(client));
}

void ConsoleHostController::onControlRecord(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &record)
{
    const QString type = record.value(u"type"_s).toString();
    if (type == u"topology-preview"_s) {
        const auto parsed = RemoteTopologyProtocol::previewRequest(record);
        const QString request = record.value(u"id"_s).toString().left(64);
        const auto refuse = [connection](const QString &requestId, const QString &code) {
            connection->sendControlRecord(RemoteTopologyProtocol::error(requestId, code));
        };
        if (!parsed) { refuse(request, u"invalid"_s); return; }
        if (!m_experimentalPhysicalTopology || m_endpoint.target().adapter != ConsoleSeat::Adapter::PhysicalUser) {
            refuse(parsed->id, u"unsupported"_s); return;
        }
        if (!m_control.ownsControl(id) || !m_inputEnabled || !m_endpoint.ready()) {
            refuse(parsed->id, u"not-owner"_s); return;
        }
        if (!m_topologyAvailable || m_topologyPriorities.isEmpty()) {
            refuse(parsed->id, u"capture-failed"_s); return;
        }
        if (m_pendingPhysical || m_pendingVirtual || m_pendingResize) { refuse(parsed->id, u"busy"_s); return; }
        auto draft = parsed->draft;
        const auto snapshot = m_topologyCatalog.snapshot();
        if (draft.generation != snapshot.generation) { refuse(parsed->id, u"stale-generation"_s); return; }
        if (draft.expectedRevision != snapshot.revision) { refuse(parsed->id, u"stale-revision"_s); return; }
        const bool virtualOperation = draft.operations.size() == 1
            && (draft.operations.first().kind == RemoteTopologyDraft::Operation::Kind::AddVirtual
                || draft.operations.first().kind == RemoteTopologyDraft::Operation::Kind::Remove);
        if (virtualOperation) {
            if (!m_experimentalConsoleVirtual) { refuse(parsed->id, u"unsupported"_s); return; }
            const auto operation = draft.operations.first();
            if (draft.allowPhysicalChange || (operation.kind == RemoteTopologyDraft::Operation::Kind::Remove && !draft.allowRemoval)
                || (operation.kind == RemoteTopologyDraft::Operation::Kind::AddVirtual && m_physicalLeaseActive && !m_consoleCreatorsActive)) {
                refuse(parsed->id, u"invalid"_s); return;
            }
            draft.owner = u"physical-console"_s;
            auto proposed = RemoteTopologyDraft::preview(snapshot,
                {.addVirtual = true, .removeVirtual = true, .maxOutputs = 16,
                 .maxOutputDimension = 4096, .maxAtlasDimension = 8192}, draft);
            if (!proposed.valid()) { refuse(parsed->id, proposed.error); return; }
            QString backendKey;
            if (operation.kind == RemoteTopologyDraft::Operation::Kind::AddVirtual) {
                backendKey = u"Virtual-krdp-added-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
                proposed.after.last().output.backendKey = backendKey;
                proposed.after.last().output.name = backendKey;
            }
            if (operation.kind == RemoteTopologyDraft::Operation::Kind::Remove) {
                const auto found = std::find_if(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&operation](const auto &entry) {
                    return entry.id == operation.id;
                });
                if (found == snapshot.outputs.cend() || !found->output.backendKey.startsWith(u"Virtual-krdp-added-"_s)) {
                    refuse(parsed->id, u"not-owner"_s); return;
                }
                backendKey = found->output.backendKey;
            }
            QString token;
            for (int i = 0; i < 4; ++i)
                token += QString::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, QLatin1Char('0'));
            m_physicalPreview.reset();
            m_virtualPreview = VirtualPreview{id, m_controlGeneration, parsed->id, token, snapshot,
                m_topologyPriorities, proposed, operation, backendKey, {}};
            m_virtualPreview->age.start();
            connection->sendControlRecord(RemoteTopologyProtocol::previewReply(parsed->id, token, proposed,
                snapshot, u"lease"_s, QJsonArray{u"temporary-console-output"_s}));
            return;
        }
        if (!draft.allowPhysicalChange || draft.allowRemoval || m_consoleCreatorsActive) {
            refuse(parsed->id, u"invalid"_s); return;
        }
        draft.owner = QString::number(id); // Only authenticated controller supplies ownership.
        const auto plan = ConsoleTopologyPlan::make(snapshot, m_topologyPriorities, draft,
            {.changePrimary = true, .maxOutputs = 16, .maxOutputDimension = 4096, .maxAtlasDimension = 8192});
        if (!plan) { refuse(parsed->id, u"invalid"_s); return; }
        QString token;
        for (int i = 0; i < 4; ++i)
            token += QString::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, QLatin1Char('0'));
        m_physicalPreview = PhysicalPreview{id, m_controlGeneration, parsed->id, token, *plan, draft.operations, {}};
        m_virtualPreview.reset();
        m_physicalPreview->age.start();
        connection->sendControlRecord(RemoteTopologyProtocol::previewReply(parsed->id, token,
            {plan->before.outputs, plan->after, {}}, snapshot, u"lease"_s,
            QJsonArray{u"physical-output-change"_s}));
        return;
    }
    if (type == u"topology-commit"_s) {
        const auto parsed = RemoteTopologyProtocol::commitRequest(record);
        const QString request = record.value(u"id"_s).toString().left(64);
        const auto refuse = [connection](const QString &requestId, const QString &code) {
            connection->sendControlRecord(RemoteTopologyProtocol::error(requestId, code));
        };
        if (!parsed) { refuse(request, u"invalid"_s); return; }
        if (!m_experimentalPhysicalTopology || m_endpoint.target().adapter != ConsoleSeat::Adapter::PhysicalUser) {
            refuse(parsed->id, u"unsupported"_s); return;
        }
        if (!m_control.ownsControl(id) || !m_inputEnabled || !m_endpoint.ready()) {
            refuse(parsed->id, u"not-owner"_s); return;
        }
        if (m_pendingPhysical || m_pendingVirtual || m_pendingResize) { refuse(parsed->id, u"busy"_s); return; }
        if (m_virtualPreview && m_virtualPreview->owner == id && m_virtualPreview->id == parsed->id
            && m_virtualPreview->token == parsed->token) {
            const auto preview = std::exchange(m_virtualPreview, std::nullopt);
            const auto &before = preview->before;
            const auto &snapshot = m_topologyCatalog.snapshot();
            if (!preview->age.isValid() || preview->age.elapsed() >= RemoteTopologyProtocol::PreviewLifetimeMs) {
                refuse(parsed->id, u"invalid"_s); return;
            }
            if (!m_topologyAvailable || parsed->generation != before.generation || snapshot.generation != before.generation) {
                refuse(parsed->id, u"stale-generation"_s); return;
            }
            if (parsed->expectedRevision != before.revision || snapshot != before
                || preview->priorities != m_topologyPriorities || preview->controlGeneration != m_controlGeneration) {
                refuse(parsed->id, u"stale-revision"_s); return;
            }
            if (++m_nextPhysicalId == 0) { refuse(parsed->id, u"capture-failed"_s); return; }
            const bool add = preview->operation.kind == RemoteTopologyDraft::Operation::Kind::AddVirtual;
            const auto backendKey = preview->backendKey;
            if (!add) {
                const auto found = std::find_if(before.outputs.cbegin(), before.outputs.cend(), [&preview](const auto &entry) {
                    return entry.id == preview->operation.id;
                });
                if (found == before.outputs.cend() || found->output.physical
                    || found->output.owner != u"physical-console"_s
                    || !found->output.backendKey.startsWith(u"Virtual-krdp-added-"_s)) {
                    refuse(parsed->id, u"not-owner"_s); return;
                }
                if (backendKey != found->output.backendKey) { refuse(parsed->id, u"stale-output"_s); return; }
            }
            const auto expected = preview->draft;
            releaseInput();
            m_pendingVirtual = PendingVirtual{id, parsed->id, m_nextPhysicalId, m_controlGeneration,
                before, preview->priorities, expected, backendKey, add, false};
            // An in-flight creator may survive a failed broker request. Treat
            // the worker as holding a lease until exact release is verified.
            m_physicalLeaseActive = true;
            m_physicalLeaseGeneration = m_controlGeneration;
            m_physicalDeadline.start();
            const bool sent = add
                ? m_endpoint.addVirtual({m_nextPhysicalId, m_controlGeneration, backendKey,
                    preview->operation.pixels, preview->operation.scale, preview->operation.position})
                : m_endpoint.removeVirtual({m_nextPhysicalId, m_controlGeneration, backendKey});
            if (!sent) {
                // Nothing crossed the endpoint, so this dispatch did not
                // create a new lease. Preserve any earlier creator lease.
                if (!m_consoleCreatorsActive) {
                    m_physicalLeaseActive = false;
                    m_physicalLeaseGeneration = 0;
                }
                finishVirtualTopology(u"capture-failed"_s);
            }
            return;
        }
        if (!m_physicalPreview || m_physicalPreview->owner != id || m_physicalPreview->id != parsed->id
            || m_physicalPreview->token != parsed->token || !m_physicalPreview->age.isValid()
            || m_physicalPreview->age.elapsed() >= RemoteTopologyProtocol::PreviewLifetimeMs) {
            refuse(parsed->id, u"invalid"_s); return;
        }
        const auto preview = std::exchange(m_physicalPreview, std::nullopt); // One use before dispatch.
        const auto &plan = preview->plan;
        const auto &snapshot = m_topologyCatalog.snapshot();
        if (parsed->generation != plan.before.generation || snapshot.generation != plan.before.generation) {
            refuse(parsed->id, u"stale-generation"_s); return;
        }
        if (parsed->expectedRevision != plan.before.revision || snapshot.revision != plan.before.revision
            || snapshot.outputs != plan.before.outputs || m_topologyPriorities != plan.beforePriorities
            || preview->controlGeneration != m_controlGeneration) {
            refuse(parsed->id, u"stale-revision"_s); return;
        }
        if (!m_topologyAvailable || ++m_nextPhysicalId == 0) {
            refuse(parsed->id, u"capture-failed"_s); return;
        }
        const auto command = physicalCommand(plan, preview->operations, m_nextPhysicalId, m_controlGeneration);
        if (!command) { refuse(parsed->id, u"stale-output"_s); return; }
        releaseInput();
        m_pendingPhysical = PendingPhysical{id, parsed->id, command->requestId, command->controlGeneration, plan, false};
        m_physicalDeadline.start();
        if (!m_endpoint.physicalLayout(*command)) finishPhysicalTopology(u"capture-failed"_s);
        return;
    }
    if (type == u"topology-query"_s) {
        const auto requestId = RemoteTopologyProtocol::queryId(record);
        if (!requestId) {
            connection->sendControlRecord(RemoteTopologyProtocol::error(record.value(u"id"_s).toString().left(64), u"invalid"_s));
        } else if (m_pendingPhysical || m_pendingVirtual) {
            connection->sendControlRecord(RemoteTopologyProtocol::error(*requestId, u"busy"_s));
        } else if (!m_control.admitted(id) || !m_endpoint.ready()) {
            connection->sendControlRecord(RemoteTopologyProtocol::error(*requestId, u"capture-failed"_s));
        } else {
            m_pendingTopology.insert(id, *requestId);
            if (!m_endpoint.requestTopology()) {
                m_pendingTopology.remove(id);
                connection->sendControlRecord(RemoteTopologyProtocol::error(*requestId, u"capture-failed"_s));
                return;
            }
            QTimer::singleShot(6000, this, [this, id, request = *requestId] {
                if (m_pendingTopology.value(id) != request) return;
                m_pendingTopology.remove(id);
                for (const auto &client : m_clients) if (client->id == id)
                    client->connection->sendControlRecord(RemoteTopologyProtocol::error(request, u"timeout"_s));
            });
        }
        return;
    }
    if (type == u"audio-priority"_s) {
        const auto request = AudioPriority::parse(record);
        if (!request) {
            connection->sendControlRecord(AudioPriority::reply(record, false, u"invalid audio-priority request"_s));
        } else if (!m_control.admitted(id) || !m_control.ownsControl(id)) {
            connection->sendControlRecord(AudioPriority::reply(record, false, u"only the authenticated console controller may change audio priority"_s));
        } else {
            connection->setAudioPriority(request->enabled);
            connection->sendControlRecord(AudioPriority::reply(record, connection->audioPriorityActive()));
        }
        return;
    }
    if (type == u"console-resize"_s) {
        const QString requestId = record.value(u"id"_s).toString();
        const auto refuse = [connection, &requestId](const QString &error) {
            connection->sendControlRecord(QJsonObject{{u"type"_s, u"console-resize"_s}, {u"v"_s, 1}, {u"id"_s, requestId.left(64)},
                                                       {u"ok"_s, false}, {u"message"_s, error}});
        };
        const double width = record.value(u"width"_s).toDouble(0);
        const double height = record.value(u"height"_s).toDouble(0);
        const double scale = record.value(u"scale"_s).toDouble(0);
        const QString output = record.value(u"output"_s).toString();
        if (record.value(u"v"_s).toInt() != 1 || !ConsoleResize::safeToken(requestId) || requestId.size() > 64
            || !ConsoleResize::safeToken(output) || output.startsWith(u"Virtual-"_s)
            || !std::isfinite(width) || !std::isfinite(height) || width < 320 || width > 4096 || height < 200 || height > 4096
            || width != std::floor(width) || height != std::floor(height) || !std::isfinite(scale) || scale < 1 || scale > 4) {
            refuse(u"invalid physical resize request"_s);
            return;
        }
        if (!m_control.ownsControl(id) || !m_inputEnabled || !m_endpoint.ready()) {
            refuse(u"physical resize requires the active console controller"_s);
            return;
        }
        if (m_pendingResize || m_pendingPhysical || m_pendingVirtual) {
            refuse(u"another physical resize is still pending"_s);
            return;
        }
        if (std::none_of(m_outputs.monitors.cbegin(), m_outputs.monitors.cend(), [&output](const auto &monitor) { return monitor.name == output; })) {
            refuse(u"physical output is not in the active capture layout"_s);
            return;
        }
        if (m_experimentalPhysicalTopology && m_endpoint.target().adapter == ConsoleSeat::Adapter::PhysicalUser) {
            // A legacy Fit must share the same original physical baseline as
            // visual topology edits. Its separate resize helper would restore
            // asynchronously after the topology lease had already released.
            if (!m_topologyAvailable || m_topologyPriorities.isEmpty()) {
                refuse(u"physical layout capture is unavailable"_s);
                return;
            }
            const auto snapshot = m_topologyCatalog.snapshot();
            const auto found = std::find_if(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&output](const auto &entry) {
                return entry.output.backendKey == output;
            });
            if (found == snapshot.outputs.cend()) {
                refuse(u"physical output is not in the verified layout"_s);
                return;
            }
            RemoteTopologyDraft::Request draft;
            draft.generation = snapshot.generation;
            draft.expectedRevision = snapshot.revision;
            draft.owner = QString::number(id);
            draft.allowPhysicalChange = true;
            draft.operations.append({RemoteTopologyDraft::Operation::Kind::Resize, found->id,
                {}, QSize(int(width), int(height)), scale});
            const auto plan = ConsoleTopologyPlan::make(snapshot, m_topologyPriorities, draft,
                {.changePrimary = true, .maxOutputs = 16, .maxOutputDimension = 4096, .maxAtlasDimension = 8192});
            if (!plan || ++m_nextPhysicalId == 0) {
                refuse(u"physical resize conflicts with the verified layout"_s);
                return;
            }
            const auto command = physicalCommand(*plan, draft.operations, m_nextPhysicalId, m_controlGeneration);
            if (!command) {
                refuse(u"physical output changed before resize"_s);
                return;
            }
            releaseInput();
            m_physicalPreview.reset();
            m_virtualPreview.reset();
            m_pendingPhysical = PendingPhysical{id, requestId, command->requestId,
                command->controlGeneration, *plan, false, true};
            m_physicalDeadline.start();
            if (!m_endpoint.physicalLayout(*command)) finishPhysicalTopology(u"capture-failed"_s);
            return;
        }
        releaseInput();
        m_physicalPreview.reset();
        const auto serial = ++m_nextResizeId;
        m_pendingResize = PendingResize{id, requestId, serial, m_controlGeneration};
        m_resizeDeadline.start();
        if (!m_endpoint.resize({serial, m_controlGeneration, output, QSize(int(width), int(height)), scale})) {
            finishResize(u"console capture worker is unavailable"_s);
        }
        return;
    }
    if (type == u"console-control"_s) {
        const QString action = record.value(u"action"_s).toString();
        const auto refuse = [connection](const QString &code, const QString &message) {
            connection->sendControlRecord(QJsonObject{{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"request"_s, u"console-control"_s},
                                                      {u"code"_s, code}, {u"message"_s, message}});
        };
        if (record.value(u"v"_s).toInt() != 1 || (action != u"acquire"_s && action != u"release"_s)) {
            refuse(u"invalid"_s, u"console-control requires v1 and action acquire or release"_s);
            return;
        }
        if (!m_control.admitted(id)) {
            refuse(u"not-owner"_s, u"console control requires an authenticated streaming connection"_s);
            return;
        }
        if (action == u"release"_s) {
            if (!m_control.ownsControl(id)) {
                refuse(u"not-owner"_s, u"only the current controller may release control"_s);
                return;
            }
            releaseInput();
            m_control.release(id);
        } else if (!m_control.acquire(id)) {
            refuse(u"not-owner"_s, u"another client owns control; it must release control first"_s);
            return;
        }
        syncControlState();
        updateMedia();
        sendLayouts();
        connection->sendControlRecord(QJsonObject{{u"type"_s, u"console-control"_s}, {u"v"_s, 1}, {u"ok"_s, true}, {u"action"_s, action}});
        return;
    }
    if (type == u"query"_s || type == u"attach"_s || type == u"apply"_s) {
        if (record.value(u"v"_s).toInt() != 1 || (type == u"attach"_s && record.value(u"target"_s).toString() != u"physical"_s)) {
            connection->sendControlRecord(LayoutControl::errorRecord({u"invalid"_s, u"console attach requires v1 and target physical"_s}));
            return;
        }
        for (const auto &client : m_clients) {
            if (client->id == id) {
                client->wantsLayout = true;
            }
        }
        if (type == u"apply"_s) {
            // This host does not implement monitor changes yet. Explicitly
            // refuse them, then describe the existing physical desktop.
            connection->sendControlRecord(LayoutControl::errorRecord({u"unsupported"_s, u"physical console monitor changes are not available yet; use attach"_s}));
        }
        sendLayouts();
        return;
    }
    if (type != u"media"_s) {
        return;
    }
    if (!m_control.admitted(id)) {
        for (const auto &client : m_clients) {
            if (client->id == id) {
                // Bounded: keep only the latest preflight. Failed
                // authentication discards it with the client.
                client->pendingMedia = record;
                break;
            }
        }
        return;
    }
    const QJsonValue playback = record.value(QLatin1String("playback"));
    const QJsonValue microphone = record.value(QLatin1String("microphone"));
    const QJsonValue camera = record.value(QLatin1String("camera"));
    const QJsonValue silenceHost = record.value(QLatin1String("silenceHost"));
    if (!playback.isBool() || !microphone.isBool() || !camera.isBool() || (!silenceHost.isUndefined() && !silenceHost.isBool())) {
        connection->sendControlRecord(QJsonObject{{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"request"_s, u"media"_s}, {u"code"_s, u"invalid"_s}, {u"message"_s, u"media fields must be booleans"_s}});
        return;
    }
    if (camera.toBool()) {
        connection->sendControlRecord(QJsonObject{{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"request"_s, u"media"_s}, {u"code"_s, u"unsupported"_s}, {u"message"_s, u"physical console camera is not available yet"_s}});
        return;
    }
    const auto found = std::find_if(m_clients.begin(), m_clients.end(), [id](const auto &client) { return client->id == id; });
    if (found == m_clients.end()) return;
    auto &client = **found;
    if (microphone.toBool() && (!client.externalMicrophone || !m_inputEnabled || !m_endpoint.ready()
        || m_endpoint.target().adapter != ConsoleSeat::Adapter::PhysicalUser)) {
        sendMedia(client, false, u"microphone requires a ready logged-in desktop"_s);
        return;
    }
    const ConsoleControl::Media requested{playback.toBool(), silenceHost.toBool(false) && playback.toBool(), microphone.toBool()};
    if (!m_control.setMedia(id, requested)) {
        connection->sendControlRecord(QJsonObject{{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"request"_s, u"media"_s}, {u"code"_s, u"not-owner"_s}, {u"message"_s, u"only the authenticated console controller may inject microphone audio or silence the host"_s}});
        return;
    }
    if (m_microphoneClient == id) stopMicrophone({});
    client.media = requested;
    m_control.setMedia(id, requested);
    connection->setExternalAudioPlayback(requested.playback);
    connection->setMediaPolicy(requested.playback, false, false, false);
    updateMedia();
    if (!requested.microphone) {
        sendMedia(client, false);
        return;
    }
    m_microphoneClient = id;
    m_microphonePolicy = {m_controlGeneration, ++m_nextMicrophoneId, true};
    if (!m_endpoint.setMicrophone(m_microphonePolicy)) {
        stopMicrophone(u"cannot dispatch microphone startup"_s);
    } else {
        m_microphoneDeadline.start();
    }
}

void ConsoleHostController::removeClient(RdpConnection *connection)
{
    for (const auto &client : m_clients) {
        if (client->connection == connection) {
            if (m_physicalPreview && m_physicalPreview->owner == client->id) m_physicalPreview.reset();
            if (m_virtualPreview && m_virtualPreview->owner == client->id) m_virtualPreview.reset();
            if (m_pendingPhysical && m_pendingPhysical->owner == client->id) finishPhysicalTopology(u"not-owner"_s);
            if (m_pendingVirtual && m_pendingVirtual->owner == client->id) finishVirtualTopology(u"not-owner"_s);
            m_pendingTopology.remove(client->id);
            if (m_control.ownsControl(client->id)) {
                releaseInput();
            }
            m_control.remove(client->id);
        }
    }
    std::erase_if(m_clients, [connection](const auto &client) { return client->connection == connection; });
    syncControlState();
    updateMedia();
    sendLayouts();
}

void ConsoleHostController::finishTopologyQueries(const QString &error)
{
    const auto pending = std::exchange(m_pendingTopology, {});
    for (const auto &client : m_clients) {
        const QString request = pending.value(client->id);
        if (request.isEmpty()) continue;
        client->connection->sendControlRecord(error.isEmpty()
            ? consoleTopology(request)
            : RemoteTopologyProtocol::error(request, error));
    }
}

void ConsoleHostController::sendLayouts()
{
    if (m_outputs.monitors.isEmpty() || !m_endpoint.ready()) {
        return;
    }
    LayoutControl::Layout layout;
    for (const auto &output : m_outputs.monitors) {
        LayoutControl::HostMonitor monitor;
        monitor.id = monitor.name = output.name;
        monitor.kind = LayoutControl::Kind::Real;
        monitor.size = output.geometry.size() * output.scale;
        monitor.position = output.geometry.topLeft();
        monitor.scale = output.scale;
        monitor.primary = output.primary;
        layout.monitors.append(monitor);
    }
    layout.owner = m_control.owner() ? QString::number(m_control.owner()) : QString();
    layout.caps.cursorMetadata = false;
    for (const auto &client : m_clients) {
        if (client->wantsLayout && m_control.admitted(client->id)) {
            layout.you = m_control.ownsControl(client->id) ? u"owner"_s : u"viewer"_s;
            auto record = LayoutControl::layoutRecord(layout);
            record.insert(u"consoleResize"_s, true);
            client->connection->sendControlRecord(record);
        }
    }
}

void ConsoleHostController::releaseInput()
{
    const auto releases = m_inputState.releaseAll();
    if (!releases.isEmpty()) {
        qInfo() << "Releasing" << releases.size() << "held console input(s)";
    }
    for (const auto &input : releases) {
        m_endpoint.sendInput(input);
    }
}

void ConsoleHostController::syncControlState()
{
    if (m_workerOwner != m_control.owner()) {
        if (m_physicalLeaseActive) setWorkerActive(false);
        m_physicalPreview.reset();
        m_virtualPreview.reset();
        finishPhysicalTopology(u"not-owner"_s);
        finishVirtualTopology(u"not-owner"_s);
        stopMicrophone(u"console control changed; microphone disabled"_s);
        finishResize(u"console control changed during resize"_s);
        m_workerOwner = m_control.owner();
        ++m_controlGeneration;
        m_endpoint.setControlState({m_controlGeneration, m_workerOwner != 0});
        for (const auto &client : m_clients) {
            // A new controller must explicitly reapply its live preference;
            // no previous ownership period may carry a latent shared policy.
            client->connection->setAudioPriority(false);
            client->connection->videoStream()->setQualityCap(80);
            client->connection->clearAudioPriorityOverride();
            client->connection->setAudioPriorityDefault(m_audioPriorityDefault && m_control.ownsControl(client->id));
            client->videoQuality = 80;
        }
    }
}

void ConsoleHostController::setAudioPriorityDefault(bool enabled)
{
    m_audioPriorityDefault = enabled;
    for (const auto &client : m_clients) {
        client->connection->setAudioPriorityDefault(enabled && m_control.ownsControl(client->id));
    }
}

void ConsoleHostController::finishResize(const QString &error)
{
    m_resizeDeadline.stop();
    const auto pending = std::exchange(m_pendingResize, std::nullopt);
    if (!pending) {
        return;
    }
    for (const auto &client : m_clients) {
        if (client->id == pending->client) {
            client->connection->sendControlRecord(QJsonObject{{u"type"_s, u"console-resize"_s}, {u"v"_s, 1},
                                                               {u"id"_s, pending->clientRequest}, {u"ok"_s, error.isEmpty()}, {u"message"_s, error}});
            break;
        }
    }
}

void ConsoleHostController::finishPhysicalTopology(const QString &code, const QString &detail)
{
    m_physicalDeadline.stop();
    const auto pending = std::exchange(m_pendingPhysical, std::nullopt);
    if (!pending) return;
    if (!code.isEmpty()) {
        // The worker may still be inside a blocked KScreen call. Keep this
        // broker closed to stale frames/input until that worker actually dies.
        releaseInput();
        m_inputEnabled = false;
        for (const auto &client : m_clients) client->session->setWorkerActive(false);
        m_endpoint.stopWorker();
    }
    for (const auto &client : m_clients) {
        if (client->id != pending->owner) continue;
        if (pending->resizeReply) {
            client->connection->sendControlRecord(QJsonObject{{u"type"_s, u"console-resize"_s}, {u"v"_s, 1},
                {u"id"_s, pending->id}, {u"ok"_s, code.isEmpty()},
                {u"message"_s, detail.isEmpty() ? code : detail.left(1024)}});
        } else if (!code.isEmpty()) {
            auto reply = RemoteTopologyProtocol::error(pending->id, code);
            if (!detail.isEmpty()) reply.insert(u"message"_s, detail.left(1024));
            client->connection->sendControlRecord(reply);
        }
        else client->connection->sendControlRecord(QJsonObject{{u"type"_s, u"topology-result"_s},
            {u"v"_s, 1}, {u"id"_s, pending->id}, {u"ok"_s, true},
            {u"topology"_s, consoleTopology(pending->id)}});
        break;
    }
    if (code.isEmpty()) {
        sendLayouts();
        m_endpoint.requestKeyFrame(); // The verified frame was held during the transaction.
    }
}

QJsonObject ConsoleHostController::consoleTopology(const QString &id) const
{
    const bool writable = m_experimentalPhysicalTopology && m_endpoint.target().adapter == ConsoleSeat::Adapter::PhysicalUser
        && m_inputEnabled && m_topologyAvailable;
    auto record = RemoteTopologyProtocol::consoleReadOnly(id, m_topologyCatalog.snapshot(), writable);
    auto caps = record.value(u"capabilities"_s).toObject();
    const auto &outputs = m_topologyCatalog.snapshot().outputs;
    // This describes the lease inventory even for viewers/control handoff;
    // only the active authenticated controller gets write capabilities.
    const bool virtualLease = m_experimentalConsoleVirtual && m_topologyAvailable;
    caps.insert(u"consoleVirtual"_s, virtualLease);
    caps.insert(u"add"_s, writable && virtualLease && outputs.size() < 16
        && (!m_physicalLeaseActive || m_consoleCreatorsActive));
    caps.insert(u"remove"_s, writable && virtualLease && outputs.size() > 1 && m_consoleCreatorsActive
        && std::any_of(outputs.cbegin(), outputs.cend(), [](const auto &entry) {
            return !entry.output.physical && entry.output.owner == u"physical-console"_s
                && !entry.output.primary && entry.output.backendKey.startsWith(u"Virtual-krdp-added-"_s);
        }));
    caps.insert(u"multiOutputCapture"_s, writable && outputs.size() > 1);
    record.insert(u"capabilities"_s, caps);
    return record;
}

void ConsoleHostController::finishVirtualTopology(const QString &code, const QString &detail)
{
    if (!m_pendingVirtual) return;
    m_physicalDeadline.stop();
    const auto pending = std::exchange(m_pendingVirtual, std::nullopt);
    if (!code.isEmpty()) {
        releaseInput();
        m_inputEnabled = false;
        for (const auto &client : m_clients) client->session->setWorkerActive(false);
        m_endpoint.stopWorker();
    }
    for (const auto &client : m_clients) {
        if (client->id != pending->owner) continue;
        if (!code.isEmpty()) {
            auto reply = RemoteTopologyProtocol::error(pending->id, code);
            if (!detail.isEmpty()) reply.insert(u"message"_s, detail.left(1024));
            client->connection->sendControlRecord(reply);
        } else client->connection->sendControlRecord(QJsonObject{{u"type"_s, u"topology-result"_s},
            {u"v"_s, 1}, {u"id"_s, pending->id}, {u"ok"_s, true}, {u"topology"_s, consoleTopology(pending->id)}});
        break;
    }
    if (code.isEmpty()) {
        sendLayouts();
        m_endpoint.requestKeyFrame();
    }
}

void ConsoleHostController::updateMedia()
{
    const auto policy = m_control.media();
    const ConsoleWorkerWire::Media media{policy.playback, policy.silenceHost};
    if (!m_mediaConfigured || media != m_media) {
        m_media = media;
        m_mediaConfigured = true;
        // A viewer cannot disable another client's playback or private route.
        // Removing the controller restores host routing even if viewers stay.
        m_endpoint.setMedia(m_media);
    }
}

void ConsoleHostController::sendMedia(Client &client, bool microphone, const QString &error)
{
    client.connection->sendControlRecord(QJsonObject{{u"type"_s, u"media"_s}, {u"v"_s, 1}, {u"ok"_s, error.isEmpty()},
        {u"playback"_s, client.media.playback}, {u"microphone"_s, microphone}, {u"camera"_s, false},
        {u"silenceHost"_s, client.media.silenceHost}, {u"message"_s, error}});
}

void ConsoleHostController::stopMicrophone(const QString &error)
{
    m_microphoneDeadline.stop();
    m_microphonePump.stop();
    m_microphoneReady = false;
    const auto id = std::exchange(m_microphoneClient, 0);
    if (!id) return;
    for (const auto &client : m_clients) {
        if (client->id != id) continue;
        client->media.microphone = false;
        if (!m_control.ownsControl(id)) client->media.silenceHost = false;
        m_control.setMedia(id, client->media);
        client->connection->setMediaPolicy(client->media.playback, false, false, false);
        if (!error.isEmpty()) sendMedia(*client, false, error);
        break;
    }
    m_endpoint.setMicrophone({m_microphonePolicy.generation, ++m_nextMicrophoneId, false});
    m_microphonePolicy = {};
}

void ConsoleHostController::microphoneResult(const ConsoleWorkerWire::MicrophoneResult &result)
{
    if (!m_microphoneClient || result.generation != m_microphonePolicy.generation
        || result.requestId != m_microphonePolicy.requestId) return;
    if (!result.error.isEmpty()) { stopMicrophone(result.error); return; }
    if (m_microphoneReady) return;
    if (!m_inputEnabled || !m_control.ownsControl(m_microphoneClient)) {
        stopMicrophone(u"console microphone authority changed"_s);
        return;
    }
    for (const auto &client : m_clients) {
        if (client->id != m_microphoneClient) continue;
        m_microphoneDeadline.stop();
        client->connection->setMediaPolicy(client->media.playback, true, false, false);
        m_microphoneReady = true;
        m_microphonePump.start();
        sendMedia(*client, true);
        break;
    }
}
}
