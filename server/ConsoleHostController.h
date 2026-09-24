// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QTimer>

#include "ConsoleHandoff.h"
#include "ConsoleControl.h"
#include "ConsoleInputState.h"
#include "ConsoleWorkerEndpoint.h"
#include "ConsoleTopologyPlan.h"
#include "RemoteTopologyCatalog.h"

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
    void setAudioPriorityDefault(bool enabled);
    void refreshSeat();
    void workerExited(const QString &socketName);

private:
    friend class ConsoleHostControllerTest;
    struct Client {
        ConsoleControl::Id id = 0;
        RdpConnection *connection = nullptr;
        std::unique_ptr<ConsoleWorkerSession> session;
        QList<QMetaObject::Connection> connections;
        QJsonObject pendingMedia;
        bool wantsLayout = false;
        quint8 videoQuality = 80;
        ConsoleControl::Media media;
        bool externalMicrophone = false;
    };

    void apply(const ConsoleHandoff::Actions &actions);
    void startWorker(const ConsoleHandoff::Target &target);
    void setWorkerActive(bool active);
    void removeClient(RdpConnection *connection);
    void addClient(RdpConnection *connection);
    void updateMedia();
    void onControlRecord(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &record);
    void sendLayouts();
    void finishTopologyQueries(const QString &error = {});
    void releaseInput();
    void syncControlState();
    void finishResize(const QString &error);
    void finishPhysicalTopology(const QString &code, const QString &detail = {});
    void finishVirtualTopology(const QString &code, const QString &detail = {});
    QJsonObject consoleTopology(const QString &id) const;
    void stopMicrophone(const QString &error);
    void microphoneResult(const ConsoleWorkerWire::MicrophoneResult &result);
    void sendMedia(Client &client, bool microphone, const QString &error = {});

    Server *m_server = nullptr;
    WorkerLauncher m_launchWorker;
    QString m_runtimeDirectory;
    ConsoleHandoff::State m_handoff;
    ConsoleWorkerEndpoint m_endpoint;
    QTimer m_seatPoll;
    std::vector<std::unique_ptr<Client>> m_clients;
    bool m_inputEnabled = false;
    bool m_audioPriorityDefault = false;
    ConsoleControl m_control;
    ConsoleControl::Id m_nextClientId = 0;
    bool m_mediaConfigured = false;
    ConsoleWorkerWire::Media m_media;
    ConsoleWorkerWire::Outputs m_outputs;
    RemoteTopologyCatalog m_topologyCatalog;
    bool m_topologyAvailable = false;
    QMap<QString, int> m_topologyPriorities; // Exact same-worker KScreen order, never inferred from primary flags.
    QHash<ConsoleControl::Id, QString> m_pendingTopology;
    struct PhysicalPreview {
        ConsoleControl::Id owner = 0;
        quint64 controlGeneration = 0;
        QString id;
        QString token;
        ConsoleTopologyPlan::Plan plan;
        QVector<RemoteTopologyDraft::Operation> operations;
        QElapsedTimer age;
    };
    struct PendingPhysical {
        ConsoleControl::Id owner = 0;
        QString id;
        quint64 serial = 0;
        quint64 controlGeneration = 0;
        ConsoleTopologyPlan::Plan plan;
        bool waitingReadback = false;
        bool resizeReply = false; // Legacy Console resize is an adapter into the same physical lease.
    };
    struct VirtualPreview {
        ConsoleControl::Id owner = 0;
        quint64 controlGeneration = 0;
        QString id;
        QString token;
        RemoteTopologyCatalog::Snapshot before;
        QMap<QString, int> priorities;
        RemoteTopologyDraft::Preview draft;
        RemoteTopologyDraft::Operation operation;
        QString backendKey;
        QElapsedTimer age;
    };
    struct PendingVirtual {
        ConsoleControl::Id owner = 0;
        QString id;
        quint64 serial = 0;
        quint64 controlGeneration = 0;
        RemoteTopologyCatalog::Snapshot before;
        QMap<QString, int> priorities;
        RemoteTopologyDraft::Preview draft;
        QString backendKey;
        bool add = false;
        bool waitingReadback = false;
    };
    bool m_experimentalPhysicalTopology = false;
    bool m_experimentalConsoleVirtual = false; // Separate from physical edits until the client accepts mixed lease inventory.
    bool m_physicalLeaseActive = false; // Fail closed if a worker vanishes before verified release.
    bool m_consoleCreatorsActive = false;
    quint64 m_physicalLeaseGeneration = 0;
    std::optional<PhysicalPreview> m_physicalPreview;
    std::optional<PendingPhysical> m_pendingPhysical;
    std::optional<VirtualPreview> m_virtualPreview;
    std::optional<PendingVirtual> m_pendingVirtual;
    quint64 m_nextPhysicalId = 0;
    QTimer m_physicalDeadline;
    ConsoleInputState m_inputState;
    ConsoleControl::Id m_workerOwner = 0;
    quint64 m_controlGeneration = 0;
    struct PendingResize {
        ConsoleControl::Id client = 0;
        QString clientRequest;
        quint64 requestId = 0;
        quint64 generation = 0;
    };
    std::optional<PendingResize> m_pendingResize;
    quint64 m_nextResizeId = 0;
    QTimer m_resizeDeadline;
    ConsoleControl::Id m_microphoneClient = 0;
    ConsoleWorkerWire::MicrophonePolicy m_microphonePolicy;
    quint64 m_nextMicrophoneId = 0;
    bool m_microphoneReady = false;
    QTimer m_microphoneDeadline;
    QTimer m_microphonePump;
};
}
