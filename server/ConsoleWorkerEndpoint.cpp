// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleWorkerEndpoint.h"

#include <QLocalServer>
#include <QLocalSocket>

#include <sys/socket.h>

#include <utility>

namespace KRdp
{
ConsoleWorkerEndpoint::ConsoleWorkerEndpoint(QObject *parent)
    : QObject(parent)
    , m_server(std::make_unique<QLocalServer>(this))
{
    connect(m_server.get(), &QLocalServer::newConnection, this, &ConsoleWorkerEndpoint::acceptConnection);
    m_authenticationDeadline.setSingleShot(true);
    m_authenticationDeadline.setInterval(AuthenticationTimeoutMs);
    connect(&m_authenticationDeadline, &QTimer::timeout, this, [this] {
        if (m_worker && !m_authenticated) fail(QStringLiteral("worker did not authenticate in time"));
    });
}

void ConsoleWorkerEndpoint::setAuthenticationTimeout(int milliseconds)
{
    m_authenticationDeadline.setInterval(milliseconds);
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
    // host. The caller places the socket in a directory only that uid (and
    // root) can traverse; the peer uid and then the token are verified here.
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

void ConsoleWorkerEndpoint::dropWorker()
{
    m_authenticationDeadline.stop();
    if (m_worker) {
        QLocalSocket *socket = m_worker;
        m_worker = nullptr;
        socket->disconnect(this);
        socket->disconnectFromServer();
        socket->deleteLater();
    }
}

void ConsoleWorkerEndpoint::close()
{
    dropWorker();
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
    m_stopRequested = false;
    m_displayPolicy.reset();
    m_encoderCaps.reset();
    m_earlyReports.clear();
    m_workerCpuNs = -1;
}

bool ConsoleWorkerEndpoint::ready() const
{
    return m_ready;
}

bool ConsoleWorkerEndpoint::authenticated() const
{
    return m_authenticated;
}

bool ConsoleWorkerEndpoint::stopPending() const
{
    return m_stopRequested && !m_authenticated;
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
    // A Stop requested before the worker authenticated used to be dropped,
    // leaving the broker draining forever (AUD-C-2). Remember it and deliver
    // it the moment authentication completes; never grant readiness after.
    m_stopRequested = true;
    send(ConsoleWorkerWire::Kind::Stop);
}

void ConsoleWorkerEndpoint::requestKeyFrame()
{
    send(ConsoleWorkerWire::Kind::RequestKeyFrame);
}

bool ConsoleWorkerEndpoint::requestTopology()
{
    if (!m_ready || !m_worker) return false;
    return m_worker->write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::TopologyQuery)) >= 0;
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

void ConsoleWorkerEndpoint::setControlState(const ConsoleWorkerWire::ControlState &state)
{
    if (m_ready && m_worker) {
        m_worker->write(ConsoleWorkerWire::frame(state));
    }
}

bool ConsoleWorkerEndpoint::setDisplayPolicy(bool active, bool wakeEnabled)
{
    wakeEnabled &= active;
    if (m_stopRequested) return false;
    if (m_displayPolicy && m_displayPolicy->active == active && m_displayPolicy->wakeEnabled == wakeEnabled) return true;
    const auto revision = m_displayPolicy ? m_displayPolicy->revision + 1 : quint64(1);
    if (!revision) {
        stopWorker(); // Never wrap and replay a stale grant.
        return false;
    }
    m_displayPolicy = ConsoleWorkerWire::DisplayPolicy{revision, active, wakeEnabled};
    if (!m_authenticated || !m_worker) return true; // Desired state only, no pre-auth bytes.
    return m_worker->write(ConsoleWorkerWire::frame(*m_displayPolicy)) >= 0;
}

bool ConsoleWorkerEndpoint::resize(const ConsoleWorkerWire::Resize &request)
{
    if (!m_ready || !m_worker) {
        return false;
    }
    return m_worker->write(ConsoleWorkerWire::frame(request)) >= 0;
}

bool ConsoleWorkerEndpoint::position(const ConsoleWorkerWire::Position &request)
{
    if (!m_ready || !m_worker) return false;
    return m_worker->write(ConsoleWorkerWire::frame(request)) >= 0;
}

bool ConsoleWorkerEndpoint::positionBatch(const ConsoleWorkerWire::PositionBatch &request)
{
    if (!m_ready || !m_worker) return false;
    return m_worker->write(ConsoleWorkerWire::frame(request)) >= 0;
}

bool ConsoleWorkerEndpoint::managedFit(const ConsoleWorkerWire::ManagedFit &request)
{
    if (!m_ready || !m_worker) return false;
    return m_worker->write(ConsoleWorkerWire::frame(request)) >= 0;
}

bool ConsoleWorkerEndpoint::primary(const ConsoleWorkerWire::Primary &request)
{
    if (!m_ready || !m_worker) return false;
    return m_worker->write(ConsoleWorkerWire::frame(request)) >= 0;
}

bool ConsoleWorkerEndpoint::mixed(const ConsoleWorkerWire::Mixed &request)
{
    if (!m_ready || !m_worker) return false;
    return m_worker->write(ConsoleWorkerWire::frame(request)) >= 0;
}

bool ConsoleWorkerEndpoint::mixedCreate(const ConsoleWorkerWire::MixedCreate &request)
{
    if (!m_ready || !m_worker) return false;
    return m_worker->write(ConsoleWorkerWire::frame(request)) >= 0;
}

bool ConsoleWorkerEndpoint::physicalLayout(const ConsoleWorkerWire::PhysicalLayout &request)
{
    if (!m_ready || !m_worker || !request.allowPhysicalChange) return false;
    return m_worker->write(ConsoleWorkerWire::frame(request)) >= 0;
}

bool ConsoleWorkerEndpoint::addVirtual(const ConsoleWorkerWire::AddVirtual &request)
{
    if (!m_ready || !m_worker) return false;
    return m_worker->write(ConsoleWorkerWire::frame(request)) >= 0;
}

bool ConsoleWorkerEndpoint::removeVirtual(const ConsoleWorkerWire::RemoveVirtual &request)
{
    if (!m_ready || !m_worker) return false;
    return m_worker->write(ConsoleWorkerWire::frame(request)) >= 0;
}

bool ConsoleWorkerEndpoint::setVideoQuality(const ConsoleWorkerWire::VideoQuality &quality)
{
    if (!m_ready || !m_worker || !quality.generation || quality.quality > 100) {
        return false;
    }
    return m_worker->write(ConsoleWorkerWire::frame(quality)) >= 0;
}

bool ConsoleWorkerEndpoint::setEncoderConfig(const ConsoleWorkerWire::EncoderConfig &config)
{
    if (!m_ready || !m_worker || !config.generation) {
        return false;
    }
    return m_worker->write(ConsoleWorkerWire::frame(config)) >= 0;
}

bool ConsoleWorkerEndpoint::setMicrophone(const ConsoleWorkerWire::MicrophonePolicy &policy)
{
    if (!m_ready || !m_worker || !policy.generation || !policy.requestId) return false;
    return m_worker->write(ConsoleWorkerWire::frame(policy)) >= 0;
}

bool ConsoleWorkerEndpoint::sendMicrophoneAudio(const ConsoleWorkerWire::MicrophoneAudio &audio)
{
    // Never let microphone PCM add an unbounded backlog to the worker socket.
    // The broker must discard a failed packet, not retry stale speech later.
    if (!m_ready || !m_worker || m_worker->bytesToWrite() > 7680 || !audio.generation || !audio.requestId
        || audio.pcm.isEmpty() || audio.pcm.size() > 3840 || audio.pcm.size() % 4) return false;
    return m_worker->write(ConsoleWorkerWire::frame(audio)) >= 0;
}

bool ConsoleWorkerEndpoint::setCamera(const ConsoleWorkerWire::CameraPolicy &policy)
{
    if (!m_ready || !m_worker || !policy.generation || !policy.requestId) return false;
    return m_worker->write(ConsoleWorkerWire::frame(policy)) >= 0;
}

bool ConsoleWorkerEndpoint::setCameraFormat(const ConsoleWorkerWire::CameraFormat &format)
{
    if (!m_ready || !m_worker || !format.generation || !format.requestId
        || !format.width || format.width > 4096 || !format.height || format.height > 4096
        || !format.fps || format.fps > 120) return false;
    return m_worker->write(ConsoleWorkerWire::frame(format)) >= 0;
}

bool ConsoleWorkerEndpoint::sendCameraFrame(const ConsoleWorkerWire::CameraFrame &sample)
{
    // A slow desktop must not accumulate stale webcam frames or unbounded memory.
    if (!m_ready || !m_worker || m_worker->bytesToWrite() > 2 * ConsoleWorkerWire::MaxCameraJpegBytes
        || !sample.generation || !sample.requestId || sample.jpeg.isEmpty()
        || sample.jpeg.size() > ConsoleWorkerWire::MaxCameraJpegBytes) return false;
    return m_worker->write(ConsoleWorkerWire::frame(sample)) >= 0;
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
    // Only the selected session's uid may even attempt the token handshake.
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (getsockopt(int(candidate->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0
        || length != sizeof(credentials) || credentials.uid != m_target.uid) {
        candidate->disconnectFromServer();
        candidate->deleteLater();
        Q_EMIT protocolError(QStringLiteral("worker peer credentials do not match the selected session"));
        return;
    }
    m_worker = candidate;
    connect(candidate, &QLocalSocket::readyRead, this, &ConsoleWorkerEndpoint::readWorker);
    connect(candidate, &QLocalSocket::disconnected, this, &ConsoleWorkerEndpoint::workerDisconnected);
    m_authenticationDeadline.start();
}

void ConsoleWorkerEndpoint::readWorker()
{
    while (m_worker) {
        QByteArray chunk;
        if (!m_authenticated) {
            // Never buffer an unauthenticated peer's bytes without bound; the
            // Hello record is tiny. Read at most up to the cap so coalesced
            // post-Hello frames from a legitimate worker are still accepted.
            const qsizetype room = MaxPreAuthenticationBytes - m_deframer.pending();
            if (room <= 0) {
                fail(QStringLiteral("worker sent too much data before authenticating"));
                return;
            }
            chunk = m_worker->read(room);
        } else {
            chunk = m_worker->readAll();
        }
        if (chunk.isEmpty()) {
            return;
        }
        m_deframer.feed(chunk);
        if (!processRecords()) {
            return;
        }
    }
}

bool ConsoleWorkerEndpoint::processRecords()
{
    while (const auto record = m_deframer.next()) {
        if (!m_authenticated) {
            const auto greeting = ConsoleWorkerWire::hello(*record);
            if (!greeting || greeting->sessionId != m_target.sessionId || greeting->uid != m_target.uid || greeting->token != m_token) {
                fail(QStringLiteral("worker authentication failed"));
                return false;
            }
            m_authenticated = true;
            m_authenticationDeadline.stop();
            if (m_stopRequested) {
                send(ConsoleWorkerWire::Kind::Stop);
            } else {
                if (m_displayPolicy) m_worker->write(ConsoleWorkerWire::frame(*m_displayPolicy));
                Q_EMIT workerAuthenticated(m_target);
            }
            if (!m_worker) return false;
            continue;
        }
        // AUD-FIX7: the worker's encoder probe comes right after Hello, before Ready.
        if (const auto caps = ConsoleWorkerWire::encoderCaps(*record)) {
            m_encoderCaps = *caps;
            Q_EMIT encoderCapsReceived(*caps);
            continue;
        }
        if (!m_ready) {
            // AUD-FIX8: the worker's encoder opens before capture is confirmed. Its reports may
            // legitimately precede Ready (a 23b328a worker sends them then): hold them and apply
            // them right after Ready. Load samples are only useful while controlled.
            if (const auto report = ConsoleWorkerWire::encoderReport(*record)) {
                if (m_earlyReports.size() >= MaxEarlyReports) {
                    fail(QStringLiteral("worker sent too many encoder reports before confirming capture"));
                    return false;
                }
                m_earlyReports.append(*report);
                continue;
            }
            if (const auto load = ConsoleWorkerWire::encoderLoad(*record)) {
                m_workerCpuNs = load->cpuNs;
                continue;
            }
            if (record->kind == ConsoleWorkerWire::Kind::Error) {
                const QString reason = QString::fromUtf8(record->payload.left(256)).trimmed();
                fail(reason.isEmpty() ? QStringLiteral("worker failed before confirming capture")
                                      : QStringLiteral("worker failed before confirming capture: %1").arg(reason));
                return false;
            }
            if (record->kind != ConsoleWorkerWire::Kind::Ready || !record->payload.isEmpty()) {
                fail(QStringLiteral("worker did not confirm active capture (record %1 before Ready)").arg(int(record->kind)));
                return false;
            }
            if (m_stopRequested) {
                m_earlyReports.clear();
                continue; // A worker being drained never becomes the input endpoint.
            }
            m_ready = true;
            Q_EMIT workerReady(m_target);
            // Listeners may have closed or replaced the worker; apply the held reports only to
            // the worker that confirmed capture.
            const auto early = std::exchange(m_earlyReports, {});
            for (const auto &report : early) {
                if (!m_worker || !m_ready) break;
                Q_EMIT encoderReported(report);
            }
            if (!m_worker) return false;
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
        } else if (const auto topology = ConsoleWorkerWire::topology(*record)) {
            Q_EMIT topologyReceived(*topology);
        } else if (const auto state = ConsoleWorkerWire::controlState(*record, ConsoleWorkerWire::Kind::LocalTakeover); state && state->active) {
            Q_EMIT localTakeover(state->generation);
        } else if (const auto result = ConsoleWorkerWire::resizeResult(*record)) {
            Q_EMIT resizeFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::positionResult(*record)) {
            Q_EMIT positionFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::positionBatchResult(*record)) {
            Q_EMIT positionBatchFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::managedFitResult(*record)) {
            Q_EMIT managedFitFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::primaryResult(*record)) {
            Q_EMIT primaryFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::mixedResult(*record)) {
            Q_EMIT mixedFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::mixedCreateResult(*record)) {
            Q_EMIT mixedCreateFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::physicalLayoutResult(*record)) {
            Q_EMIT physicalLayoutFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::physicalLeaseReleased(*record)) {
            Q_EMIT physicalLeaseReleased(*result);
        } else if (const auto result = ConsoleWorkerWire::addVirtualResult(*record)) {
            Q_EMIT addVirtualFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::removeVirtualResult(*record)) {
            Q_EMIT removeVirtualFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::microphoneResult(*record)) {
            Q_EMIT microphoneFinished(*result);
        } else if (const auto result = ConsoleWorkerWire::cameraResult(*record)) {
            Q_EMIT cameraFinished(*result);
        } else if (const auto demand = ConsoleWorkerWire::cameraDemand(*record)) {
            Q_EMIT cameraDemand(*demand);
        } else if (const auto report = ConsoleWorkerWire::encoderReport(*record)) {
            Q_EMIT encoderReported(*report);
        } else if (const auto load = ConsoleWorkerWire::encoderLoad(*record)) {
            m_workerCpuNs = load->cpuNs;
        } else if (const auto stats = ConsoleWorkerWire::encoderStats(*record)) {
            Q_EMIT encoderStatsReceived(*stats);
        } else if (const auto timing = ConsoleWorkerWire::chromaTiming(*record)) {
            Q_EMIT chromaTimingReceived(*timing);
        } else if (const auto cursor = ConsoleWorkerWire::cursorShape(*record)) {
            m_cursorShape = *cursor;
            Q_EMIT cursorShapeReceived(*cursor);
        } else {
            fail(QStringLiteral("unexpected worker record"));
            return false;
        }
    }
    if (m_deframer.overflowed() || m_deframer.takeInvalidCount() != 0) {
        if (const auto version = m_deframer.mismatchedVersion()) {
            Q_EMIT versionMismatch(*version);
            fail(QStringLiteral("worker speaks wire version %1, broker speaks %2").arg(*version).arg(ConsoleWorkerWire::ProtocolVersion));
            return false;
        }
        fail(QStringLiteral("malformed worker record"));
        return false;
    }
    return m_worker != nullptr;
}

void ConsoleWorkerEndpoint::workerDisconnected()
{
    const bool wasReady = m_ready;
    m_authenticationDeadline.stop();
    if (m_worker) {
        m_worker->disconnect(this);
        m_worker->deleteLater();
    }
    m_worker = nullptr;
    m_authenticated = false;
    m_ready = false;
    m_deframer = {};
    m_encoderCaps.reset();
    m_earlyReports.clear();
    m_workerCpuNs = -1;
    m_cursorShape.reset();
    m_displayPolicy.reset();
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
