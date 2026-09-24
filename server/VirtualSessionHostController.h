// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionTransport.h"
#include "VirtualSessionBrokerLease.h"
#include "VirtualSessionJournal.h"
#include "RemoteTopologyCatalog.h"
#include <QSet>
#include <Server.h>

namespace KRdp
{
/** Owns transport/registry/namespace integration. A trusted launcher prepares
 * the private runtime and token delivery; host listens before spawning it.
 * Preparation must derive account identity from uid, never client strings.
 * No production launch policy or capability advertisement is implied here.
 */
class VirtualSessionHostController : public QObject
{
public:
    struct PreparedLaunch {
        QString socketName;
        VirtualSessionSupervisor::Launch process;
    };
    using Prepare = std::function<std::optional<PreparedLaunch>(quint32, const VirtualSessionRegistry::Handle &, const QByteArray &)>;
    VirtualSessionHostController(Server *server, Prepare prepare, QObject *parent = nullptr);
    ~VirtualSessionHostController() override;
    bool adopt(const VirtualSessionGuardianClient::Identity &identity, const QString &workerSocket);
    // Call once before server.start(). False is fatal to startup; true means
    // intents imported, NOT desktop readiness. Keep the journal lease alive.
    bool recover(VirtualSessionJournal &journal, QString *error = nullptr);
    using StartService = std::function<bool(const QString &, const VirtualSessionRegistry::Handle &)>;
    // Journal must outlive host. Enable only after recovery and before clients.
    enum class CreateAdmission { Permitted, Maintenance };
    using AdmitCreate = std::function<CreateAdmission(quint32)>;
    // Create-only preflight, not the keeper's mandatory held maintenance lease.
    // Empty remains permitted for this unwired stage; no production enforcement.
    bool enableIndependentCreates(VirtualSessionJournal &journal, StartService start = {}, AdmitCreate admission = {},
        bool experimentalInitialLayout = false);

private:
    friend class VirtualSessionHostControllerTest;
    bool recoverRecords(const QVector<VirtualSessionJournal::Record> &records, const QString &boot, QString *error,
                        const QSet<QString> &completed = {});
    void reconcileCleanExits();
    std::optional<VirtualSessionJournal::Record> dismissalRecord(quint32 uid, const QString &id) const;
    bool dismissalEligible(quint32 uid, const QString &id) const;
    VirtualSessionControl::DismissResult dismissFailure(quint32 uid, const QString &id);
    VirtualSessionControl::CreateResult createIndependent(quint32 uid,
        const VirtualSessionControl::InitialOutputs &initialOutputs = {});
    bool startIndependentService(const QString &unit, const VirtualSessionRegistry::Handle &handle);
    struct Worker {
        VirtualSessionRegistry::Handle handle;
        std::unique_ptr<VirtualSessionBrokerLease> lease; // destroyed after endpoint
        std::unique_ptr<ConsoleWorkerEndpoint> endpoint;
        ConsoleWorkerWire::Outputs outputs;
        RemoteTopologyCatalog topology;
        QVector<VideoMonitor> wireLayout;
        QSet<int> verifiedKeyframes;
        bool multiPublished = false;
    };
    std::optional<VirtualSessionSupervisor::Launch> prepare(quint32 uid, const VirtualSessionRegistry::Handle &handle);
    bool prepareEndpoint(quint32 uid, const VirtualSessionRegistry::Handle &handle, const QString &socket, const QByteArray &token,
        std::unique_ptr<VirtualSessionBrokerLease> lease = {});
    ConsoleWorkerEndpoint *resolve(const VirtualSessionRegistry::Handle &handle);
    std::optional<RemoteTopologyCatalog::Snapshot> topologyFor(const VirtualSessionRegistry::Handle &handle) const;
    void addClient(RdpConnection *connection);
    void removeClient(quint64 id);

    Prepare m_prepare;
    std::map<QString, std::unique_ptr<Worker>> m_workers;
    quint64 m_sequence = 0;
    quint64 m_nextClient = 0;
    bool m_recoveryAttempted = false;
    VirtualSessionJournal *m_journal = nullptr;
    VirtualSessionJournal *m_recoveredJournal = nullptr;
    QString m_recoveryBoot;
    QTimer m_reconcileTimer;
    StartService m_startService;
    AdmitCreate m_admitCreate;
    std::function<bool(const VirtualSessionJournal::Record &)> m_commitIntent;
    bool m_creationBlocked = false;
    std::map<QString, VirtualSessionJournal::Record> m_newIntents;
    VirtualSessionSupervisor m_supervisor;
    VirtualSessionControl m_control;
    std::map<quint64, std::unique_ptr<VirtualSessionTransport>> m_clients;
};
}
