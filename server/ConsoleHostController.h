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
#include <QPointer>
#include <QTimer>

#include <DeviceControl.h>
#include <RdpConnection.h>

#include "ConsoleHandoff.h"
#include "ConsoleWorkerBackoff.h"
#include "ConsoleControl.h"
#include "ConsoleInputState.h"
#include "ConsoleWorkerEndpoint.h"
#include "ConsoleTopologyPlan.h"
#include "RemoteTopologyCatalog.h"
#include "CodecRequest.h"
#include "VideoCodecHost.h"
#include "WorkerCodecBridge.h"
#include "BrokerUserSettings.h"

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
    /** The two privileged process operations the system service supplies. */
    struct WorkerLauncher {
        /** Start the worker for `target`; `socketName` identifies this launch until workerExited(). */
        std::function<bool(const ConsoleHandoff::Target &, const QString &socketName, const QByteArray &token, QString *error)> launch;
        /** Deliver `signal` (SIGTERM/SIGKILL) to the still-running launch `socketName`. */
        std::function<void(const QString &socketName, int signal)> signal;
    };
    /** PAM uid of an authenticated connection; injectable for tests. */
    using UidResolver = std::function<std::optional<quint32>(RdpConnection *)>;
    /** Disconnect with a standard RDP error-info code (MS-RDPBCGR 2.2.5.1.1); injectable for tests. */
    using Refuse = std::function<void(RdpConnection *, quint32 errorInfo)>;

    /// Stop handshake budget, then SIGTERM, then SIGKILL (AUD-C-2).
    static constexpr int DrainDeadlineMs = 5000;
    static constexpr int KillDeadlineMs = 2000;

    explicit ConsoleHostController(Server *server, WorkerLauncher launchWorker, QString runtimeDirectory, QObject *parent = nullptr);
    ~ConsoleHostController() override;
    void start();
    void setAudioPriorityDefault(bool enabled);
    void setUserSettingsReader(BrokerUserSettings::Reader reader) { m_userSettingsReader = std::move(reader); }
    void setVideoQualityPolicy(quint8 cap, bool adaptive) { m_qualityCap = cap; m_adaptiveQuality = adaptive; }
    /**
     * AUD-FIX7: offer the codec policy (`capabilities.video`, `codec`) with \a host's encoders and
     * SoftwareEncoding (unset: AVC only). The one worker encodes for every client, so a private
     * codec runs only while the controlling client is the only one admitted; otherwise AVC.
     */
    void setVideoCodecHost(const VideoCodecHost &host) { m_videoHost = host; }
    /** Latest logind sessions (ConsoleSeatWatcher::sessionsChanged). */
    void setSeatSessions(const QList<ConsoleSeat::Session> &sessions);
    /** A launched worker process exited (reaped). The only event that ends a drain. */
    void workerExited(const QString &socketName);
    void setUidResolver(UidResolver resolver);
    void setRefuse(Refuse refuse);

