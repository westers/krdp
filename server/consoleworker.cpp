// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <memory>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include <PlasmaScreencastV1Session.h>

#include "ConsoleWorkerWire.h"
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
        connect(&m_socket, &QLocalSocket::connected, this, [this]() {
            m_connectTimeout.stop();
            m_socket.write(ConsoleWorkerWire::frame(ConsoleWorkerWire::Hello{m_sessionId, m_uid, m_token}));
            startCapture();
        });
        connect(&m_socket, &QLocalSocket::readyRead, this, &Worker::readBroker);
        connect(&m_socket, &QLocalSocket::disconnected, qApp, []() { QCoreApplication::exit(1); });
        connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            if (m_socket.state() == QLocalSocket::UnconnectedState) {
                QCoreApplication::exit(1);
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
                m_socket.write(ConsoleWorkerWire::frame(frame));
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
    void startCapture()
    {
        m_session.setActiveStream(-1); // Physical console: capture the session's complete workspace.
        m_session.setVideoCodec(VideoCodec::Avc420);
        m_session.setStreamingEnabled(true);
    }

    void readBroker()
    {
        m_deframer.feed(m_socket.readAll());
        while (const auto record = m_deframer.next()) {
            if (record->kind == ConsoleWorkerWire::Kind::Stop && record->payload.isEmpty()) {
                m_audioTimer.stop();
                m_audio.reset();
                m_session.setStreamingEnabled(false);
                QCoreApplication::quit();
                return;
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
                if (const auto event = eventFor(*input)) {
                    m_session.sendEvent(event);
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
};
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    application.setDesktopFileName(QStringLiteral("org.kde.krdpconsoleworker"));
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption socketOption(QStringLiteral("socket"), QStringLiteral("Broker socket path."), QStringLiteral("path"));
    const QCommandLineOption sessionOption(QStringLiteral("session"), QStringLiteral("logind session id."), QStringLiteral("id"));
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
