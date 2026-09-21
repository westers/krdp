// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QJsonObject>
#include <QTimer>

#include "ConsoleHandoff.h"
#include "ConsoleControl.h"
#include "ConsoleWorkerEndpoint.h"

namespace KRdp
{
class RdpConnection;
class Server;
class ConsoleWorkerSession;

/**
 * Stable, compositor-independent owner for physical-console RDP clients.
 *
 * The system service supplies `WorkerLauncher`, which is the only privileged
 * operation: launch an already-installed capture worker in the selected
 * logind session.  All video/input crossing afterwards is constrained to the
 * authenticated endpoint.  This class never opens a Wayland connection.
 */
class ConsoleHostController final : public QObject
{
    Q_OBJECT

public:
    using WorkerLauncher = std::function<bool(const ConsoleHandoff::Target &, const QString &socketName, const QByteArray &token, QString *error)>;

    explicit ConsoleHostController(Server *server, WorkerLauncher launchWorker, QString runtimeDirectory, QObject *parent = nullptr);
    ~ConsoleHostController() override;
    void start();
    void refreshSeat();
    void workerExited(const QString &socketName);

private:
    struct Client {
        ConsoleControl::Id id = 0;
        RdpConnection *connection = nullptr;
        std::unique_ptr<ConsoleWorkerSession> session;
        QList<QMetaObject::Connection> connections;
        QJsonObject pendingMedia;
        bool wantsLayout = false;
    };

    void apply(const ConsoleHandoff::Actions &actions);
    void startWorker(const ConsoleHandoff::Target &target);
    void setWorkerActive(bool active);
    void removeClient(RdpConnection *connection);
    void addClient(RdpConnection *connection);
    void updateMedia();
    void onControlRecord(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &record);
    void sendLayouts();

    Server *m_server = nullptr;
    WorkerLauncher m_launchWorker;
    QString m_runtimeDirectory;
    ConsoleHandoff::State m_handoff;
    ConsoleWorkerEndpoint m_endpoint;
    QTimer m_seatPoll;
    std::vector<std::unique_ptr<Client>> m_clients;
    bool m_inputEnabled = false;
    ConsoleControl m_control;
    ConsoleControl::Id m_nextClientId = 0;
    bool m_mediaConfigured = false;
    ConsoleWorkerWire::Media m_media;
    ConsoleWorkerWire::Outputs m_outputs;
};
}