private:
    friend class ConsoleHostControllerTest;
    friend class ConsoleHostLifecycleTest;
    struct Client {
        ConsoleControl::Id id = 0;
        // Server deletes the connection inside its own Closed emission, before
        // our Closed slot can run; never dereference it without this guard.
        QPointer<RdpConnection> connection;
        std::optional<quint32> uid;
        std::unique_ptr<ConsoleWorkerSession> session;
        // AUD-FIX7: this client's codec policy -> the worker, while it holds control.
        std::unique_ptr<WorkerCodecBridge> codec;
        std::optional<CodecRequest::Request> codecRequest;
        bool codecApplied = false; // its private-codec policy is live (it is alone and in control)
        QList<QMetaObject::Connection> connections;
        // `device` records that arrived before admission (bounded: the latest per device).
        QList<QJsonObject> pendingDevices;
        bool wantsLayout = false;
        quint8 videoQuality = 80;
        BrokerUserSettings::Preferences preferences;
        bool preferencesLoaded = false;
        ConsoleControl::Media media;
        bool externalMicrophone = false;
        bool externalCamera = false;
        QVector<VideoMonitor> wireLayout; // RDPGFX surfaces installed for this client, not the catalog's sorted order.
        // KRDPCTL v2: the requests the next `layout` / microphone `device` record answers.
        QString layoutRequestId;
        QString microphoneRequestId;
        QString cameraRequestId;
        bool capabilitiesSent = false;
        // StandardClientMedia (DEVICES-DESIGN.md §1): either flag set means this
        // client speaks KRDPCTL and asks for each device itself.
        bool deviceRecordSeen = false;
        bool spokeKrdpctl = false;
        // Its standard AUDIN negotiation is its microphone consent (until it refuses).
        bool standardMicrophone = false;
        bool standardCamera = false;
    };

    void apply(const ConsoleHandoff::Actions &actions);
    void startWorker(const ConsoleHandoff::Target &target);
    void stopCurrentWorker();
    void workerFailed();
    void retryWorker();
    void removeWorkerDirectory();
    void clearPhysicalLease(const char *why);
    void enforceAdmission();
    bool admissible(const Client &client) const;
    void evictClient(ConsoleControl::Id id, const QString &message);
    void setWorkerActive(bool active);
    void removeClient(RdpConnection *connection, ConsoleControl::Id id = 0);
    void addClient(RdpConnection *connection);
    void loadUserSettings(Client &client);
    void updateClientDisplayPolicy(Client &client);
    void armConfiguredConsoleOutputs();
    void updateMedia();
    void onControlRecord(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &incoming);
    /** KRDPCTL v2: a reply echoing the request being handled, or the `id` it answers. */
    void replyTo(RdpConnection *connection, const QJsonObject &record);
    void sendCapabilities(Client &client);
    void sendLayouts();
    void finishTopologyQueries(const QString &error = {});
    void releaseInput();
    void syncControlState();
    void syncDisplayPolicy();
    ConsoleWorkerWire::DisplayPolicy displayPolicy() const;
    void finishResize(const QString &error);
    void finishPhysicalTopology(const QString &code, const QString &detail = {});
    void finishVirtualTopology(const QString &code, const QString &detail = {});
    QJsonObject consoleTopology(const QString &id, ConsoleControl::Id requester) const;
    bool configuredOutputTopology() const;
    bool previewOwnedTopology(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &record);
    /**
     * End the console microphone. With a \a code (revoked: state `off`;
     * anything else: `error`) its client is told; a start still pending is
     * answered either way (its requestId is echoed once).
     */
    void stopMicrophone(const QString &code = {}, const QString &message = {});
    void stopCamera(const QString &code = {}, const QString &message = {});
    void cameraResult(const ConsoleWorkerWire::CameraResult &result);
    void startCamera(Client &client, const QString &requestId);
    void startStandardCamera(Client &client);
    void microphoneResult(const ConsoleWorkerWire::MicrophoneResult &result);
    /** A microphone `device` state to \a client, answering its pending request if any. */
    void sendMicrophoneState(Client &client, const DeviceStatus &status);
    DeviceStatus deviceStatus(const Client &client, MediaDevice device) const;
    void onControlDevice(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &record, const QJsonObject &incoming);
    /** Ask the worker for the console microphone for \a client; \a requestId is answered by its acknowledgement. */
    void dispatchMicrophone(Client &client, const QString &requestId);
    /**
     * StandardClientMedia for the admitted controlling client \a id if it never
     * spoke KRDPCTL (no channel, or nothing known within StandardGateMs of its
     * admission): playback if it joined RDPSND, the microphone if it has
     * DRDYNVC (its AUDIN accept is the consent). Viewers get nothing; no camera.
     */
    void applyStandardMedia(ConsoleControl::Id id);
    /** The standard microphone of the controlling client, once the logged-in desktop is ready. */
    void startStandardMicrophone(Client &client);
    /// KRDPCTL's first-record gate (as krdpserver's): a channel client that said nothing known by then is a stock client.
    int m_standardGateMs = 3000;
    /** Test seam: RdpConnection::standardMediaChannels() (a detached test connection joined nothing). */
    std::function<std::optional<RdpConnection::StandardMediaChannels>(RdpConnection *)> m_standardChannels;
    QString m_replyRequestId; // the request onControlRecord() is handling
    /** Every record to a client goes out here (replyTo(), device states). */
    void sendRecord(RdpConnection *connection, const QJsonObject &record);
    /** Test seam: sees each record sendRecord() sends (a detached test connection drops them). */
    std::function<void(RdpConnection *, const QJsonObject &)> m_recordSent;

    Server *m_server = nullptr;
    WorkerLauncher m_launchWorker;
    QString m_runtimeDirectory;
    ConsoleHandoff::State m_handoff;
    ConsoleWorkerEndpoint m_endpoint;
    QList<ConsoleSeat::Session> m_sessions;
    UidResolver m_uidOf;
    Refuse m_refuse;
    // One launched process at a time; a replacement waits for its reaping.
    QString m_workerSocket;
    QString m_workerDirectory;
    bool m_workerAlive = false;
    QTimer m_drainDeadline;
    QTimer m_killDeadline;
    ConsoleWorkerBackoff m_backoff;
    ConsoleHandoff::Target m_failedTarget;
    QTimer m_retryTimer;
    bool m_deferredStart = false;
    std::vector<std::unique_ptr<Client>> m_clients;
    bool m_inputEnabled = false;
    bool m_audioPriorityDefault = false;
    BrokerUserSettings::Reader m_userSettingsReader;
    std::optional<VideoCodecHost> m_videoHost;
    quint8 m_qualityCap = 80;
    bool m_adaptiveQuality = false;
    /** AUD-FIX7: bind the controller's codec bridge; a private codec only while it is alone. */
    void syncCodecPolicy();
    MonitorCapturePolicy capturePolicy() const;
    ConsoleControl m_control;
    ConsoleControl::Id m_nextClientId = 0;
    bool m_mediaConfigured = false;
    ConsoleWorkerWire::Media m_media;
    ConsoleWorkerWire::Outputs m_outputs;
    RemoteTopologyCatalog m_topologyCatalog;
    bool m_topologyAvailable = false;
    bool m_topologyComplete = false;
    bool m_layoutAwaitingReadback = false;
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
        bool ownedMutation = false;
        bool managedFit = false;
        QVector<ConsoleWorkerWire::FitRelation> fitRelations;
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
        bool ownedMutation = false;
        bool managedFit = false;
    };
    bool m_experimentalPhysicalTopology = false;
    bool m_experimentalConsoleVirtual = false; // Separate from physical edits until the client accepts mixed lease inventory.
    bool m_physicalLeaseActive = false; // Fail closed if a worker vanishes before verified release.
    bool m_consoleCreatorsActive = false;
    bool m_configuredConsoleOutputs = false;
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
    ConsoleControl::Id m_cameraClient = 0;
    ConsoleWorkerWire::CameraPolicy m_cameraPolicy;
    quint64 m_nextCameraId = 0;
    bool m_cameraReady = false;
    bool m_cameraInUse = false;
    QTimer m_cameraDeadline;
};
}
