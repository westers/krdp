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
#include "LogThrottle.h"
#include "ConsoleVirtualOutputPlan.h"
#include "OutputSnapshot.h"

#include <QSet>

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
    void physicalInputActivity();
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
    /** The monitor block of a connection (default: RdpConnection::clientDisplayInfo()); a test seam. */
    using DisplayInfoProvider = std::function<ClientDisplay::Info(RdpConnection *)>;
    void setDisplayInfoProvider(DisplayInfoProvider provider) { m_displayInfoOf = std::move(provider); }
    // How long a changed Outputs record may wait for its topology reply before the frame gate opens by itself.
    void setLayoutReadbackDeadline(int milliseconds) { m_layoutReadbackDeadline.setInterval(milliseconds); }
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
        // OPT-060: Console "Replace" is attempted at most once per connection.
        // Armed once; spent when that attempt is over (worker exit, verified release,
        // desk reclaim or the client's restore request). A spent client captures
        // normally for the rest of its connection.
        bool replaceAttempted = false;
        bool replaceSpent = false;
        bool replaceEnding = false; // the worker was told to end Replace (release + plain capture); asked once per attempt
        // The last `console-screens` state pushed (nullopt: nothing pushed yet).
        std::optional<bool> screensActiveSent;
        // Why Replace ended for this client, set by the path that ended it (first wins); pushed with the
        // `console-screens` active:false edge. Empty = not known, so the record carries no reason.
        QString screensEndReason;
        bool screensAdvertised = false; // `capabilities.console.screens` was sent to it
        // OPT-060 D0: the monitors of its `console-screens-request` (a one-monitor client has no standard block).
        std::optional<QVector<VideoMonitor>> screensRequest;
        // OPT-060 M-3: the v2 (`layout:"mapped"`) request, taken (its monitors are also in screensRequest).
        std::optional<LayoutControl::ConsoleScreensMappedRequest> mappedRequest;
        // A v2 request the broker's own plan check refused (cap, union limit...): no Replace on this connection,
        // not even the standard block's one-output-per-client-monitor layout (the client asked for something else).
        QString mappedRefusal;
        // The 2 s grace for a block-armed Replace on a KRDPCTL connection that was offered `mapped` (spec 3.3).
        bool graceStarted = false;
        bool graceExpired = false;
        QElapsedTimer graceClock;
        // OPT-060 M-4: the surfaces this client shows (`console-screens-view`); nullopt = every surface is forwarded.
        std::optional<LayoutControl::ConsoleScreensView> view;
        std::optional<QSet<int>> visibleSurfaces; // `view` resolved against the current outputs; nullopt = forward all
        QSet<int> awaitingKeyFrame;               // newly visible surfaces: nothing forwarded until their next key frame
        QHash<int, quint64> framesForwarded;      // per surface, debug counters for the M-3m/M-4m measurements
        QHash<int, quint64> framesDropped;
        // M-8: a one-shot human sentence for the next `console-screens active:false` (host screens changed).
        QString screensHostChangeMessage;
    };

    /** M-3: the broker's own run of the mapped planner over the last known host screens (no wire, no worker). */
    struct MappedPrediction {
        bool planned = false; // false: no host screens known yet, nothing can be said
        ConsoleVirtualOutputPlan::MappedReport report;
    };
    MappedPrediction predictMapped(const LayoutControl::ConsoleScreensMappedRequest &request) const;
    void handleMappedScreensRequest(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &record);
    void handleScreensView(RdpConnection *connection, ConsoleControl::Id id, const QJsonObject &record);
    LayoutControl::ConsoleScreensDetail screensDetail(const Client &client) const;
    bool mappedGraceHeld(Client &client, const ClientDisplay::Info &block);
    void expireMappedGrace(ConsoleControl::Id id);
    bool hasControlChannel(RdpConnection *connection) const;
    /** M-4: resolve \a client's view against the current outputs (and clear stale gating). */
    void resolveVisibleSurfaces(Client &client);
    bool forwardsFrameTo(Client &client, const VideoFrame &frame);
    void logViewCounters(const Client &client, const char *why) const;
    /** The host's physical screens as last captured plainly (not ours, not Virtual-*): the mapped plan's input. */
    QVector<OutputSnapshot::Output> m_hostScreens;

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
    /** The attempt of every client that armed one is over; the next worker captures normally. */
    void endReplaceAttempts(const char *why, const QString &reason = QStringLiteral("workerExit"));
    /** Replace is live for \a client: its configured outputs are up and the host's screens are off. */
    bool replaceAttemptLive(const Client &client) const;
    bool replaceActiveFor(const Client &client) const;
    /** Push `console-screens` to each KRDPCTL client whose state changed. */
    void syncScreensRecords();
    /** `console-screens-restore`: turn the host's screens back on, same path as a local reclaim. */
    bool restoreHostScreens(Client &client);
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
    /** M-3: how long a block-armed Replace waits for the client's v2 request (spec 3.3; a test seam). */
    int m_screensGraceMs = 2000;
    /** Test seam: RdpConnection::hasControlChannel() (a detached test connection opened none). */
    std::function<bool(RdpConnection *)> m_controlChannelOf;
    /** Test seam: RdpConnection::isAuthenticated(). */
    std::function<bool(RdpConnection *)> m_authenticatedOf;
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
    DisplayInfoProvider m_displayInfoOf;
    bool m_expectedWorkerExit = false; // a verified Replace release was reported: the exit that follows is not a failure
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
    /** OPT-058: whether camera/microphone can start (a ready logged-in desktop); the last state pushed. */
    bool deviceSessionAvailable() const;
    void publishDeviceAvailability();
    bool m_deviceAvailabilitySent = false;
    /** Test seam: replaces the endpoint-derived availability. */
    std::optional<bool> m_deviceSessionForTest;
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
    QTimer m_layoutReadbackDeadline;
    bool m_layoutFailedOpen = false; // the gate was opened without a confirmed topology: forward frames until the outputs change
    void openLayoutGate(const char *why);
    void closeLayoutGate(); // normal end of the wait
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
    LogThrottle m_resizeRefusalLog{std::chrono::seconds(10), 3};
    void logResizeRefusal(const QString &reason);
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
