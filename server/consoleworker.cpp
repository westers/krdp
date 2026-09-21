// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <memory>

#include <QAction>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>

#include <PlasmaScreencastV1Session.h>
#include <KGlobalAccel>

#include "ConsoleWorkerWire.h"
#include "ConsoleInputState.h"
#include "ConsoleResizeSession.h"
#include "TakeoverDetector.h"
#include "PipeWireAudioPlayback.h"

using namespace KRdp;

namespace
{
std::shared_ptr<QEvent> eventFor(const ConsoleWorkerWire::Input &input)
{
    switch (input.type) {
    case ConsoleWorkerWire::Input::Type::Mouse:
        if (input.eventType != QEvent::MouseMove && input.eventType != QEvent::MouseButtonPress && input.eventType != QEvent::MouseButtonRelease) {
            return {};
        }
        return std::make_shared<QMouseEvent>(input.eventType, input.position, QPointF{}, input.button, input.buttons, Qt::NoModifier);
    case ConsoleWorkerWire::Input::Type::Wheel:
        if (input.eventType != QEvent::Wheel) {
            return {};
        }
        return std::make_shared<QWheelEvent>(input.position, QPointF{}, QPoint{}, input.angleDelta, input.buttons, Qt::NoModifier, Qt::NoScrollPhase, false);
    case ConsoleWorkerWire::Input::Type::Key:
        if (input.eventType != QEvent::KeyPress && input.eventType != QEvent::KeyRelease) {
            return {};
        }
        return std::make_shared<QKeyEvent>(input.eventType, 0, Qt::NoModifier, input.nativeScanCode, input.nativeVirtualKey, 0, input.text);
    }
    return {};
}

class Worker : public QObject
{
public:
    Worker(const QString &socketName, const QString &sessionId, quint32 uid, const QByteArray &token, QObject *parent = nullptr)
        : QObject(parent)
        , m_socketName(socketName)
        , m_sessionId(sessionId)
        , m_uid(uid)
        , m_token(token)
    {
        m_clock.start();
        connect(&m_resize, &ConsoleResizeSession::mutationStarting, this, [this]() {
            releaseInput();
            m_takeover.outputMoved(m_clock.elapsed());
        });
        connect(&m_resize, &ConsoleResizeSession::keyframeNeeded, &m_session, &AbstractSession::requestKeyFrame);
        connect(&m_resize, &ConsoleResizeSession::result, this, [this](const auto &result) {
            if (m_socket.state() == QLocalSocket::ConnectedState) {
                m_socket.write(ConsoleWorkerWire::frame(result));
            }
        });
        connect(&m_resize, &ConsoleResizeSession::stopped, this, [this](const QString &error) {
            if (!error.isEmpty()) {
                qWarning().noquote() << "Console output restoration:" << error;
            }
            m_session.setStreamingEnabled(false);
            QCoreApplication::exit(error.isEmpty() ? m_exitCode : 1);
        });
        // Use Plasma's shortcut service, never a raw keyboard grab. SDDM need
        // not provide it; do not auto-start desktop services in the greeter.
        if (QDBusConnection::sessionBus().interface()
            && QDBusConnection::sessionBus().interface()->isServiceRegistered(QStringLiteral("org.kde.kglobalaccel"))) {
            m_reclaimAction.setText(QStringLiteral("Reclaim physical console"));
            m_reclaimAction.setObjectName(QStringLiteral("reclaim-console"));
            m_reclaimAction.setProperty("componentName", QStringLiteral("krdp-console-worker"));
            m_reclaimAction.setProperty("componentDisplayName", QStringLiteral("KRDP Physical Console"));
            m_reclaimAction.setEnabled(false);
            connect(&m_reclaimAction, &QAction::triggered, this, &Worker::reclaimConsole);
            const QKeySequence shortcut(Qt::META | Qt::CTRL | Qt::ALT | Qt::Key_T);
            KGlobalAccel::self()->setDefaultShortcut(&m_reclaimAction, {shortcut});
            KGlobalAccel::self()->setShortcut(&m_reclaimAction, {shortcut});
        }
        connect(&m_session, &AbstractSession::cursorUpdate, this, [this](const PipeWireCursor &cursor) {
            if (m_control.active && !m_resize.changing() && m_session.outputGeometryResolved()
                && m_takeover.observed(m_session.mapToGlobal(cursor.position).toPoint(), m_clock.elapsed())) {
                reclaimConsole();
            }
        });
        connect(&m_session, &AbstractSession::outputGeometryChanged, this, [this](const QRect &) {
            m_takeover.outputMoved(m_clock.elapsed());
        });
        connect(&m_socket, &QLocalSocket::connected, this, [this]() {
            m_connectTimeout.stop();
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{m_sessionId, m_uid, m_token}));
            startCapture();
        });
        connect(&m_socket, &QLocalSocket::readyRead, this, &Worker::readBroker);
        connect(&m_socket, &QLocalSocket::disconnected, this, [this]() {
            shutdown(1);
        });
        connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            if (m_socket.state() == QLocalSocket::UnconnectedState) {
                shutdown(1);
            }
        });
        connect(&m_session, &AbstractSession::streamActiveChanged, this, [this](bool active) {
            if (active && !m_captureReady) {
                m_captureReady = true;
                m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Ready));
            }
        });
        connect(&m_session, &AbstractSession::frameReceived, this, [this](const VideoFrame &frame) {
            if (m_captureReady) {
                // Use the same ordering and coordinates as the captured frame,
                // not a root-side guess about the user's monitor setup.
                const auto screens = qGuiApp->screens();
                ConsoleWorkerWire::Outputs outputs;
                QRect workspace;
                for (const auto *screen : screens) {
                    workspace = workspace.united(screen->geometry());
                }
                if (screens.size() == frame.monitors.size()) {
                    for (qsizetype i = 0; i < screens.size(); ++i) {
                        if (screens[i]->geometry().translated(-workspace.topLeft()) != frame.monitors[i].geometry) {
                            outputs.monitors.clear();
                            break; // Wait until capture and screen discovery agree after hotplug.
                        }
                        outputs.monitors.append({screens[i]->name(), frame.monitors[i].geometry,
                                                 screens[i]->devicePixelRatio(), frame.monitors[i].primary});
                    }
                }
                if (!outputs.monitors.isEmpty() && outputs != m_outputs) {
                    m_outputs = outputs;
                    m_socket.write(ConsoleWorkerWire::frame(outputs));
                }
                m_socket.write(ConsoleWorkerWire::frame(frame));
                // Only this frame's validated output geometry may complete Fit,
                // never metadata cached before a resize or a compositor handoff.
                m_resize.captured(outputs, frame.isKeyFrame);
            }
        });
        connect(&m_session, &AbstractSession::error, this, [this]() {
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Error, QByteArrayLiteral("screencast failed")));
            m_socket.disconnectFromServer();
        });
        m_connectTimeout.setSingleShot(true);
        connect(&m_connectTimeout, &QTimer::timeout, qApp, []() { QCoreApplication::exit(1); });
        m_audioTimer.setInterval(20);
        connect(&m_audioTimer, &QTimer::timeout, this, [this]() {
            if (!m_audio || !m_captureReady) {
                return;
            }
            const QByteArray pcm = m_audio->take();
            if (!pcm.isEmpty()) {
                m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Audio{pcm}));
            }
        });
    }

    void connectToBroker()
    {
        m_connectTimeout.start(10000);
        m_socket.connectToServer(m_socketName);
    }

