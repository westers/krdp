// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionTransport.h"
#include "VirtualSessionBrokerLease.h"
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

private:
    friend class VirtualSessionHostControllerTest;
    struct Worker {
        VirtualSessionRegistry::Handle handle;
        std::unique_ptr<VirtualSessionBrokerLease> lease; // destroyed after endpoint
        std::unique_ptr<ConsoleWorkerEndpoint> endpoint;
        ConsoleWorkerWire::Outputs outputs;
    };
    std::optional<VirtualSessionSupervisor::Launch> prepare(quint32 uid, const VirtualSessionRegistry::Handle &handle);
    bool prepareEndpoint(quint32 uid, const VirtualSessionRegistry::Handle &handle, const QString &socket, const QByteArray &token,
        std::unique_ptr<VirtualSessionBrokerLease> lease = {});
    ConsoleWorkerEndpoint *resolve(const VirtualSessionRegistry::Handle &handle);
    void addClient(RdpConnection *connection);
    void removeClient(quint64 id);

    Prepare m_prepare;
    std::map<QString, std::unique_ptr<Worker>> m_workers;
    quint64 m_sequence = 0;
    quint64 m_nextClient = 0;
    VirtualSessionSupervisor m_supervisor;
    VirtualSessionControl m_control;
    std::map<quint64, std::unique_ptr<VirtualSessionTransport>> m_clients;
};
}
