// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ConsoleWorkerEndpoint.h"

#include <limits>
#include "PointerCaptureProtocol.h"

#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>

#include <sys/socket.h>

#include <chrono>
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
    m_watchdogTimer.setInterval(ConsoleWorkerWire::HeartbeatIntervalMs);
    connect(&m_watchdogTimer, &QTimer::timeout, this, &ConsoleWorkerEndpoint::watchdogTick);
}

qint64 ConsoleWorkerEndpoint::watchdogNow() const
{
    if (m_watchdogClock) return m_watchdogClock();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ConsoleWorkerEndpoint::watchdogTick()
{
    if (!m_worker || !m_authenticated) return;
    // Unread bytes mean the worker spoke and this process has not got to them yet (the broker is the slow one).
    if (m_watchdog.tick(watchdogNow(), m_worker->bytesAvailable() > 0) != ConsoleWorkerWatchdog::Verdict::Hung) return;
    const qint64 silent = m_watchdog.silentMs();
    qInfo().noquote() << "Console worker sent nothing for" << silent << "ms (" << (m_ready ? "after" : "before") << "Ready); treating it as hung";
    m_watchdogTimer.stop();
    Q_EMIT workerUnresponsive(silent);
    if (m_worker) m_worker->disconnectFromServer();
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
    m_pointerState = {}; m_pointerControl = {};
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
    m_watchdog.disarm();
    m_watchdogTimer.stop();
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
    ++m_keyFrameRequests;
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

void ConsoleWorkerEndpoint::reclaimConsole(quint64 generation)
{
    ++m_reclaimRequests;
    if (m_ready && m_worker)
        m_worker->write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ControlState{generation, true}, ConsoleWorkerWire::Kind::ReclaimConsole));
}

void ConsoleWorkerEndpoint::setControlState(const ConsoleWorkerWire::ControlState &state)
{
    if (state != m_pointerControl) m_pointerState = {};
    m_pointerControl = state;
    if (m_ready && m_worker) {
        m_worker->write(ConsoleWorkerWire::frame(state));
    }
}

bool ConsoleWorkerEndpoint::setPointerCapture(const QJsonObject &request)
{
    if (!m_ready || !m_worker || !m_pointerControl.active || !PointerCaptureProtocol::request(request)
        || request.value(QStringLiteral("generation")).toString() != QString::number(m_pointerControl.generation)
        || request.value(QStringLiteral("epoch")) != m_pointerState.value(QStringLiteral("epoch"))
        || !m_pointerState.value(QStringLiteral("supported")).toBool()) return false;
    return m_worker->write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::PointerCapture, QJsonDocument(request).toJson(QJsonDocument::Compact))) >= 0;
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

bool ConsoleWorkerEndpoint::setClipboardPolicy(quint64 controlGeneration, bool hostToClient, bool clientToHost, bool locked, bool greeter)
{
    if (m_stopRequested) return false;
    ConsoleWorkerWire::ClipboardPolicy policy{0, controlGeneration, hostToClient && !greeter, clientToHost, locked, greeter};
    if (m_clipboardPolicy) {
        policy.revision = m_clipboardPolicy->revision;
        if (policy == *m_clipboardPolicy) return true;
    }
    if (m_clipboardRevision == std::numeric_limits<quint64>::max()) {
        stopWorker(); // Never wrap and replay a stale grant.
        return false;
    }
    policy.revision = ++m_clipboardRevision;
    m_clipboardPolicy = policy;
    if (!m_ready || !m_worker) return true; // Sent when the worker is Ready.
    m_clipboardPolicySent = policy.revision;
    return m_worker->write(ConsoleWorkerWire::frame(policy)) >= 0;
}

quint64 ConsoleWorkerEndpoint::requestClipboard(quint32 maxBytes)
{
    // The text is fetched when the client pastes; a backlogged socket refuses instead of queueing stale pastes.
    if (!m_ready || !m_worker || !m_clipboardPolicy || !m_clipboardPolicy->hostToClient || !maxBytes
        || maxBytes > ConsoleWorkerWire::MaxClipboardBytes || m_worker->bytesToWrite() > 2 * qint64(ConsoleWorkerWire::MaxClipboardBytes)
        || m_clipboardRequests.size() >= 8 || m_nextClipboardId == std::numeric_limits<quint64>::max()) return 0;
    const quint64 id = ++m_nextClipboardId;
    if (m_worker->write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardRequest{id, maxBytes})) < 0) return 0;
    m_clipboardRequests.insert(id);
    return id;
}

