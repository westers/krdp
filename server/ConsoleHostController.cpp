// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleHostController.h"

#include <algorithm>
#include <utility>

#include <QDir>
#include <QRandomGenerator>

#include <RdpConnection.h>
#include <Server.h>
#include <VideoStream.h>
#include <VideoCodecSupport.h>
#include <InputHandler.h>
#include <LayoutControl.h>

#include "ConsoleSeat.h"
#include "ConsoleWorkerSession.h"

using namespace Qt::StringLiterals;

namespace KRdp
{
ConsoleHostController::ConsoleHostController(Server *server, WorkerLauncher launchWorker, QString runtimeDirectory, QObject *parent)
    : QObject(parent)
    , m_server(server)
    , m_launchWorker(std::move(launchWorker))
    , m_runtimeDirectory(std::move(runtimeDirectory))
{
    Q_ASSERT(m_server);
    m_seatPoll.setInterval(500);
    connect(&m_seatPoll, &QTimer::timeout, this, &ConsoleHostController::refreshSeat);
    connect(m_server, &Server::newConnectionCreated, this, &ConsoleHostController::addClient);
    connect(&m_endpoint, &ConsoleWorkerEndpoint::workerReady, this, [this](const auto &target) {
        apply(m_handoff.workerReady(target));
        m_endpoint.setControlState({m_controlGeneration, m_control.owner() != 0});
        if (m_mediaConfigured) {
            m_endpoint.setMedia(m_media);
        }
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::workerStopped, this, [this]() {
        apply(m_handoff.workerStopped());
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::frameReceived, this, [this](const VideoFrame &frame) {
        if (!m_inputEnabled) {
            return;
        }
        for (const auto &client : m_clients) {
            client->session->submitFrame(frame);
        }
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::audioReceived, this, [this](const ConsoleWorkerWire::Audio &audio) {
        for (const auto &client : m_clients) {
            client->connection->submitExternalAudio(audio.pcm);
        }
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::outputsReceived, this, [this](const ConsoleWorkerWire::Outputs &outputs) {
        m_outputs = outputs;
        sendLayouts();
    });
    connect(&m_endpoint, &ConsoleWorkerEndpoint::localTakeover, this, [this](quint64 generation) {
        if (!m_control.owner() || generation != m_controlGeneration) {
            return; // A late cursor report must not revoke a newer controller.
        }
        qInfo() << "Local console pointer activity: releasing remote control";
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

ConsoleHostController::~ConsoleHostController() = default;

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
    if (!active) {
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
    auto client = std::make_unique<Client>();
    const auto id = client->id = ++m_nextClientId;
    client->connection = connection;
    client->session = std::make_unique<ConsoleWorkerSession>([this, id](const ConsoleWorkerWire::Input &input) {
        if (m_inputEnabled && m_control.ownsControl(id)) {
            m_endpoint.sendInput(input);
            m_inputState.record(input);
        }
    });
    client->session->setWorkerActive(m_inputEnabled);
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
    if (microphone.toBool() || camera.toBool()) {
        connection->sendControlRecord(QJsonObject{{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"request"_s, u"media"_s}, {u"code"_s, u"unsupported"_s}, {u"message"_s, u"physical console microphone and camera are not available yet"_s}});
        return;
    }
    const ConsoleControl::Media requested{playback.toBool(), silenceHost.toBool(false) && playback.toBool()};
    if (!m_control.setMedia(id, requested)) {
        connection->sendControlRecord(QJsonObject{{u"type"_s, u"error"_s}, {u"v"_s, 1}, {u"request"_s, u"media"_s}, {u"code"_s, u"not-owner"_s}, {u"message"_s, u"only the authenticated console controller may silence the host"_s}});
        return;
    }
    connection->setExternalAudioPlayback(requested.playback);
    connection->setMediaPolicy(requested.playback, false, false, false);
    updateMedia();
    connection->sendControlRecord(QJsonObject{{u"type"_s, u"media"_s}, {u"v"_s, 1}, {u"ok"_s, true}, {u"playback"_s, requested.playback}, {u"microphone"_s, false}, {u"camera"_s, false}, {u"silenceHost"_s, requested.silenceHost}});
}

void ConsoleHostController::removeClient(RdpConnection *connection)
{
    for (const auto &client : m_clients) {
        if (client->connection == connection) {
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
            client->connection->sendControlRecord(LayoutControl::layoutRecord(layout));
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
        m_workerOwner = m_control.owner();
        ++m_controlGeneration;
        m_endpoint.setControlState({m_controlGeneration, m_workerOwner != 0});
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
}
