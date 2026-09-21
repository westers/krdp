// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleWorkerEndpoint.h"

#include <QLocalServer>
#include <QLocalSocket>

namespace KRdp
{
ConsoleWorkerEndpoint::ConsoleWorkerEndpoint(QObject *parent)
    : QObject(parent)
    , m_server(std::make_unique<QLocalServer>(this))
{
    connect(m_server.get(), &QLocalServer::newConnection, this, &ConsoleWorkerEndpoint::acceptConnection);
}

ConsoleWorkerEndpoint::~ConsoleWorkerEndpoint()
{
    close();
}

bool ConsoleWorkerEndpoint::listen(const QString &socketName, const ConsoleHandoff::Target &target, const QByteArray &token, QString *error)
{
    close();
    if (error) {
        error->clear();
    }
    if (socketName.isEmpty() || !target.valid() || token.size() < 16) {
        if (error) {
            *error = QStringLiteral("invalid worker endpoint parameters");
        }
        return false;
    }
    // The selected greeter/user worker has a different uid from the system
    // host. Authentication is token based, so socket filesystem access only
    // needs to permit that worker to connect.
    m_server->setSocketOptions(QLocalServer::WorldAccessOption);
    QLocalServer::removeServer(socketName);
    if (!m_server->listen(socketName)) {
        if (error) {
            *error = m_server->errorString();
        }
        return false;
    }
    m_target = target;
    m_token = token;
    return true;
}

void ConsoleWorkerEndpoint::close()
{
    if (m_worker) {
        m_worker->disconnect(this);
        m_worker->disconnectFromServer();
        m_worker = nullptr;
    }
    if (m_server->isListening()) {
        const QString name = m_server->fullServerName();
        m_server->close();
        QLocalServer::removeServer(name);
    }
    m_deframer = {};
    m_target = {};
    m_token.clear();
    m_authenticated = false;
    m_ready = false;
}

bool ConsoleWorkerEndpoint::ready() const
{
    return m_ready;
}

QString ConsoleWorkerEndpoint::socketName() const
{
    return m_server->fullServerName();
}

ConsoleHandoff::Target ConsoleWorkerEndpoint::target() const
{
    return m_target;
}

void ConsoleWorkerEndpoint::stopWorker()
{
    send(ConsoleWorkerWire::Kind::Stop);
}

void ConsoleWorkerEndpoint::requestKeyFrame()
{
    send(ConsoleWorkerWire::Kind::RequestKeyFrame);
}

void ConsoleWorkerEndpoint::sendInput(const ConsoleWorkerWire::Input &input)
{
    if (m_ready && m_worker) {
        m_worker->write(ConsoleWorkerWire::frame(input));
    }
}

void ConsoleWorkerEndpoint::setMedia(const ConsoleWorkerWire::Media &media)
{
    if (m_ready && m_worker) {
        m_worker->write(ConsoleWorkerWire::frame(media));
    }
}

void ConsoleWorkerEndpoint::acceptConnection()
{
    QLocalSocket *candidate = m_server->nextPendingConnection();
    if (m_worker || !candidate) {
        if (candidate) {
            candidate->disconnectFromServer();
            candidate->deleteLater();
        }
        return;
    }
    m_worker = candidate;
    connect(candidate, &QLocalSocket::readyRead, this, &ConsoleWorkerEndpoint::readWorker);
    connect(candidate, &QLocalSocket::disconnected, this, &ConsoleWorkerEndpoint::workerDisconnected);
}

void ConsoleWorkerEndpoint::readWorker()
{
    if (!m_worker) {
        return;
    }
    m_deframer.feed(m_worker->readAll());
    while (const auto record = m_deframer.next()) {
        if (!m_authenticated) {
            const auto greeting = ConsoleWorkerWire::hello(*record);
            if (!greeting || greeting->sessionId != m_target.sessionId || greeting->uid != m_target.uid || greeting->token != m_token) {
                fail(QStringLiteral("worker authentication failed"));
                return;
            }
            m_authenticated = true;
            continue;
        }
        if (!m_ready) {
            if (record->kind != ConsoleWorkerWire::Kind::Ready || !record->payload.isEmpty()) {
                fail(QStringLiteral("worker did not confirm active capture"));
                return;
            }
            m_ready = true;
            Q_EMIT workerReady(m_target);
            continue;
        }
        if (const auto frame = ConsoleWorkerWire::videoFrame(*record)) {
            Q_EMIT frameReceived(*frame);
        } else if (const auto input = ConsoleWorkerWire::input(*record)) {
            Q_EMIT inputReceived(*input);
        } else if (const auto audio = ConsoleWorkerWire::audio(*record)) {
            Q_EMIT audioReceived(*audio);
        } else if (const auto outputs = ConsoleWorkerWire::outputs(*record)) {
            Q_EMIT outputsReceived(*outputs);
        } else {
            fail(QStringLiteral("unexpected worker record"));
            return;
        }
    }
    if (m_deframer.overflowed() || m_deframer.takeInvalidCount() != 0) {
        fail(QStringLiteral("malformed worker record"));
    }
}

void ConsoleWorkerEndpoint::workerDisconnected()
{
    const bool wasReady = m_ready;
    m_worker = nullptr;
    m_authenticated = false;
    m_ready = false;
    m_deframer = {};
    if (wasReady) {
        Q_EMIT workerStopped();
    }
}

void ConsoleWorkerEndpoint::send(ConsoleWorkerWire::Kind kind)
{
    // A replacement may be selected before its encoder has become active.
    // Authentication is enough to deliver Stop; readiness is only the point
    // at which frames and input may cross the endpoint.
    if (m_authenticated && m_worker) {
        m_worker->write(ConsoleWorkerWire::frame(kind));
    }
}

void ConsoleWorkerEndpoint::fail(const QString &message)
{
    Q_EMIT protocolError(message);
    if (m_worker) {
        m_worker->disconnectFromServer();
    }
}
}
