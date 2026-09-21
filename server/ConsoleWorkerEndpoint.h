// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <memory>

#include <QObject>
#include <QPointer>

#include "ConsoleHandoff.h"
#include "ConsoleWorkerWire.h"

class QLocalServer;
class QLocalSocket;

namespace KRdp
{
/**
 * Broker-side endpoint for one greeter or desktop capture worker.
 *
 * The caller supplies an unguessable per-launch token and a socket pathname in
 * that worker's runtime directory. A connection cannot send a frame or accept
 * input until its Hello matches both the selected logind session and token.
 */
class ConsoleWorkerEndpoint : public QObject
{
    Q_OBJECT

public:
    explicit ConsoleWorkerEndpoint(QObject *parent = nullptr);
    ~ConsoleWorkerEndpoint() override;

    bool listen(const QString &socketName, const ConsoleHandoff::Target &target, const QByteArray &token, QString *error = nullptr);
    void close();
    bool ready() const;
    QString socketName() const;
    ConsoleHandoff::Target target() const;

    void stopWorker();
    void requestKeyFrame();
    void sendInput(const ConsoleWorkerWire::Input &input);
    void setMedia(const ConsoleWorkerWire::Media &media);

Q_SIGNALS:
    void workerReady(const KRdp::ConsoleHandoff::Target &target);
    void workerStopped();
    void frameReceived(const KRdp::VideoFrame &frame);
    void inputReceived(const KRdp::ConsoleWorkerWire::Input &input);
    void audioReceived(const KRdp::ConsoleWorkerWire::Audio &audio);
    void protocolError(const QString &message);

private:
    void acceptConnection();
    void readWorker();
    void workerDisconnected();
    void send(ConsoleWorkerWire::Kind kind);
    void fail(const QString &message);

    std::unique_ptr<QLocalServer> m_server;
    QPointer<QLocalSocket> m_worker;
    ConsoleWorkerWire::Deframer m_deframer;
    ConsoleHandoff::Target m_target;
    QByteArray m_token;
    bool m_authenticated = false;
    bool m_ready = false;
};
}