quint64 ConsoleWorkerEndpoint::setClipboard(quint64 controlGeneration, const QString &text)
{
    if (!m_ready || !m_worker || !m_clipboardPolicy || !m_clipboardPolicy->clientToHost || !controlGeneration
        || controlGeneration != m_clipboardPolicy->controlGeneration
        || quint64(text.toUtf8().size()) > ConsoleWorkerWire::MaxClipboardBytes
        || m_worker->bytesToWrite() > 2 * qint64(ConsoleWorkerWire::MaxClipboardBytes)
        || m_clipboardSets.size() >= 8 || m_nextClipboardId == std::numeric_limits<quint64>::max()) return 0;
    const quint64 id = ++m_nextClipboardId;
    if (m_worker->write(ConsoleWorkerWire::frame(ConsoleWorkerWire::ClipboardSet{id, controlGeneration, text})) < 0) return 0;
    m_clipboardSets.insert(id);
    return id;
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
            if (m_watchdogEnabled) {
                m_watchdog.arm(watchdogNow());
                m_watchdogTimer.start();
            }
            if (m_stopRequested) {
                send(ConsoleWorkerWire::Kind::Stop);
            } else {
                if (m_displayPolicy) m_worker->write(ConsoleWorkerWire::frame(*m_displayPolicy));
                Q_EMIT workerAuthenticated(m_target);
            }
            if (!m_worker) return false;
            continue;
        }
        m_watchdog.alive(watchdogNow()); // any well-formed record from the authenticated worker is proof of life
        if (record->kind == ConsoleWorkerWire::Kind::Heartbeat) {
            if (!ConsoleWorkerWire::heartbeat(*record)) {
                fail(QStringLiteral("malformed worker heartbeat"));
                return false;
            }
            continue; // valid before Ready as well: a worker stuck in startup is still hung
        }
        // AUD-FIX7: the worker's encoder probe comes right after Hello, before Ready.
        if (const auto caps = ConsoleWorkerWire::encoderCaps(*record)) {
            m_encoderCaps = *caps;
            Q_EMIT encoderCapsReceived(*caps);
            continue;
        }
        if (record->kind == ConsoleWorkerWire::Kind::Error) {
            const QString reason = QString::fromUtf8(record->payload.left(256)).trimmed();
            const QString context = m_ready ? QStringLiteral("worker capture failed")
                                           : QStringLiteral("worker failed before confirming capture");
            fail(reason.isEmpty() ? context : QStringLiteral("%1: %2").arg(context, reason));
            return false;
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
            if (m_worker && m_clipboardPolicy && m_clipboardPolicySent != m_clipboardPolicy->revision) {
                m_clipboardPolicySent = m_clipboardPolicy->revision;
                m_worker->write(ConsoleWorkerWire::frame(*m_clipboardPolicy));
            }
            if (!m_worker) return false;
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
        if (record->kind == ConsoleWorkerWire::Kind::PointerState) {
            const auto state = PointerCaptureProtocol::decode(record->payload);
            if (!PointerCaptureProtocol::state(state)) { fail(QStringLiteral("invalid worker pointer state")); return false; }
            if (state.value(QStringLiteral("generation")).toString() == QString::number(m_pointerControl.generation)) {
                m_pointerState = state; Q_EMIT pointerStateReceived(state);
            }
        } else if (const auto frame = ConsoleWorkerWire::videoFrame(*record)) {
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
        } else if (const auto state = ConsoleWorkerWire::controlState(*record, ConsoleWorkerWire::Kind::LocalReclaim); state && state->active) {
            Q_EMIT localReclaim(state->generation);
        } else if (const auto state = ConsoleWorkerWire::controlState(*record, ConsoleWorkerWire::Kind::HostScreensChanged); state && state->active) {
            Q_EMIT hostScreensChanged(state->generation);
        } else if (const auto offer = ConsoleWorkerWire::clipboardOffer(*record)) {
            // Newest only; one the policy no longer allows is dropped here, never forwarded.
            if (offer->seq > m_clipboardOfferSeq) {
                m_clipboardOfferSeq = offer->seq;
                if (m_clipboardPolicy && m_clipboardPolicy->hostToClient) Q_EMIT clipboardOffered(offer->seq);
            }
        } else if (const auto data = ConsoleWorkerWire::clipboardData(*record)) {
            if (m_clipboardRequests.remove(data->requestId)) Q_EMIT clipboardDataReceived(*data);
        } else if (const auto result = ConsoleWorkerWire::clipboardSetResult(*record)) {
            if (m_clipboardSets.remove(result->setId)) Q_EMIT clipboardSetFinished(*result);
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
            qWarning() << "Rejected worker record kind" << static_cast<quint8>(record->kind)
                       << "payload bytes" << record->payload.size();
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
    m_pointerState = {}; m_pointerControl = {};
    m_watchdog.disarm();
    m_watchdogTimer.stop();
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
    // OPT-067: a new worker starts from nothing; ids already issued are forgotten, so a late answer is discarded.
    m_clipboardPolicy.reset();
    m_clipboardPolicySent = 0;
    m_clipboardOfferSeq = 0;
    m_clipboardRequests.clear();
    m_clipboardSets.clear();
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
