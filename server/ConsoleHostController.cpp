// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleHostController.h"
#include "DisplayWakePolicy.h"
#include "ChromaMerge.h"

#include <QScopeGuard>
#include "AudioPriority.h"
#include "CursorTracker.h"

#include <algorithm>
#include <csignal>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

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
#include <StatsRequest.h>

#include "ConsoleAdmission.h"
#include "ConsoleSeat.h"
#include "ConsoleWorkerSession.h"
#include "ConsoleResize.h"
#include "ConsoleFrameLayout.h"
#include "RemoteTopologyProtocol.h"
#include "CameraAvailability.h"

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
    m_uidOf = [](RdpConnection *connection) -> std::optional<quint32> {
        return connection ? connection->authenticatedUserUid() : std::nullopt;
    };
    m_refuse = [](RdpConnection *connection, quint32 errorInfo) {
        // A Set Error Info PDU, so mstsc, stock FreeRDP and Remmina show a
        // reason; no KRDPCTL channel is needed. AUD-FIX F4: it must go out
        // before the Deactivate All that close() sends, or the client reports
        // 0xC LOGOFF_BY_USER instead (as the virtual broker already does).
        connection->closeWithErrorInfo(errorInfo);
    };
    m_drainDeadline.setSingleShot(true);
    m_drainDeadline.setInterval(DrainDeadlineMs);
    connect(&m_drainDeadline, &QTimer::timeout, this, [this] {
        if (!m_workerAlive) return;
        qWarning().noquote() << "Console worker" << m_workerSocket << "did not stop within" << DrainDeadlineMs << "ms; sending SIGTERM";
        if (m_launchWorker.signal) m_launchWorker.signal(m_workerSocket, SIGTERM);
        m_killDeadline.start();
    });
    m_killDeadline.setSingleShot(true);
    m_killDeadline.setInterval(KillDeadlineMs);
    connect(&m_killDeadline, &QTimer::timeout, this, [this] {
        if (!m_workerAlive) return;
        qWarning().noquote() << "Console worker" << m_workerSocket << "ignored SIGTERM for" << KillDeadlineMs << "ms; sending SIGKILL";
        if (m_launchWorker.signal) m_launchWorker.signal(m_workerSocket, SIGKILL);
    });
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &ConsoleHostController::retryWorker);
    m_experimentalPhysicalTopology = qEnvironmentVariableIntValue("FARSIDE_EXPERIMENTAL_CONSOLE_TOPOLOGY") == 1;
    m_experimentalConsoleVirtual = qEnvironmentVariableIntValue("FARSIDE_EXPERIMENTAL_CONSOLE_VIRTUAL") == 1;
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
        m_configuredConsoleOutputs = false;
        m_physicalLeaseGeneration = 0;
    });
    const auto virtualFinished = [this](quint64 requestId, quint64 generation, const QString &error, bool add) {
        if (!m_pendingVirtual || m_pendingVirtual->serial != requestId
            || m_pendingVirtual->controlGeneration != generation || m_pendingVirtual->ownedMutation
            || m_pendingVirtual->add != add) return;
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
        stopMicrophone(DeviceControl::Timeout, u"microphone worker startup timed out"_s);
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::microphoneFinished, this, &ConsoleHostController::microphoneResult);
    m_cameraDeadline.setSingleShot(true);
    m_cameraDeadline.setInterval(5000);
    connect(&m_cameraDeadline, &QTimer::timeout, this, [this] {
        stopCamera(DeviceControl::Timeout, u"camera worker startup timed out"_s);
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::cameraFinished, this, &ConsoleHostController::cameraResult);
    connect(&m_endpoint, &ConsoleWorkerEndpoint::cameraDemand, this, [this](const auto &demand) {
        if (!m_cameraClient || !m_cameraReady || demand.generation != m_cameraPolicy.generation
            || demand.requestId != m_cameraPolicy.requestId) return;
        for (const auto &client : m_clients) {
            if (client->id != m_cameraClient || !client->connection) continue;
            if (!m_control.ownsControl(client->id) || !m_inputEnabled) {
                stopCamera(DeviceControl::Revoked, u"console camera authority changed"_s);
                return;
            }
            client->connection->setExternalCameraState(true, demand.capture, demand.inUse);
            if (m_cameraInUse != demand.inUse) {
                m_cameraInUse = demand.inUse;
                sendRecord(client->connection, DeviceControl::stateRecord(MediaDevice::Camera,
                    {DeviceStatus::State::On, demand.inUse, {}, {}}));
            }
            return;
        }
    });
    m_microphonePump.setInterval(20);
    connect(&m_microphonePump, &QTimer::timeout, this, [this] {
        if (!m_microphoneReady || !m_inputEnabled || !m_control.ownsControl(m_microphoneClient)) return;
        for (const auto &client : m_clients) {
            if (client->id != m_microphoneClient || !client->connection) continue;
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
        if (m_pendingVirtual && m_pendingVirtual->ownedMutation && !m_pendingVirtual->managedFit
            && result.requestId == m_pendingVirtual->serial && result.generation == m_pendingVirtual->controlGeneration) {
            if (!result.error.isEmpty()) { finishVirtualTopology(u"partial"_s, result.error); return; }
            m_pendingVirtual->waitingReadback = true;
            if (!m_endpoint.requestTopology()) finishVirtualTopology(u"capture-failed"_s);
            return;
        }
        if (m_pendingResize && result.requestId == m_pendingResize->requestId && result.generation == m_pendingResize->generation) {
            finishResize(result.error);
        }
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::managedFitFinished, this, [this](const auto &result) {
        if (!m_pendingVirtual || !m_pendingVirtual->ownedMutation || !m_pendingVirtual->managedFit
            || result.requestId != m_pendingVirtual->serial || result.generation != m_pendingVirtual->controlGeneration) return;
        if (!result.error.isEmpty()) { finishVirtualTopology(u"partial"_s, result.error); return; }
        m_pendingVirtual->waitingReadback = true;
        if (!m_endpoint.requestTopology()) finishVirtualTopology(u"capture-failed"_s);
    });
    connect(m_server, &Server::newConnectionCreated, this, &ConsoleHostController::addClient);
    connect(&m_endpoint, &ConsoleWorkerEndpoint::workerAuthenticated, this, [this](const auto &) {
        syncDisplayPolicy(); // Wake can be necessary before capture proves Ready.
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::workerReady, this, [this](const auto &target) {
        apply(m_handoff.workerReady(target));
        m_backoff.reset();
        m_failedTarget = {};
        qInfo() << "Console worker ready:" << target.sessionId << "forwarding" << m_inputEnabled;
        m_endpoint.setControlState({m_controlGeneration, m_control.owner() != 0});
        armConfiguredConsoleOutputs();
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
        stopMicrophone(DeviceControl::Unavailable, u"console microphone worker stopped"_s);
        stopCamera(DeviceControl::Unavailable, u"console camera worker stopped"_s);
        finishResize(u"console capture worker stopped during resize"_s);
        // A closed socket is not a reaped process: the worker may still be
        // restoring outputs. The handoff advances only on workerExited().
        stopCurrentWorker();
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [this](const VideoFrame &frame) {
        if (!m_inputEnabled || m_pendingPhysical || m_pendingVirtual || m_layoutAwaitingReadback) {
            return;
        }
        const bool workspace = capturePolicy().mode == MonitorCapturePolicy::Mode::Workspace;
        if (m_outputs.monitors.size() > 1 || (m_topologyAvailable && !m_topologyComplete)) {
            const bool confirmed = workspace
                ? ConsoleFrameLayout::workspaceConfirmed(frame, m_outputs, m_topologyCatalog.snapshot())
                : ConsoleFrameLayout::confirmed(frame, m_outputs, m_topologyCatalog.snapshot());
            if (!m_topologyAvailable || !confirmed) return;
        } else if (frame.monitors.size() > 1) return;
        for (const auto &client : m_clients) {
            // AUD-C-1: the session thread can mark a stream enabled before the
            // main-thread authenticated owner UID check has admitted it. Only admitted clients
            // may see the console, not even its monitor layout.
            if (!client->connection || !m_control.admitted(client->id)) continue;
            const QVector<VideoMonitor> desired = !workspace && frame.monitors.size() > 1 ? frame.monitors : QVector<VideoMonitor>{};
            if (client->wireLayout != desired) {
                if (!frame.isKeyFrame || (desired.isEmpty() && !m_topologyAvailable && !client->wireLayout.isEmpty())) continue;
                client->connection->videoStream()->setMonitorLayout(desired);
                client->wireLayout = desired;
            }
            client->session->submitFrame(frame);
        }
    });
    // FIX-CURSOR: the console's cursor shape as RDP pointer updates, to every admitted client.
    connect(&m_endpoint, &ConsoleWorkerEndpoint::cursorShapeReceived, this, [this](const ConsoleWorkerWire::CursorShape &shape) {
        for (const auto &client : m_clients) {
            if (client->connection && m_control.admitted(client->id)) CursorTracker::apply(*client->connection->cursor(), shape);
        }
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::audioReceived, this, [this](const ConsoleWorkerWire::Audio &audio) {
        for (const auto &client : m_clients) {
            // Desktop audio is console content too (AUD-C-1).
            if (client->connection && m_control.admitted(client->id)) client->connection->submitExternalAudio(audio.pcm);
        }
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [this](const ConsoleWorkerWire::Outputs &outputs) {
        qInfo() << "Console capture outputs:" << outputs.monitors.size() << "forwarding" << m_inputEnabled << "clients" << m_clients.size();
        const bool changed = outputs != m_outputs;
        const bool wasMulti = m_outputs.monitors.size() > 1;
        const auto capture = capturePolicy();
        const bool selected = capture.mode == MonitorCapturePolicy::Mode::Primary
            || capture.mode == MonitorCapturePolicy::Mode::Specific || m_configuredConsoleOutputs;
        const bool independentTransition = ((changed && (outputs.monitors.size() > 1 || wasMulti)) || selected)
            && !m_pendingPhysical && !m_pendingVirtual;
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
        if (independentTransition) {
            releaseInput(); // Held keys/buttons must not carry across coordinate systems.
            m_layoutAwaitingReadback = true;
        }
        if (!m_pendingPhysical && !m_pendingVirtual && !m_layoutAwaitingReadback) sendLayouts();
        if (independentTransition) m_endpoint.requestTopology();
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::topologyReceived, this, [this](const ConsoleWorkerWire::Topology &topology) {
        const auto capture = capturePolicy();
        const bool selected = capture.mode == MonitorCapturePolicy::Mode::Primary
            || capture.mode == MonitorCapturePolicy::Mode::Specific;
        const bool ownedProjection = m_configuredConsoleOutputs && std::all_of(topology.outputs.cbegin(), topology.outputs.cend(), [](const auto &output) {
            return !output.physical;
        });
        if (topology.outputs.isEmpty() || topology.outputs.size() != m_outputs.monitors.size()
            || (m_configuredConsoleOutputs && (!ownedProjection || topology.complete))
            || (!topology.complete && !ownedProjection && (!selected || topology.outputs.size() != 1))) {
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
        m_topologyComplete = m_topologyAvailable && topology.complete;
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
                && snapshot.revision == pending.before.revision + (pending.draft.before != pending.draft.after ? 1 : 0)
                && snapshot.outputs.size() == pending.draft.after.size();
            if (matches) {
                QSet<QString> survivors;
                for (const auto &entry : pending.before.outputs) {
                    if (entry.output.backendKey != pending.backendKey) survivors.insert(entry.output.backendKey);
                }
                matches = priorityOrder(pending.priorities, survivors) == priorityOrder(m_topologyPriorities, survivors);
            }
            if (matches && pending.ownedMutation) {
                matches = configuredOutputTopology() && snapshot.outputs == pending.draft.after
                    && m_topologyPriorities == pending.priorities;
            } else if (matches && pending.add) {
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
    connect(&m_endpoint, &ConsoleWorkerEndpoint::versionMismatch, this, [](quint16 version) {
        qWarning().noquote() << "Console worker speaks wire version" << version << "but this broker speaks"
                             << ConsoleWorkerWire::ProtocolVersion << "- install the paired broker and worker together";
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::protocolError, this, [this](const QString &message) {
        qWarning().noquote() << "Console worker protocol error:" << message;
        setWorkerActive(false);
        stopCurrentWorker();
        m_endpoint.close(); // The worker sees its socket close and exits; its reaping retries with backoff.
    });
}

ConsoleHostController::~ConsoleHostController()
{
    stopMicrophone();
    stopCamera();
}

void ConsoleHostController::start()
{
    apply(m_handoff.reconcile(m_sessions));
}

void ConsoleHostController::setUidResolver(UidResolver resolver)
{
    m_uidOf = std::move(resolver);
}

void ConsoleHostController::setRefuse(Refuse refuse)
{
    m_refuse = std::move(refuse);
}

void ConsoleHostController::setSeatSessions(const QList<ConsoleSeat::Session> &sessions)
{
    m_sessions = sessions;
    // Admission first: an account that may not see the new seat state must be
    // gone before a worker for it can become ready.
    enforceAdmission();
    apply(m_handoff.reconcile(m_sessions));
    syncDisplayPolicy();
}

void ConsoleHostController::workerExited(const QString &socketName)
{
    // The socket path identifies this launch, including retries for the same
    // logind session. A late exit from an old worker must not stop its successor.
    if (!m_workerAlive || socketName != m_workerSocket) {
        return;
    }
    qInfo().noquote() << "Console worker" << socketName << "exited";
    workerFailed();
}

void ConsoleHostController::workerFailed()
{
    // The current launch is gone: reaped, or never started. Only now may a
    // replacement start, so two workers never drive the same seat (AUD-C-7).
    const auto target = m_handoff.startingTarget().valid() ? m_handoff.startingTarget() : m_handoff.activeTarget();
    const bool intentional = m_handoff.draining();
    m_workerAlive = false;
    m_workerSocket.clear();
    m_drainDeadline.stop();
    m_killDeadline.stop();
    setWorkerActive(false);
    m_endpoint.close();
    removeWorkerDirectory();
    clearPhysicalLease("console worker exited");
    apply(m_handoff.workerStopped());
    if (!intentional && !m_workerAlive) {
        // Crash, failed launch or wire-version mismatch: never relaunch the
        // same target in a tight loop (AUD-C-6).
        m_failedTarget = target;
        const auto step = m_backoff.next();
        if (step.newLevel) {
            qWarning().noquote() << "Console worker for session" << target.sessionId << "failed; retrying in" << step.delayMs << "ms";
        }
        m_retryTimer.start(step.delayMs);
    }
}

void ConsoleHostController::retryWorker()
{
    if (m_workerAlive) {
        return;
    }
    if (std::exchange(m_deferredStart, false) && m_handoff.startingTarget().valid()) {
        startWorker(m_handoff.startingTarget());
        return;
    }
    apply(m_handoff.reconcile(m_sessions));
}

void ConsoleHostController::stopCurrentWorker()
{
    m_endpoint.stopWorker(); // Delivered at once, or right after the worker authenticates.
    if (m_workerAlive) {
        if (!m_drainDeadline.isActive() && !m_killDeadline.isActive()) {
            m_drainDeadline.start();
        }
        return;
    }
    // Nothing is running (a launch deferred by backoff, or no launcher): the
    // drain is already over. Deferred, so callers never re-enter apply().
    QTimer::singleShot(0, this, [this] {
        if (!m_workerAlive && m_handoff.draining()) {
            apply(m_handoff.workerStopped());
        }
    });
}

void ConsoleHostController::removeWorkerDirectory()
{
    if (m_workerDirectory.isEmpty()) {
        return;
    }
    QDir(m_workerDirectory).removeRecursively();
    m_workerDirectory.clear();
}

void ConsoleHostController::clearPhysicalLease(const char *why)
{
    // A dead worker holds no compositor lease: its temporary outputs die
    // with its screencast connection, and the next worker replays the
    // output-restore journal before it reports Ready (AUD-C-5). Keeping the
    // lease would refuse every later worker, the greeter included.
    if (m_physicalLeaseActive || m_consoleCreatorsActive) {
        qInfo() << "Clearing Console physical lease:" << why;
    }
    m_physicalLeaseActive = false;
    m_consoleCreatorsActive = false;
    m_configuredConsoleOutputs = false;
    m_physicalLeaseGeneration = 0;
}

bool ConsoleHostController::admissible(const Client &client) const
{
    const auto seat = ConsoleHandoff::targetFor(m_sessions);
    const auto session = ConsoleSeat::find(m_sessions, seat.sessionId);
    return ConsoleAdmission::allowed(seat, session && session->locked, client.uid);
}

void ConsoleHostController::enforceAdmission()
{
    QList<ConsoleControl::Id> evicted;
    for (const auto &client : m_clients) {
        if (m_control.admitted(client->id) && !admissible(*client)) {
            evicted.append(client->id);
        }
    }
    for (const auto id : evicted) {
        evictClient(id, u"the physical console now shows another account's unlocked desktop"_s);
    }
}

void ConsoleHostController::evictClient(ConsoleControl::Id id, const QString &message)
{
    const auto found = std::find_if(m_clients.cbegin(), m_clients.cend(), [id](const auto &client) { return client->id == id; });
    if (found == m_clients.cend()) {
        return;
    }
    const QPointer<RdpConnection> connection = (*found)->connection;
    qWarning().noquote() << "Refusing console client" << id << "(uid" << ((*found)->uid ? QString::number(*(*found)->uid) : u"none"_s) << "):" << message;
    if (connection) {
        if (connection->hasControlChannel()) {
            // Own client only: a readable reason in its UI. Never required.
            connection->sendControlRecord(QJsonObject{{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"request"_s, u"console"_s},
                                                      {u"code"_s, u"not-owner"_s}, {u"message"_s, message}});
        }
        // The standard path every RDP client understands: disconnect with
        // ERRINFO_SERVER_INSUFFICIENT_PRIVILEGES. Queued: Server deletes the
        // connection when it closes; never inside its own emission or our loops.
        QMetaObject::invokeMethod(this, [this, connection] {
            if (connection && m_refuse) m_refuse(connection, ERRINFO_SERVER_INSUFFICIENT_PRIVILEGES);
        }, Qt::QueuedConnection);
    }
    removeClient(connection.data(), id);
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
        stopCurrentWorker();
    }
    if (actions.startWorker) {
        startWorker(actions.target);
    }
    if (actions.grantInput) {
        setWorkerActive(true);
    }
    if (actions.resetGraphics) {
        for (const auto &client : m_clients) {
            if (client->connection) client->connection->videoStream()->reset();
        }
    }
    if (actions.requestKeyFrame) {
        m_endpoint.requestKeyFrame();
    }
}

void ConsoleHostController::startWorker(const ConsoleHandoff::Target &target)
{
    if (m_workerAlive) {
        // Unreachable through the handoff (it waits for workerExited), but a
        // second worker on the same seat must never be launched (AUD-C-7).
        qWarning() << "Not starting a console worker while the previous one has not exited";
        stopCurrentWorker();
        return;
    }
    if (m_retryTimer.isActive() && target == m_failedTarget) {
        m_deferredStart = true; // Backoff still running for this very target.
        return;
    }
    if (!(target == m_failedTarget)) {
        m_backoff.reset();
        m_retryTimer.stop();
        m_deferredStart = false;
        m_failedTarget = {};
    }
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
    QString error;
    // One private directory per launch, mode 0700 and owned by the worker's
    // uid: no other local account can reach, pre-empt or replace the socket
    // (AUD-C-9). The endpoint additionally checks SO_PEERCRED and the token.
    const QString directory = QDir(m_runtimeDirectory)
                                  .filePath(QStringLiteral("seat0-%1-%2").arg(target.sessionId).arg(QRandomGenerator::system()->generate64(), 0, 16));
    if (m_runtimeDirectory.isEmpty() || !QDir().mkpath(m_runtimeDirectory) || ::mkdir(QFile::encodeName(directory).constData(), 0700) != 0
        || (geteuid() == 0 && ::chown(QFile::encodeName(directory).constData(), target.uid, gid_t(-1)) != 0)) {
        qWarning().noquote() << "Cannot create console worker directory" << directory;
        workerFailed();
        return;
    }
    m_workerDirectory = directory;
    const QString socketName = QDir(directory).filePath(QStringLiteral("worker.sock"));
    QByteArray token;
    token.reserve(32);
    for (int i = 0; i < 4; ++i) {
        const quint64 value = QRandomGenerator::system()->generate64();
        for (int byte = 0; byte < 8; ++byte) {
            token.append(char(value >> (byte * 8)));
        }
    }
    if (!m_endpoint.listen(socketName, target, token, &error)) {
        qWarning().noquote() << "Cannot create console worker endpoint:" << error;
        workerFailed();
        return;
    }
    if (!m_launchWorker.launch || !m_launchWorker.launch(target, m_endpoint.socketName(), token, &error)) {
        qWarning().noquote() << "Cannot launch console worker for session" << target.sessionId << ':' << error;
        workerFailed();
        return;
    }
    m_workerAlive = true;
    m_workerSocket = m_endpoint.socketName();
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
        stopMicrophone(DeviceControl::Revoked, u"console session changed; microphone consent must be renewed"_s);
        stopCamera(DeviceControl::Revoked, u"console session changed; camera consent must be renewed"_s);
        finishResize(u"console capture worker changed during resize"_s);
        releaseInput();
    }
    m_inputEnabled = active;
    for (const auto &client : m_clients) {
        client->session->setWorkerActive(active);
    }
    if (active) {
        for (const auto &client : m_clients) {
            startStandardMicrophone(*client);
            startStandardCamera(*client);
        }
    }
}

void ConsoleHostController::addClient(RdpConnection *connection)
{
    // Shared viewers use AVC420. A sole admitted controller may use its saved
    // AVC preference or private codec; the bound worker's actual probe gates444.
    connection->videoStream()->setCodecPreference(CodecPreference::Avc420);
    connection->videoStream()->setAvc444Available(false);
    if (m_videoHost) {
        connection->videoStream()->setEncoderPolicy(m_videoHost->probe.encoders, m_videoHost->mode);
        connection->videoStream()->setAv1TilesSetting(m_videoHost->av1Tiles);
    }
    connection->videoStream()->setQualityCap(m_qualityCap);
    // Keep the configured quality cap; adaptation is explicitly configured.
    connection->videoStream()->setAdaptiveQuality(m_adaptiveQuality);
    auto client = std::make_unique<Client>();
    const auto id = client->id = ++m_nextClientId;
    client->connection = connection;
    client->videoQuality = m_qualityCap;
    client->externalMicrophone = connection->enableExternalMicrophone();
    client->externalCamera = connection->enableExternalCamera();
    client->session = std::make_unique<ConsoleWorkerSession>([this, id](const ConsoleWorkerWire::Input &input) {
        if (m_inputEnabled && !m_pendingPhysical && !m_pendingVirtual && !m_layoutAwaitingReadback
            && m_control.ownsControl(id)) {
            m_endpoint.sendInput(input);
            m_inputState.record(input);
        }
    });
    client->session->setWorkerActive(m_inputEnabled);
    client->codec = std::make_unique<WorkerCodecBridge>(connection->videoStream(), client->session.get());
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
                                       &m_endpoint, [this]() {
        syncDisplayPolicy();
        m_endpoint.requestKeyFrame();
    }, Qt::QueuedConnection));
    client->connections.append(connect(client->session.get(), &AbstractSession::frameReceived, connection->videoStream(), &VideoStream::queueFrame));
    client->connections.append(connect(client->session.get(), &ConsoleWorkerSession::keyFrameRequested, &m_endpoint, &ConsoleWorkerEndpoint::requestKeyFrame));
    client->connections.append(connect(connection->inputHandler(), &InputHandler::inputEvent, client->session.get(), &AbstractSession::sendEvent));
    // Queued: a record can still be in the event queue after Server deleted
    // the connection. Only a live connection that is still our client counts.
    const QPointer<RdpConnection> guarded(connection);
    client->connections.append(connect(connection, &RdpConnection::controlRecordReceived, this, [this, guarded, id](const QJsonObject &record) {
        if (!guarded || std::none_of(m_clients.cbegin(), m_clients.cend(), [id](const auto &entry) { return entry->id == id; })) {
            return;
        }
        onControlRecord(guarded, id, record);
    }, Qt::QueuedConnection));
    client->connections.append(connect(connection, &RdpConnection::externalCameraFormat, this,
                                       [this, guarded, id](quint64 epoch, quint32 width, quint32 height, quint32 fps) {
        if (!guarded || epoch != guarded->externalCameraEpoch() || m_cameraClient != id || !m_cameraPolicy.enabled) return;
        if (!m_endpoint.setCameraFormat({m_cameraPolicy.generation, m_cameraPolicy.requestId, width, height, fps}))
            stopCamera(DeviceControl::Unavailable, u"cannot dispatch camera format"_s);
    }, Qt::QueuedConnection));
    client->connections.append(connect(connection, &RdpConnection::externalCameraFrame, this,
                                       [this, guarded, id](quint64 epoch, const QByteArray &jpeg) {
        if (!guarded) return;
        if (epoch == guarded->externalCameraEpoch() && m_cameraClient == id && m_cameraReady && m_control.ownsControl(id) && m_inputEnabled)
            m_endpoint.sendCameraFrame({m_cameraPolicy.generation, m_cameraPolicy.requestId, jpeg});
        guarded->acknowledgeExternalCameraFrame(epoch);
    }, Qt::QueuedConnection));
    // Server erases (deletes) the connection from its own Closed slot, which
    // runs before ours; Qt then skips our Closed slot. `destroyed` always runs
    // and is where a disconnected client really leaves (AUD-C-4).
    client->connections.append(connect(connection, &QObject::destroyed, this, [this, id] {
        removeClient(nullptr, id);
    }));
    // The connection's own channel work fails after the broker said `on`: the
    // client refused AUDIN, or never joined RDPSND. Pass it on.
    client->connections.append(connect(connection, &RdpConnection::deviceState, this,
                                       [this, id](MediaDevice device, const DeviceStatus &status, const QString &) {
        if (status.state != DeviceStatus::State::Error) return;
        if (device == MediaDevice::Microphone && m_microphoneClient == id) {
            for (const auto &client : m_clients) {
                if (client->id == id) client->standardMicrophone = false; // a standard refusal simply ends it
            }
            stopMicrophone(status.code, status.message);
            return;
        }
        if (device == MediaDevice::Camera && m_cameraClient == id) {
            for (const auto &client : m_clients) if (client->id == id) client->standardCamera = false;
            stopCamera(status.code, status.message);
            return;
        }
        for (const auto &client : m_clients) {
            if (client->id == id && client->connection && device == MediaDevice::Playback && client->media.playback)
                sendRecord(client->connection, DeviceControl::stateRecord(device, status));
        }
    }, Qt::QueuedConnection));
    // Post-authentication (AUD-S1): KRDPCTL clients hear `capabilities` first; others nothing.
    client->connections.append(connect(connection, &RdpConnection::clientDisplayInfoReceived, this, [this, id] {
        for (const auto &client : m_clients) {
            if (client->id == id) {
                updateClientDisplayPolicy(*client);
                sendCapabilities(*client);
            }
        }
    }, Qt::QueuedConnection));
    client->connections.append(connect(connection, &RdpConnection::stateChanged, this, [this, connection, id](RdpConnection::State state) {
        if (state == RdpConnection::State::Streaming) {
            // Running is pre-authentication. Media preflight can also arrive
            // before Streaming; defer it without granting any authority.
            const auto self = std::find_if(m_clients.begin(), m_clients.end(), [id](const auto &entry) { return entry->id == id; });
            if (self == m_clients.end()) return;
            (*self)->uid = m_uidOf(connection);
            if (!admissible(**self)) {
                evictClient(id, u"this account may not use the physical console now: an unlocked desktop belongs to its own user"_s);
                return;
            }
            loadUserSettings(**self);
            m_control.admit(id);
            if (const auto shape = m_endpoint.cursorShape()) CursorTracker::apply(*connection->cursor(), *shape);
            syncControlState();
            sendLayouts();
            // Frames were withheld until now; start this client on a key frame.
            m_endpoint.requestKeyFrame();
            for (const auto &client : m_clients) {
                if (client->id == id && !client->pendingDevices.isEmpty()) {
                    const auto pending = std::exchange(client->pendingDevices, {});
                    const QPointer<RdpConnection> guarded(connection);
                    for (const auto &record : pending) {
                        if (!guarded) break;
                        onControlRecord(connection, id, record); // replayed whole, requestId included
                    }
                    break;
                }
            }
            // StandardClientMedia: a client without KRDPCTL now; a channel client
            // once the first-record gate has passed without a known record.
            const QPointer<RdpConnection> admitted(connection);
            if (admitted && !admitted->hasControlChannel()) {
                applyStandardMedia(id);
            } else if (admitted) {
                QTimer::singleShot(m_standardGateMs, this, [this, id] { applyStandardMedia(id); });
            }
        }
        if (state == RdpConnection::State::Closed) {
            removeClient(connection);
        }
    }));
    m_clients.push_back(std::move(client));
}

void ConsoleHostController::loadUserSettings(Client &client)
{
    if (client.preferencesLoaded || !client.connection || !m_userSettingsReader) return;
    const auto uid = m_uidOf(client.connection);
    if (!uid || !*uid) return;
    client.preferencesLoaded = true;
    const auto result = m_userSettingsReader(*uid);
    if (!result.error.isEmpty()) {
        qWarning() << "Console user preferences rejected for uid" << *uid << result.error;
        return;
    }
    client.preferences = result.preferences;
    updateClientDisplayPolicy(client);
    client.codec->setChromaPolicy(client.preferences.chroma.value_or(ChromaPolicy{}));
    if (const auto capture = MonitorCapturePolicy::parse(client.preferences.monitorMode.value_or(u"multi"_s),
                                                        client.preferences.monitorIndex.value_or(0)))
        client.codec->setCapturePolicy(*capture);
    auto *stream = client.connection->videoStream();
    client.videoQuality = client.preferences.quality.value_or(m_qualityCap);
    stream->setQualityCap(client.videoQuality);
    stream->setAdaptiveQuality(client.preferences.adaptiveQuality.value_or(m_adaptiveQuality));
    if (m_videoHost) {
        stream->setEncoderPolicy(m_videoHost->probe.encoders, client.preferences.softwareEncoding.value_or(m_videoHost->mode));
        stream->setAv1TilesSetting(client.preferences.av1Tiles.value_or(m_videoHost->av1Tiles));
    }
}

void ConsoleHostController::updateClientDisplayPolicy(Client &client)
{
    // RDP display information can arrive before authentication. Only this
    // connection's admitted identity/preferences may supply a policy; the
    // bridge transmits it only while bound to the current control generation.
    if (!client.preferencesLoaded || !client.connection || !client.codec) return;
    const auto uid = m_uidOf(client.connection);
    if (!uid || !*uid || (client.uid && client.uid != uid)) return;
    const auto &p = client.preferences;
    const auto policy = ConsoleVirtualOutputPolicy::parse(p.monitorMode == std::optional(u"virtual"_s),
        p.virtualMonitorPolicy.value_or(u"replace"_s), p.virtualMonitorLayout.value_or(u"client"_s),
        p.virtualMonitorFallbackSize.value_or(QSize(1920, 1080)), client.connection->clientDisplayInfo());
    if (policy) client.codec->setConsoleVirtualPolicy(*policy);
}

bool ConsoleHostController::configuredOutputTopology() const
{
    const auto &outputs = m_topologyCatalog.snapshot().outputs;
    return m_configuredConsoleOutputs && m_physicalLeaseActive && m_topologyAvailable && !m_topologyComplete
        && !m_layoutAwaitingReadback && m_endpoint.target().adapter == ConsoleSeat::Adapter::PhysicalUser
        && m_physicalLeaseGeneration == m_controlGeneration && !outputs.isEmpty()
        && std::all_of(outputs.cbegin(), outputs.cend(), [](const auto &entry) {
            return !entry.output.physical && entry.output.enabled && entry.output.owner == u"physical-console"_s;
        });
}

bool ConsoleHostController::previewOwnedTopology(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &record)
{
    if (!m_configuredConsoleOutputs) return false;
    const bool managedFit = record.value(u"type"_s) == u"topology-fit-preview"_s;
    const QString request = record.value(u"id"_s).toString().left(64);
    const auto refuse = [this, connection, &request](const QString &code) {
        replyTo(connection, RemoteTopologyProtocol::error(request, code));
        return true;
    };
    QString generation, outputId;
    quint64 revision = 0;
    QSize pixels;
    double scale = 1;
    QVector<RemoteTopologyFit::Relation> relations;
    if (managedFit) {
        const auto parsed = RemoteTopologyProtocol::fitPreviewRequest(record);
        if (!parsed) return refuse(u"invalid"_s);
        generation = parsed->generation; revision = parsed->expectedRevision; outputId = parsed->output;
        pixels = parsed->pixels; scale = parsed->scale; relations = parsed->relations;
    } else {
        const auto parsed = RemoteTopologyProtocol::previewRequest(record);
        if (!parsed || parsed->draft.allowPhysicalChange || parsed->draft.allowRemoval
            || parsed->draft.operations.size() != 1
            || parsed->draft.operations.first().kind != RemoteTopologyDraft::Operation::Kind::Resize) return refuse(u"unsupported"_s);
        const auto &operation = parsed->draft.operations.first();
        generation = parsed->draft.generation; revision = parsed->draft.expectedRevision;
        outputId = operation.id; pixels = operation.pixels; scale = operation.scale;
    }
    if (!m_control.ownsControl(id) || !m_inputEnabled || !m_endpoint.ready()) return refuse(u"not-owner"_s);
    if (!configuredOutputTopology() || m_topologyPriorities.isEmpty()) return refuse(u"capture-failed"_s);
    if (m_pendingVirtual || m_pendingPhysical || m_pendingResize) return refuse(u"busy"_s);
    const auto snapshot = m_topologyCatalog.snapshot();
    if (generation != snapshot.generation) return refuse(u"stale-generation"_s);
    if (revision != snapshot.revision) return refuse(u"stale-revision"_s);
    const auto selected = std::find_if(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&outputId](const auto &entry) { return entry.id == outputId; });
    if (selected == snapshot.outputs.cend()) return refuse(u"stale-output"_s);
    if (pixels.width() < 320 || pixels.height() < 200 || pixels.width() > 4096 || pixels.height() > 4096
        || pixels.width() % 2 || pixels.height() % 2 || !std::isfinite(scale) || scale < 1 || scale > 4) return refuse(u"invalid"_s);
    RemoteTopologyDraft::Request base;
    base.generation = generation; base.expectedRevision = revision; base.owner = u"physical-console"_s;
    const auto fit = RemoteTopologyFit::planInWorkspace(snapshot,
        {.moveVirtual = true, .resizeVirtual = true, .maxOutputs = 16, .maxOutputDimension = 4096, .maxAtlasDimension = 8192},
        base, outputId, pixels, scale, relations);
    if (!fit.valid()) return refuse(fit.error);
    for (const auto &entry : fit.proposal.after) {
        const auto &geometry = entry.output.logicalGeometry;
        if (geometry.x() < 0 || geometry.y() < 0 || qint64(geometry.x()) + geometry.width() > 32768
            || qint64(geometry.y()) + geometry.height() > 32768) return refuse(u"limit"_s);
    }
    QString token;
    for (int i = 0; i < 4; ++i) token += QString::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, QLatin1Char('0'));
    VirtualPreview preview{id, m_controlGeneration, request, token, snapshot, m_topologyPriorities, fit.proposal,
        {.kind = RemoteTopologyDraft::Operation::Kind::Resize, .id = outputId, .position = {}, .pixels = pixels, .scale = scale},
        selected->output.backendKey, {}, true, managedFit, {}};
    for (const auto &relation : relations) {
        const auto backend = [&snapshot](const QString &stableId) {
            const auto found = std::find_if(snapshot.outputs.cbegin(), snapshot.outputs.cend(), [&stableId](const auto &entry) { return entry.id == stableId; });
            return found == snapshot.outputs.cend() ? QString{} : found->output.backendKey;
        };
        const auto parent = backend(relation.parent), child = backend(relation.child);
        if (parent.isEmpty() || child.isEmpty()) return refuse(u"stale-output"_s);
        preview.fitRelations.append({parent, child, quint8(relation.edge), relation.offset});
    }
    preview.age.start();
    m_physicalPreview.reset();
    m_virtualPreview = std::move(preview);
    replyTo(connection, RemoteTopologyProtocol::previewReply(request, token, fit.proposal, snapshot, u"lease"_s,
        QJsonArray{u"temporary-console-output"_s}));
    return true;
}

void ConsoleHostController::onControlRecord(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &incoming)
{
    // KRDPCTL v2: requestId off before the strict parsers, echoed on the replies.
    QJsonObject record = incoming;
    const auto requestId = LayoutControl::takeRequestId(record);
    if (requestId.invalid) {
        connection->sendControlRecord(LayoutControl::invalidRequestIdRecord());
        return;
    }
    const QPointer<ConsoleHostController> alive(this);
    const QString outerRequestId = std::exchange(m_replyRequestId, requestId.value);
    const auto restoreRequestId = qScopeGuard([alive, outerRequestId] {
        if (alive) alive->m_replyRequestId = outerRequestId;
    });
    const QString type = record.value(u"type"_s).toString();
    {
        // StandardClientMedia is for clients that do not speak KRDPCTL: a
        // `device` record, or any other record this host knows (not
        // audio-priority, which never closes krdpserver's gate either).
        static const QSet<QString> known{u"topology-preview"_s, u"topology-commit"_s, u"topology-query"_s, u"console-resize"_s,
                                         u"console-control"_s, u"query"_s, u"attach"_s, u"apply"_s, u"codec"_s, u"stats"_s, u"chroma"_s};
        for (const auto &client : m_clients) {
            if (client->id != id) continue;
            if (type == u"device"_s) client->deviceRecordSeen = true;
            if (known.contains(type)) client->spokeKrdpctl = true;
            break;
        }
    }
    if (type == u"chroma"_s) {
        const auto found = std::find_if(m_clients.begin(), m_clients.end(), [id](const auto &entry) { return entry->id == id; });
        if (found == m_clients.end()) return;
        auto &client = **found;
        if (!m_control.admitted(id) || !m_control.ownsControl(id) || !admissible(client)) {
            replyTo(connection, LayoutControl::errorRecord({u"not-owner"_s, u"only the admitted console controller may change chroma policy"_s}));
            return;
        }
        const auto merged = ChromaMerge::merge(client.codec->chromaPolicy(), LayoutControl::chromaFromJson(record));
        if (merged.outcome == ChromaMerge::Outcome::Applied) client.codec->setChromaPolicy(merged.policy);
        replyTo(connection, ChromaMerge::brokerReply(merged));
        return;
    }
    if (type == u"codec"_s) {
        // AUD-FIX7: the connection's codec preference (CodecRequest, as krdpserver). It runs
        // while this client controls the console alone (syncCodecPolicy()); otherwise the answer
        // is AVC and a `codec` push follows when that changes.
        const auto parsed = CodecRequest::parse(record);
        if (!parsed) {
            replyTo(connection, CodecRequest::invalidRecord());
            return;
        }
        auto found = std::find_if(m_clients.begin(), m_clients.end(), [id](const auto &entry) { return entry->id == id; });
        if (found == m_clients.end()) return;
        auto &client = **found;
        if (!m_videoHost) {
            replyTo(connection, LayoutControl::codecRecord(u"avc"_s, std::nullopt, u"this console streams AVC only"_s));
            return;
        }
        client.codecRequest = *parsed;
        client.codecApplied = false;
        int admitted = 0;
        for (const auto &entry : m_clients) {
            if (entry->connection && m_control.admitted(entry->id)) ++admitted;
        }
        auto *stream = connection->videoStream();
        if (m_control.ownsControl(id) && admitted == 1 && m_control.admitted(id)) {
            QString log;
            replyTo(connection, CodecRequest::apply(*stream, *parsed, &log));
            client.codecApplied = true;
            qInfo().noquote() << "Console client" << id << log;
            return;
        }
        // Not (yet) alone in control: AVC now; the policy starts when it is (a push).
        stream->setPrivateCodecPolicy({}, parsed->adaptive);
        replyTo(connection, LayoutControl::codecRecord(u"avc"_s, stream->encoderPolicy().avc.hardware,
            m_control.admitted(id) ? u"another client is watching this console: AVC for everyone"_s
                                   : u"the console codec is chosen once this client controls it alone"_s));
        return;
    }
    if (type == u"stats"_s) {
        // KRDPCTL stats (StatsRequest, as krdpserver): this connection's own stream. A viewer may
        // subscribe too; its samples describe what it receives.
        const auto parsed = StatsRequest::parse(record);
        if (!parsed) {
            replyTo(connection, StatsRequest::invalidRecord());
            return;
        }
        QString log;
        replyTo(connection, StatsRequest::apply(*connection->videoStream(), *parsed, &log));
        qDebug().noquote() << "Console client" << id << log;
        return;
    }
    if ((type == u"topology-preview"_s || type == u"topology-fit-preview"_s)
        && previewOwnedTopology(connection, id, record)) return;
    if (type == u"topology-preview"_s) {
        const auto parsed = RemoteTopologyProtocol::previewRequest(record);
        const QString request = record.value(u"id"_s).toString().left(64);
        const auto refuse = [this, connection](const QString &requestId, const QString &code) {
            replyTo(connection, RemoteTopologyProtocol::error(requestId, code));
        };
        if (!parsed) { refuse(request, u"invalid"_s); return; }
        if (!m_experimentalPhysicalTopology || !m_topologyComplete || m_endpoint.target().adapter != ConsoleSeat::Adapter::PhysicalUser) {
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
            replyTo(connection, RemoteTopologyProtocol::previewReply(parsed->id, token, proposed,
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
        replyTo(connection, RemoteTopologyProtocol::previewReply(parsed->id, token,
            {plan->before.outputs, plan->after, {}}, snapshot, u"lease"_s,
            QJsonArray{u"physical-output-change"_s}));
        return;
    }
    if (type == u"topology-commit"_s) {
        const auto parsed = RemoteTopologyProtocol::commitRequest(record);
        const QString request = record.value(u"id"_s).toString().left(64);
        const auto refuse = [this, connection](const QString &requestId, const QString &code) {
            replyTo(connection, RemoteTopologyProtocol::error(requestId, code));
        };
        if (!parsed) { refuse(request, u"invalid"_s); return; }
        if ((!configuredOutputTopology() && (!m_experimentalPhysicalTopology || !m_topologyComplete))
            || m_endpoint.target().adapter != ConsoleSeat::Adapter::PhysicalUser) {
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
            if (m_configuredConsoleOutputs && !preview->ownedMutation) {
                refuse(parsed->id, u"unsupported"_s); return;
            }
            if (++m_nextPhysicalId == 0) { refuse(parsed->id, u"capture-failed"_s); return; }
            if (preview->ownedMutation) {
                if (!configuredOutputTopology()) { refuse(parsed->id, u"not-owner"_s); return; }
                const auto selected = std::find_if(before.outputs.cbegin(), before.outputs.cend(), [&preview](const auto &entry) {
                    return entry.id == preview->operation.id && entry.output.backendKey == preview->backendKey;
                });
                if (selected == before.outputs.cend() || selected->output.physical || selected->output.owner != u"physical-console"_s) {
                    refuse(parsed->id, u"not-owner"_s); return;
                }
                releaseInput();
                m_pendingVirtual = PendingVirtual{id, parsed->id, m_nextPhysicalId, m_controlGeneration,
                    before, preview->priorities, preview->draft, preview->backendKey, false, false, true, preview->managedFit};
                m_physicalDeadline.start();
                const bool sent = preview->managedFit
                    ? m_endpoint.managedFit({m_nextPhysicalId, m_controlGeneration, preview->backendKey,
                        preview->operation.pixels, preview->operation.scale, preview->fitRelations})
                    : m_endpoint.resize({m_nextPhysicalId, m_controlGeneration, preview->backendKey,
                        preview->operation.pixels, preview->operation.scale});
                if (!sent) finishVirtualTopology(u"capture-failed"_s);
                return;
            }
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
        if (m_configuredConsoleOutputs) { refuse(parsed->id, u"invalid"_s); return; }
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
            replyTo(connection, RemoteTopologyProtocol::error(record.value(u"id"_s).toString().left(64), u"invalid"_s));
        } else if (m_pendingPhysical || m_pendingVirtual) {
            replyTo(connection, RemoteTopologyProtocol::error(*requestId, u"busy"_s));
        } else if (!m_control.admitted(id) || !m_endpoint.ready()) {
            replyTo(connection, RemoteTopologyProtocol::error(*requestId, u"capture-failed"_s));
        } else {
            m_pendingTopology.insert(id, *requestId);
            if (!m_endpoint.requestTopology()) {
                m_pendingTopology.remove(id);
                replyTo(connection, RemoteTopologyProtocol::error(*requestId, u"capture-failed"_s));
                return;
            }
            QTimer::singleShot(6000, this, [this, id, request = *requestId] {
                if (m_pendingTopology.value(id) != request) return;
                m_pendingTopology.remove(id);
                for (const auto &client : m_clients) if (client->id == id)
                    replyTo(client->connection, RemoteTopologyProtocol::error(request, u"timeout"_s));
            });
        }
        return;
    }
    if (type == u"audio-priority"_s) {
        const auto request = AudioPriority::parse(record);
        if (!request) {
            replyTo(connection, AudioPriority::reply(record, false, u"invalid audio-priority request"_s));
        } else if (!m_control.admitted(id) || !m_control.ownsControl(id)) {
            replyTo(connection, AudioPriority::reply(record, false, u"only the authenticated console controller may change audio priority"_s));
        } else {
            connection->setAudioPriority(request->enabled);
            replyTo(connection, AudioPriority::reply(record, connection->audioPriorityActive()));
        }
        return;
    }
    if (type == u"console-resize"_s) {
        const QString requestId = record.value(u"id"_s).toString();
        const auto refuse = [this, connection, &requestId](const QString &error) {
            replyTo(connection, QJsonObject{{u"type"_s, u"console-resize"_s}, {u"v"_s, 1}, {u"id"_s, requestId.left(64)},
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
        if (m_experimentalPhysicalTopology && m_topologyComplete && m_endpoint.target().adapter == ConsoleSeat::Adapter::PhysicalUser) {
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
        const auto refuse = [this, connection](const QString &code, const QString &message) {
            replyTo(connection, QJsonObject{{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"request"_s, u"console-control"_s},
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
        replyTo(connection, QJsonObject{{u"type"_s, u"console-control"_s}, {u"v"_s, 1}, {u"ok"_s, true}, {u"action"_s, action}});
        return;
    }
    if (type == u"query"_s || type == u"attach"_s || type == u"apply"_s) {
        if (record.value(u"v"_s).toInt() != 1 || (type == u"attach"_s && record.value(u"target"_s).toString() != u"physical"_s)) {
            replyTo(connection, LayoutControl::errorRecord({u"invalid"_s, u"console attach requires v1 and target physical"_s}));
            return;
        }
        for (const auto &client : m_clients) {
            if (client->id == id) {
                client->wantsLayout = true;
                client->layoutRequestId = m_replyRequestId; // the next `layout` answers it
            }
        }
        if (type == u"apply"_s) {
            // This host does not implement monitor changes yet. Explicitly
            // refuse them, then describe the existing physical desktop.
            replyTo(connection, LayoutControl::errorRecord({u"unsupported"_s, u"physical console monitor changes are not available yet; use attach"_s}));
        }
        sendLayouts();
        return;
    }
    if (type == u"device"_s) {
        onControlDevice(connection, id, record, incoming);
    }
}

namespace
{
/** Device controls implemented by the Console broker and logged-in worker. */
const LayoutControl::DeviceCapabilities ConsoleDeviceCapabilities{
    .playbackToggle = true,
    .playbackSilenceHost = true,
    .microphoneToggle = true,
    .cameraToggle = true,
    .cameraReselect = true,
};
}

DeviceStatus ConsoleHostController::deviceStatus(const Client &client, MediaDevice device) const
{
    using State = DeviceStatus::State;
    switch (device) {
    case MediaDevice::Playback:
        return {client.media.playback ? State::On : State::Off, false, {}, {}};
    case MediaDevice::Microphone:
        if (m_microphoneClient != client.id) return {};
        return {m_microphoneReady ? State::On : State::Starting, false, {}, {}};
    case MediaDevice::Camera:
        if (m_cameraClient != client.id) return {};
        return {m_cameraReady ? State::On : State::Starting, m_cameraInUse, {}, {}};
    }
    return {};
}

void ConsoleHostController::onControlDevice(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &record, const QJsonObject &incoming)
{
    const auto found = std::find_if(m_clients.begin(), m_clients.end(), [id](const auto &client) { return client->id == id; });
    if (found == m_clients.end()) return;
    auto &client = **found;
    if (!m_control.admitted(id)) {
        // Before admission (Streaming): keep only the latest record per device,
        // replayed whole (requestId included). Failed authentication discards
        // them with the client.
        const QJsonValue device = record.value(u"device"_s);
        client.pendingDevices.erase(std::remove_if(client.pendingDevices.begin(), client.pendingDevices.end(), [&device](const QJsonObject &pending) {
            return pending.value(u"device"_s) == device;
        }), client.pendingDevices.end());
        if (client.pendingDevices.size() < 3) client.pendingDevices.append(incoming);
        return;
    }
    const auto parsed = DeviceControl::parseRequest(record);
    if (const auto *error = std::get_if<LayoutControl::Error>(&parsed)) {
        replyTo(connection, LayoutControl::errorRecord(*error));
        return;
    }
    const auto request = std::get<DeviceControl::Request>(parsed);
    const auto deviceCapabilities = CameraAvailability::capabilities(ConsoleDeviceCapabilities,
        CameraAvailability::reason(m_server->cameraLoopbackDevice()));
    if (const auto refused = DeviceControl::checkSupported(request, deviceCapabilities)) {
        replyTo(connection, LayoutControl::errorRecord(*refused));
        return;
    }
    if (request.action == DeviceControl::Action::Query) {
        replyTo(connection, DeviceControl::stateRecord(request.device, deviceStatus(client, request.device)));
        return;
    }
    const bool on = request.action == DeviceControl::Action::On;
    using State = DeviceStatus::State;
    if (request.device == MediaDevice::Playback) {
        auto media = client.media;
        media.playback = on;
        media.silenceHost = on && request.silenceHost;
        if (!m_control.setMedia(id, media)) {
            replyTo(connection, LayoutControl::errorRecord({u"not-owner"_s, u"only the client controlling this console can silence its speakers"_s}));
            return;
        }
        client.media = media;
        connection->setExternalAudioPlayback(media.playback);
        connection->setDeviceEnabled(MediaDevice::Playback, media.playback);
        updateMedia();
        replyTo(connection, DeviceControl::stateRecord(MediaDevice::Playback, deviceStatus(client, MediaDevice::Playback)));
        return;
    }
    if (request.device == MediaDevice::Camera) {
        if (request.action == DeviceControl::Action::Off) {
            if (m_cameraClient == id) {
                client.cameraRequestId.clear();
                stopCamera();
            }
            replyTo(connection, DeviceControl::stateRecord(MediaDevice::Camera, {}));
            return;
        }
        if (m_cameraClient && m_cameraClient != id) {
            replyTo(connection, DeviceControl::stateRecord(MediaDevice::Camera,
                {State::Error, false, DeviceControl::Busy, u"another client is sharing its camera with this console"_s}));
            return;
        }
        if (!m_control.ownsControl(id)) {
            replyTo(connection, LayoutControl::errorRecord({u"not-owner"_s, u"only the client controlling this console can share a camera"_s}));
            return;
        }
        if (!client.externalCamera || !m_inputEnabled || !m_endpoint.ready()
            || m_endpoint.target().adapter != ConsoleSeat::Adapter::PhysicalUser) {
            replyTo(connection, DeviceControl::stateRecord(MediaDevice::Camera,
                {State::Error, false, DeviceControl::Unavailable, u"camera requires a ready logged-in desktop"_s}));
            return;
        }
        if (m_cameraClient == id) {
            client.cameraRequestId.clear();
            stopCamera();
        }
        startCamera(client, m_replyRequestId);
        return;
    }
    // Microphone.
    if (!on) {
        if (m_microphoneClient == id) {
            client.microphoneRequestId.clear(); // superseded: this `off` is the answer
            stopMicrophone();
        }
        replyTo(connection, DeviceControl::stateRecord(MediaDevice::Microphone, {}));
        return;
    }
    if (m_microphoneClient && m_microphoneClient != id) {
        replyTo(connection, DeviceControl::stateRecord(MediaDevice::Microphone,
            {State::Error, false, DeviceControl::Busy, u"another client is sharing its microphone with this console"_s}));
        return;
    }
    if (!m_control.ownsControl(id)) {
        replyTo(connection, LayoutControl::errorRecord({u"not-owner"_s, u"only the client controlling this console can share a microphone"_s}));
        return;
    }
    if (!client.externalMicrophone || !m_inputEnabled || !m_endpoint.ready()
        || m_endpoint.target().adapter != ConsoleSeat::Adapter::PhysicalUser) {
        replyTo(connection, DeviceControl::stateRecord(MediaDevice::Microphone,
            {State::Error, false, DeviceControl::Unavailable, u"microphone requires a ready logged-in desktop"_s}));
        return;
    }
    if (m_microphoneClient == id) {
        client.microphoneRequestId.clear(); // superseded by this `on`
        stopMicrophone(); // a new consent period
    }
    auto media = client.media;
    media.microphone = true;
    if (!m_control.setMedia(id, media)) {
        replyTo(connection, LayoutControl::errorRecord({u"not-owner"_s, u"only the client controlling this console can share a microphone"_s}));
        return;
    }
    client.media = media;
    dispatchMicrophone(client, m_replyRequestId);
}

void ConsoleHostController::dispatchMicrophone(Client &client, const QString &requestId)
{
    updateMedia();
    client.microphoneRequestId = requestId; // the worker's acknowledgement answers this request
    m_microphoneClient = client.id;
    m_microphonePolicy = {m_controlGeneration, ++m_nextMicrophoneId, true};
    if (!m_endpoint.setMicrophone(m_microphonePolicy)) {
        stopMicrophone(DeviceControl::Unavailable, u"cannot dispatch microphone startup"_s);
    } else {
        m_microphoneDeadline.start();
    }
}

void ConsoleHostController::applyStandardMedia(ConsoleControl::Id id)
{
    const auto found = std::find_if(m_clients.begin(), m_clients.end(), [id](const auto &client) { return client->id == id; });
    if (found == m_clients.end()) return;
    auto &client = **found;
    const QPointer<RdpConnection> connection = client.connection;
    if (!connection || client.deviceRecordSeen || client.spokeKrdpctl || !client.preferences.standardClientMedia.value_or(true)) return;
    // Only the controlling client: a viewer never gets a microphone, and a
    // stock viewer cannot ask for playback either (KRDPCTL clients can).
    if (!m_control.admitted(id) || !m_control.ownsControl(id)) return;
    const auto channels = m_standardChannels ? m_standardChannels(connection) : connection->standardMediaChannels();
    if (!channels) return;
    qInfo() << "StandardClientMedia: console client" << id << "playback" << channels->playback << "microphone" << channels->dynamic;
    if (channels->playback && !client.media.playback) {
        auto media = client.media;
        media.playback = true;
        media.silenceHost = false; // never silence the host without a request
        if (m_control.setMedia(id, media)) {
            client.media = media;
            connection->setExternalAudioPlayback(true);
            connection->setDeviceEnabled(MediaDevice::Playback, true);
            updateMedia();
        }
    }
    client.standardMicrophone = channels->dynamic;
    client.standardCamera = channels->dynamic;
    startStandardMicrophone(client);
    startStandardCamera(client);
}

void ConsoleHostController::startStandardMicrophone(Client &client)
{
    // `busy`/`not-owner`/`unavailable` rules as for a `device` request; a
    // desktop that is not ready yet (greeter, lock) is retried when it is.
    if (!client.standardMicrophone || !client.connection || m_microphoneClient || !m_control.ownsControl(client.id)
        || !client.externalMicrophone || !m_inputEnabled || !m_endpoint.ready()
        || m_endpoint.target().adapter != ConsoleSeat::Adapter::PhysicalUser) return;
    auto media = client.media;
    media.microphone = true;
    if (!m_control.setMedia(client.id, media)) return;
    client.media = media;
    dispatchMicrophone(client, {});
}

void ConsoleHostController::removeClient(RdpConnection *connection, ConsoleControl::Id id)
{
    // Take the client out first: nothing below may send to a connection that
    // is closing or already destroyed (its QPointer is then null).
    std::vector<std::unique_ptr<Client>> removed;
    for (auto it = m_clients.begin(); it != m_clients.end();) {
        auto &client = *it;
        if ((id && client->id == id) || (connection && client->connection == connection) || !client->connection) {
            removed.push_back(std::move(client));
            it = m_clients.erase(it);
        } else {
            ++it;
        }
    }
    if (removed.empty()) {
        return;
    }
    for (const auto &client : removed) {
        for (const auto &handle : std::as_const(client->connections)) {
            disconnect(handle);
        }
        if (m_physicalPreview && m_physicalPreview->owner == client->id) m_physicalPreview.reset();
        if (m_virtualPreview && m_virtualPreview->owner == client->id) m_virtualPreview.reset();
        if (m_pendingPhysical && m_pendingPhysical->owner == client->id) finishPhysicalTopology(u"not-owner"_s);
        if (m_pendingVirtual && m_pendingVirtual->owner == client->id) finishVirtualTopology(u"not-owner"_s);
        if (m_pendingResize && m_pendingResize->client == client->id) finishResize(u"console client disconnected"_s);
        m_pendingTopology.remove(client->id);
        if (m_control.ownsControl(client->id)) {
            releaseInput();
        }
        m_control.remove(client->id);
    }
    syncControlState();
    updateMedia();
    sendLayouts();
}

void ConsoleHostController::finishTopologyQueries(const QString &error)
{
    const auto pending = std::exchange(m_pendingTopology, {});
    for (const auto &client : m_clients) {
        const QString request = pending.value(client->id);
        // Queries are accepted only from admitted clients; never answer one
        // that is no longer admitted with the console's monitor topology.
        if (request.isEmpty() || !client->connection || !m_control.admitted(client->id)) continue;
        replyTo(client->connection, error.isEmpty()
            ? consoleTopology(request, client->id)
            : RemoteTopologyProtocol::error(request, error));
    }
}

void ConsoleHostController::sendRecord(RdpConnection *connection, const QJsonObject &record)
{
    if (!connection) return;
    if (m_recordSent) m_recordSent(connection, record);
    connection->sendControlRecord(record);
}

void ConsoleHostController::replyTo(RdpConnection *connection, const QJsonObject &record)
{
    if (!connection) return;
    if (!m_replyRequestId.isEmpty() || record.contains(u"requestId"_s)) {
        sendRecord(connection, LayoutControl::withRequestId(record, m_replyRequestId));
        return;
    }
    // An asynchronous result names its request by `id` (v2 clients send requestId == id).
    const QJsonValue id = record.value(u"id"_s);
    sendRecord(connection, id.isString() ? LayoutControl::withRequestId(record, id.toString()) : record);
}

void ConsoleHostController::sendCapabilities(Client &client)
{
    if (client.capabilitiesSent || !client.connection || !client.connection->isAuthenticated() || !client.connection->hasControlChannel()) return;
    loadUserSettings(client);
    client.capabilitiesSent = true;
    LayoutControl::ChannelCapabilities capabilities;
    capabilities.host = u"console"_s;
    capabilities.layoutQuery = true; // `query`/`attach` describe the physical desktop; `apply` is refused
    capabilities.topologyQuery = true;
    const auto capture = capturePolicy();
    const bool fullCapture = capture.mode != MonitorCapturePolicy::Mode::Primary && capture.mode != MonitorCapturePolicy::Mode::Specific;
    capabilities.topologyPreview = fullCapture && (m_experimentalPhysicalTopology || m_experimentalConsoleVirtual);
    capabilities.topologyApply = capabilities.topologyPreview;
    capabilities.devices = CameraAvailability::capabilities(ConsoleDeviceCapabilities,
        CameraAvailability::reason(m_server->cameraLoopbackDevice()));
    if (m_videoHost) capabilities.video = EncoderSupport::videoCapabilities(m_videoHost->probe, client.preferences.softwareEncoding.value_or(m_videoHost->mode));
    capabilities.stats = LayoutControl::StatsCapabilities{};
    client.connection->sendControlRecord(LayoutControl::capabilitiesRecord(capabilities));
}

void ConsoleHostController::sendLayouts()
{
    if (m_outputs.monitors.isEmpty() || !m_endpoint.ready() || m_layoutAwaitingReadback
        || m_pendingPhysical || m_pendingVirtual) {
        return;
    }
    LayoutControl::Layout layout;
    for (const auto &output : m_outputs.monitors) {
        LayoutControl::HostMonitor monitor;
        monitor.id = monitor.name = output.name;
        monitor.kind = m_configuredConsoleOutputs ? LayoutControl::Kind::Virtual : LayoutControl::Kind::Real;
        monitor.size = output.geometry.size() * output.scale;
        monitor.position = output.geometry.topLeft();
        monitor.scale = output.scale;
        monitor.primary = output.primary;
        layout.monitors.append(monitor);
    }
    layout.owner = m_control.owner() ? QString::number(m_control.owner()) : QString();
    // FIX-CURSOR: the worker forwards the console's cursor shape (Cursor records), so the
    // default caps.cursorMetadata = true is now true here too.
    for (const auto &client : m_clients) {
        if (client->connection && client->wantsLayout && m_control.admitted(client->id)) {
            layout.you = m_control.ownsControl(client->id) ? u"owner"_s : u"viewer"_s;
            auto record = LayoutControl::layoutRecord(layout);
            record.insert(u"consoleResize"_s, !m_configuredConsoleOutputs);
            client->connection->sendControlRecord(LayoutControl::withRequestId(record, std::exchange(client->layoutRequestId, {})));
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
        stopMicrophone(DeviceControl::Revoked, u"console control changed; microphone disabled"_s);
        stopCamera(DeviceControl::Revoked, u"console control changed; camera disabled"_s);
        finishResize(u"console control changed during resize"_s);
        m_workerOwner = m_control.owner();
        ++m_controlGeneration;
        m_endpoint.setControlState({m_controlGeneration, m_workerOwner != 0});
        for (const auto &client : m_clients) {
            // A new controller must explicitly reapply its live preference;
            // no previous ownership period may carry a latent shared policy.
            client->videoQuality = client->preferences.quality.value_or(m_qualityCap);
            client->connection->setAudioPriority(false);
            client->connection->videoStream()->setQualityCap(client->videoQuality);
            client->connection->videoStream()->setAdaptiveQuality(client->preferences.adaptiveQuality.value_or(m_adaptiveQuality));
            client->codec->setChromaPolicy(client->preferences.chroma.value_or(ChromaPolicy{}));
            client->connection->clearAudioPriorityOverride();
            client->connection->setAudioPriorityDefault(client->preferences.preferAudioQuality.value_or(m_audioPriorityDefault)
                && m_control.ownsControl(client->id));
        }
        // AUD-FIX7: the new controller's codec bridge steers the worker under the new generation.
        for (const auto &client : m_clients) {
            if (!client->codec) continue;
            if (m_workerOwner && m_control.ownsControl(client->id)) client->codec->bind(&m_endpoint, m_controlGeneration);
            else client->codec->unbind();
        }
        armConfiguredConsoleOutputs();
    }
    syncCodecPolicy();
    syncDisplayPolicy();
}

void ConsoleHostController::armConfiguredConsoleOutputs()
{
    if (m_configuredConsoleOutputs || m_endpoint.target().adapter != ConsoleSeat::Adapter::PhysicalUser) return;
    for (const auto &client : m_clients) {
        if (!m_control.ownsControl(client->id) || !client->codec || !client->codec->consoleVirtualPolicy().enabled) continue;
        m_configuredConsoleOutputs = true;
        m_consoleCreatorsActive = true;
        m_physicalLeaseActive = true;
        m_physicalLeaseGeneration = m_controlGeneration;
        m_layoutAwaitingReadback = true;
        m_topologyAvailable = false;
        m_topologyComplete = false;
        m_topologyPriorities.clear();
        m_topologyCatalog.resetGeneration();
        return;
    }
}

ConsoleWorkerWire::DisplayPolicy ConsoleHostController::displayPolicy() const
{
    QVector<DisplayViewer> viewers;
    for (const auto &client : m_clients) {
        viewers.append({client->connection && m_control.admitted(client->id) && admissible(*client),
                        client->connection && client->connection->videoStream()->enabled(),
                        client->preferences.wakeDisplayOnConnect.value_or(true)});
    }
    return displayPolicyFor(viewers);
}

void ConsoleHostController::syncDisplayPolicy()
{
    const auto policy = displayPolicy();
    m_endpoint.setDisplayPolicy(policy.active, policy.wakeEnabled);
}

MonitorCapturePolicy ConsoleHostController::capturePolicy() const
{
    for (const auto &client : m_clients)
        if (client->codec && m_control.ownsControl(client->id)) return client->codec->capturePolicy();
    return {};
}

void ConsoleHostController::syncCodecPolicy()
{
    int admitted = 0;
    for (const auto &client : m_clients) {
        if (client->connection && m_control.admitted(client->id)) ++admitted;
    }
    for (const auto &client : m_clients) {
        if (!client->connection || !client->codec) continue;
        const bool owner = m_control.ownsControl(client->id);
        const bool eligible = m_videoHost && client->codecRequest && owner && admitted == 1 && m_control.admitted(client->id);
        auto *stream = client->connection->videoStream();
        const bool alone = owner && admitted == 1 && m_control.admitted(client->id) && admissible(*client);
        stream->setCodecPreference(alone ? client->preferences.codec.value_or(CodecPreference::Auto) : CodecPreference::Avc420);
        if (eligible && !client->codecApplied) {
            QString log;
            const auto record = CodecRequest::apply(*stream, *client->codecRequest, &log);
            client->codecApplied = true;
            qInfo().noquote() << "Console client" << client->id << log;
            sendRecord(client->connection, record); // unsolicited: it is alone and in control now
        } else if (!eligible && client->codecApplied) {
            client->codecApplied = false;
            stream->setPrivateCodecPolicy({}, client->codecRequest ? client->codecRequest->adaptive : true);
            const QString reason = owner ? u"another client is watching this console: AVC for everyone"_s
                                         : u"only the controlling client chooses the console's codec"_s;
            qInfo().noquote() << "Console client" << client->id << "back to AVC:" << reason;
            sendRecord(client->connection, LayoutControl::codecRecord(u"avc"_s, stream->encoderPolicy().avc.hardware, reason));
        }
    }
}

void ConsoleHostController::setAudioPriorityDefault(bool enabled)
{
    m_audioPriorityDefault = enabled;
    for (const auto &client : m_clients) {
        client->connection->setAudioPriorityDefault(client->preferences.preferAudioQuality.value_or(enabled) && m_control.ownsControl(client->id));
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
            replyTo(client->connection, QJsonObject{{u"type"_s, u"console-resize"_s}, {u"v"_s, 1},
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
        stopCurrentWorker();
    }
    for (const auto &client : m_clients) {
        if (client->id != pending->owner) continue;
        if (pending->resizeReply) {
            replyTo(client->connection, QJsonObject{{u"type"_s, u"console-resize"_s}, {u"v"_s, 1},
                {u"id"_s, pending->id}, {u"ok"_s, code.isEmpty()},
                {u"message"_s, detail.isEmpty() ? code : detail.left(1024)}});
        } else if (!code.isEmpty()) {
            auto reply = RemoteTopologyProtocol::error(pending->id, code);
            if (!detail.isEmpty()) reply.insert(u"message"_s, detail.left(1024));
            replyTo(client->connection, reply);
        }
        else replyTo(client->connection, QJsonObject{{u"type"_s, u"topology-result"_s},
            {u"v"_s, 1}, {u"id"_s, pending->id}, {u"ok"_s, true},
            {u"topology"_s, consoleTopology(pending->id, pending->owner)}});
        break;
    }
    if (code.isEmpty()) {
        sendLayouts();
        m_endpoint.requestKeyFrame(); // The verified frame was held during the transaction.
    }
}

QJsonObject ConsoleHostController::consoleTopology(const QString &id, ConsoleControl::Id requester) const
{
    const bool writable = m_experimentalPhysicalTopology && m_endpoint.target().adapter == ConsoleSeat::Adapter::PhysicalUser
        && m_control.ownsControl(requester) && m_inputEnabled && m_topologyAvailable && m_topologyComplete;
    auto record = RemoteTopologyProtocol::consoleReadOnly(id, m_topologyCatalog.snapshot(), writable);
    auto caps = record.value(u"capabilities"_s).toObject();
    const auto &outputs = m_topologyCatalog.snapshot().outputs;
    // This describes the lease inventory even for viewers/control handoff;
    // only the active authenticated controller gets write capabilities.
    const bool virtualLease = m_experimentalConsoleVirtual && m_topologyAvailable && m_topologyComplete;
    const bool owned = configuredOutputTopology();
    const bool ownedWritable = owned && m_control.ownsControl(requester) && m_inputEnabled && m_endpoint.ready();
    caps.insert(u"consoleVirtual"_s, virtualLease || owned);
    caps.insert(u"consoleOwned"_s, owned);
    if (owned) {
        caps.insert(u"resize"_s, ownedWritable);
        caps.insert(u"scale"_s, ownedWritable);
    }
    caps.insert(u"add"_s, writable && virtualLease && outputs.size() < 16
        && (!m_physicalLeaseActive || m_consoleCreatorsActive));
    caps.insert(u"remove"_s, writable && virtualLease && outputs.size() > 1 && m_consoleCreatorsActive
        && std::any_of(outputs.cbegin(), outputs.cend(), [](const auto &entry) {
            return !entry.output.physical && entry.output.owner == u"physical-console"_s
                && !entry.output.primary && entry.output.backendKey.startsWith(u"Virtual-krdp-added-"_s);
        }));
    caps.insert(u"multiOutputCapture"_s, (writable || owned) && outputs.size() > 1);
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
        stopCurrentWorker();
    }
    for (const auto &client : m_clients) {
        if (client->id != pending->owner) continue;
        if (!code.isEmpty()) {
            auto reply = RemoteTopologyProtocol::error(pending->id, code);
            if (!detail.isEmpty()) reply.insert(u"message"_s, detail.left(1024));
            replyTo(client->connection, reply);
        } else replyTo(client->connection, QJsonObject{{u"type"_s, u"topology-result"_s},
            {u"v"_s, 1}, {u"id"_s, pending->id}, {u"ok"_s, true}, {u"topology"_s, consoleTopology(pending->id, pending->owner)}});
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

void ConsoleHostController::sendMicrophoneState(Client &client, const DeviceStatus &status)
{
    if (!client.connection) return;
    sendRecord(client.connection, LayoutControl::withRequestId(DeviceControl::stateRecord(MediaDevice::Microphone, status),
                                                               std::exchange(client.microphoneRequestId, {})));
}

void ConsoleHostController::stopMicrophone(const QString &code, const QString &message)
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
        if (client->connection) client->connection->setDeviceEnabled(MediaDevice::Microphone, false);
        if (!code.isEmpty() || !client->microphoneRequestId.isEmpty()) {
            const auto state = code.isEmpty() || code == DeviceControl::Revoked ? DeviceStatus::State::Off : DeviceStatus::State::Error;
            sendMicrophoneState(*client, {state, false, code, message});
        }
        break;
    }
    m_endpoint.setMicrophone({m_microphonePolicy.generation, ++m_nextMicrophoneId, false});
    m_microphonePolicy = {};
}

void ConsoleHostController::microphoneResult(const ConsoleWorkerWire::MicrophoneResult &result)
{
    if (!m_microphoneClient || result.generation != m_microphonePolicy.generation
        || result.requestId != m_microphonePolicy.requestId) return;
    if (!result.error.isEmpty()) { stopMicrophone(DeviceControl::Unavailable, result.error); return; }
    if (m_microphoneReady) return;
    if (!m_inputEnabled || !m_control.ownsControl(m_microphoneClient)) {
        stopMicrophone(DeviceControl::Revoked, u"console microphone authority changed"_s);
        return;
    }
    for (const auto &client : m_clients) {
        if (client->id != m_microphoneClient) continue;
        m_microphoneDeadline.stop();
        client->connection->setDeviceEnabled(MediaDevice::Microphone, true);
        m_microphoneReady = true;
        m_microphonePump.start();
        sendMicrophoneState(*client, {DeviceStatus::State::On, false, {}, {}});
        break;
    }
}

void ConsoleHostController::startCamera(Client &client, const QString &requestId)
{
    m_cameraClient = client.id;
    m_cameraReady = false;
    m_cameraInUse = false;
    client.cameraRequestId = requestId;
    m_cameraPolicy = {m_controlGeneration, ++m_nextCameraId, true, m_server->cameraLoopbackDevice()};
    client.connection->setExternalCameraState(false, false, false);
    if (!m_endpoint.setCamera(m_cameraPolicy)) {
        stopCamera(DeviceControl::Unavailable, u"cannot dispatch camera startup"_s);
        return;
    }
    // Opening RDPECAM asks the client for its formats. The worker cannot
    // create a source until it receives one of those formats.
    client.connection->setDeviceEnabled(MediaDevice::Camera, true);
    m_cameraDeadline.start();
}

void ConsoleHostController::startStandardCamera(Client &client)
{
    if (!CameraAvailability::reason(m_server->cameraLoopbackDevice()).isEmpty()) return;
    if (!client.standardCamera || !client.connection || m_cameraClient || !m_control.ownsControl(client.id)
        || !client.externalCamera || !m_inputEnabled || !m_endpoint.ready()
        || m_endpoint.target().adapter != ConsoleSeat::Adapter::PhysicalUser) return;
    startCamera(client, {});
}

void ConsoleHostController::stopCamera(const QString &code, const QString &message)
{
    m_cameraDeadline.stop();
    m_cameraReady = false;
    m_cameraInUse = false;
    const auto id = std::exchange(m_cameraClient, 0);
    if (!id) return;
    for (const auto &client : m_clients) {
        if (client->id != id) continue;
        if (client->connection) {
            client->connection->setExternalCameraState(false, false, false);
            client->connection->setDeviceEnabled(MediaDevice::Camera, false);
        }
        if ((!code.isEmpty() || !client->cameraRequestId.isEmpty()) && client->connection) {
            const auto state = code.isEmpty() || code == DeviceControl::Revoked ? DeviceStatus::State::Off : DeviceStatus::State::Error;
            sendRecord(client->connection, LayoutControl::withRequestId(
                DeviceControl::stateRecord(MediaDevice::Camera, {state, false, code, message}),
                std::exchange(client->cameraRequestId, {})));
        }
        break;
    }
    m_endpoint.setCamera({m_cameraPolicy.generation, ++m_nextCameraId, false, {}});
    m_cameraPolicy = {};
}

void ConsoleHostController::cameraResult(const ConsoleWorkerWire::CameraResult &result)
{
    if (!m_cameraClient || result.generation != m_cameraPolicy.generation
        || result.requestId != m_cameraPolicy.requestId) return;
    if (!result.error.isEmpty()) {
        stopCamera(DeviceControl::Unavailable, result.error);
        return;
    }
    if (m_cameraReady) return;
    if (!m_inputEnabled || !m_control.ownsControl(m_cameraClient)) {
        stopCamera(DeviceControl::Revoked, u"console camera authority changed"_s);
        return;
    }
    for (const auto &client : m_clients) {
        if (client->id != m_cameraClient || !client->connection) continue;
        m_cameraDeadline.stop();
        m_cameraReady = true;
        client->connection->setExternalCameraState(true, false, false);
        sendRecord(client->connection, LayoutControl::withRequestId(
            DeviceControl::stateRecord(MediaDevice::Camera, {DeviceStatus::State::On, false, {}, {}}),
            std::exchange(client->cameraRequestId, {})));
        break;
    }
}
}