private:
    void shutdown(int code)
    {
        if (m_stopping) {
            return;
        }
        m_stopping = true;
        m_exitCode = code;
        releaseInput();
        m_audioTimer.stop();
        m_audio.reset();
        m_resize.stop(); // Keep the event loop alive until restoration finishes.
    }

    void releaseInput()
    {
        const auto releases = m_inputState.releaseAll();
        if (!releases.isEmpty()) {
            qInfo() << "Worker releasing" << releases.size() << "held console input(s)";
        }
        for (const auto &input : releases) {
            if (const auto event = eventFor(input)) {
                m_session.sendEvent(event);
            }
        }
    }

    void startCapture()
    {
        m_session.setActiveStream(-1); // Physical console: capture the session's complete workspace.
        m_session.setVideoCodec(VideoCodec::Avc420);
        m_session.setStreamingEnabled(true);
    }

    void reclaimConsole()
    {
        if (!m_control.active) {
            return;
        }
        releaseInput();
        m_socket.write(ConsoleWorkerWire::frame(m_control, ConsoleWorkerWire::Kind::LocalTakeover));
        m_control.active = false; // Gate immediately, before the host's acknowledgement.
        m_resize.setControl(m_control);
        m_reclaimAction.setEnabled(false);
        m_takeover.latch();
    }

    void readBroker()
    {
        if (m_stopping) {
            return;
        }
        m_deframer.feed(m_socket.readAll());
        while (const auto record = m_deframer.next()) {
            if (const auto control = ConsoleWorkerWire::controlState(*record)) {
                if (*control != m_control) {
                    releaseInput();
                    m_control = *control;
                    m_resize.setControl(*control);
                    m_reclaimAction.setEnabled(control->active);
                    m_takeover = {};
                    if (control->active) {
                        m_takeover.armed(m_clock.elapsed());
                    }
                }
                continue;
            }
            if (record->kind == ConsoleWorkerWire::Kind::Stop && record->payload.isEmpty()) {
                shutdown(0);
                return;
            }
            if (const auto request = ConsoleWorkerWire::resize(*record)) {
                releaseInput();
                m_resize.request(*request);
                continue;
            }
            if (record->kind == ConsoleWorkerWire::Kind::RequestKeyFrame && record->payload.isEmpty()) {
                m_session.requestKeyFrame();
                continue;
            }
            if (const auto media = ConsoleWorkerWire::media(*record)) {
                m_audioTimer.stop();
                m_audio.reset();
                if (media->playback) {
                    m_audio = std::make_unique<PipeWireAudioPlayback>();
                    const bool started = media->silenceHost ? m_audio->startIsolated(QStringLiteral("console-%1").arg(m_sessionId))
                                                            : m_audio->start(QStringLiteral("@DEFAULT_AUDIO_SINK@"));
                    if (!started) {
                        qWarning("Cannot capture console-session PipeWire audio");
                        m_audio.reset();
                    } else {
                        m_audioTimer.start();
                    }
                }
                continue;
            }
            if (const auto input = ConsoleWorkerWire::input(*record)) {
                if (!m_control.active || !m_resize.inputAllowed()) {
                    continue;
                }
                if (const auto event = eventFor(*input)) {
                    if (input->type == ConsoleWorkerWire::Input::Type::Mouse && input->eventType == QEvent::MouseMove) {
                        m_takeover.injected(m_session.mapToGlobal(input->position).toPoint(), m_clock.elapsed());
                    }
                    m_session.sendEvent(event);
                    m_inputState.record(*input);
                    continue;
                }
            }
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Kind::Error, QByteArrayLiteral("invalid broker record")));
            m_socket.disconnectFromServer();
            return;
        }
        if (m_deframer.overflowed() || m_deframer.takeInvalidCount() != 0) {
            m_socket.disconnectFromServer();
        }
    }

    QString m_socketName;
    QString m_sessionId;
    quint32 m_uid = 0;
    QByteArray m_token;
    QLocalSocket m_socket;
    ConsoleWorkerWire::Deframer m_deframer;
    PlasmaScreencastV1Session m_session;
    std::unique_ptr<PipeWireAudioPlayback> m_audio;
    QTimer m_connectTimeout;
    QTimer m_audioTimer;
    bool m_captureReady = false;
    ConsoleWorkerWire::Outputs m_outputs;
    ConsoleInputState m_inputState;
    QElapsedTimer m_clock;
    Takeover::Detector m_takeover;
    QAction m_reclaimAction;
    ConsoleWorkerWire::ControlState m_control;
    ConsoleResizeSession m_resize;
    bool m_stopping = false;
    int m_exitCode = 0;
};
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    application.setDesktopFileName(QStringLiteral("org.kde.krdpconsoleworker"));
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption socketOption(QStringLiteral("socket"), QStringLiteral("Broker socket path."), QStringLiteral("path"));
    // QGuiApplication consumes --session for its own session restoration.
    const QCommandLineOption sessionOption(QStringLiteral("logind-session"), QStringLiteral("logind session id."), QStringLiteral("id"));
    const QCommandLineOption uidOption(QStringLiteral("uid"), QStringLiteral("logind uid."), QStringLiteral("uid"));
    const QCommandLineOption tokenOption(QStringLiteral("token-hex"), QStringLiteral("Per-launch broker token."), QStringLiteral("token"));
    const QCommandLineOption tokenFdOption(QStringLiteral("token-fd"), QStringLiteral("Read the per-launch broker token once from this inherited fd."), QStringLiteral("fd"));
    parser.addOptions({socketOption, sessionOption, uidOption, tokenOption, tokenFdOption});
    parser.process(application);

    bool uidOk = false;
    const quint32 uid = parser.value(uidOption).toUInt(&uidOk);
    QByteArray token;
    if (parser.isSet(tokenFdOption)) {
        bool fdOk = false;
        const int fd = parser.value(tokenFdOption).toInt(&fdOk);
        QFile tokenFile;
        if (!fdOk || fd < 0 || !tokenFile.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            parser.showHelp(1);
        }
        token = tokenFile.readAll();
    } else {
        token = QByteArray::fromHex(parser.value(tokenOption).toLatin1());
    }
    if (parser.isSet(tokenOption) == parser.isSet(tokenFdOption) || !uidOk || uid == 0 || parser.value(socketOption).isEmpty() || parser.value(sessionOption).isEmpty() || token.size() < 16) {
        parser.showHelp(1);
    }

    Worker worker(parser.value(socketOption), parser.value(sessionOption), uid, token);
    worker.connectToBroker();
    return application.exec();
}
