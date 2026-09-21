// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleHostController.h"

#include <algorithm>

#include <QDir>
#include <QRandomGenerator>

#include <RdpConnection.h>
#include <Server.h>
#include <VideoStream.h>
#include <VideoCodecSupport.h>
#include <InputHandler.h>

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
    client->connection = connection;
    client->session = std::make_unique<ConsoleWorkerSession>([this](const ConsoleWorkerWire::Input &input) {
        if (m_inputEnabled) {
            m_endpoint.sendInput(input);
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
    client->connections.append(connect(connection, &RdpConnection::controlRecordReceived, this, [this, connection](const QJsonObject &record) {
        if (record.value(QLatin1String("type")).toString() != QLatin1String("media")) {
            return;
        }
        const QJsonValue playback = record.value(QLatin1String("playback"));
        const QJsonValue microphone = record.value(QLatin1String("microphone"));
        const QJsonValue camera = record.value(QLatin1String("camera"));
        const QJsonValue silenceHost = record.value(QLatin1String("silenceHost"));
        if (!playback.isBool() || !microphone.isBool() || !camera.isBool() || (!silenceHost.isUndefined() && !silenceHost.isBool())) {
            connection->sendControlRecord(QJsonObject{{u"type"_s, u"error"_s}, {u"code"_s, u"invalid"_s}, {u"message"_s, u"media fields must be booleans"_s}});
            return;
        }
        if (microphone.toBool() || camera.toBool()) {
            connection->sendControlRecord(QJsonObject{{u"type"_s, u"error"_s}, {u"code"_s, u"unsupported"_s}, {u"message"_s, u"physical console microphone and camera are not available yet"_s}});
            return;
        }
        m_media = {playback.toBool(), silenceHost.toBool(false) && playback.toBool()};
        m_mediaConfigured = true;
        connection->setExternalAudioPlayback(m_media.playback);
        connection->setMediaPolicy(m_media.playback, false, false, false);
        m_endpoint.setMedia(m_media);
        connection->sendControlRecord(QJsonObject{{u"type"_s, u"media"_s}, {u"v"_s, 1}, {u"ok"_s, true}, {u"playback"_s, m_media.playback}, {u"microphone"_s, false}, {u"camera"_s, false}, {u"silenceHost"_s, m_media.silenceHost}});
    }, Qt::QueuedConnection));
    client->connections.append(connect(connection, &RdpConnection::stateChanged, this, [this, connection](RdpConnection::State state) {
        if (state == RdpConnection::State::Closed) {
            removeClient(connection);
        }
    }));
    m_clients.push_back(std::move(client));
}

void ConsoleHostController::removeClient(RdpConnection *connection)
{
    std::erase_if(m_clients, [connection](const auto &client) { return client->connection == connection; });
}
}
