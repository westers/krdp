// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "VirtualSessionRegistry.h"
#include "VirtualSessionGuardianClient.h"
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <functional>
#include <memory>

namespace KRdp
{
/** Owns namespace-leader processes independently of RDP transports.
 * The trusted launch factory must construct an isolated, identity-dropped
 * namespace leader whose exit tears down all descendants. No program, argv,
 * environment or readiness Handle comes from client-supplied JSON.
 * Destruction tears down legacy owned processes, but only disconnects adopted
 * guardians. Adoption requires a trusted identity plus authenticated guardian
 * liveness AND fresh capture before attach. Durable identity storage and an
 * independent service launcher remain caller responsibilities.
 */
class VirtualSessionSupervisor : public QObject
{
public:
    using Handle = VirtualSessionRegistry::Handle;
    struct Launch {
        QString program;
        QStringList arguments;
        QProcessEnvironment environment; // explicit, never inherit physical desktop
        std::function<void()> childSetup; // trusted identity/descriptor setup only
    };
    using LaunchFactory = std::function<std::optional<Launch>(quint32, const Handle &)>;

    explicit VirtualSessionSupervisor(LaunchFactory factory, int readyTimeoutMs = 30000,
                                      int stopTimeoutMs = 5000, QObject *parent = nullptr);
    ~VirtualSessionSupervisor() override;
    std::optional<Handle> create(quint32 authenticatedUid);
    // Trusted durable launch identity, not RDP JSON. Starts unavailable until
    // guardian handshake AND authenticated fresh capture have both succeeded.
    std::optional<Handle> adopt(const VirtualSessionGuardianClient::Identity &identity, bool awaitingService = false);
    bool canAdopt(quint32 uid, const QString &id) const { return m_registry.canReserve({{uid, id}}); }
    void setGuardianAvailableCallback(std::function<void(const Handle &)> callback) { m_guardianAvailable = std::move(callback); }
    bool canRecover(const QList<QPair<quint32, QString>> &identities) const { return m_registry.empty() && m_registry.canReserve(identities); }
    // Trusted intent without a safe endpoint: visible Failed, no socket or spawn.
    bool rememberUnavailable(const VirtualSessionGuardianClient::Identity &identity);
    void setUnavailableCallback(std::function<void(const Handle &)> callback) { m_unavailable = std::move(callback); }
    std::optional<Handle> recreate(quint32 authenticatedUid, const QString &id);
    QList<VirtualSessionRegistry::Summary> list(quint32 uid) const { return m_registry.list(uid); }
    std::optional<Handle> attach(quint32 uid, const QString &id, quint64 client);
    bool disconnect(const Handle &handle, quint64 client) { return m_registry.disconnect(handle, client); }
    bool stop(quint32 uid, const QString &id);
    bool forget(quint32 uid, const QString &id);
    bool captureReady(const Handle &handle);
    void captureUnavailable(const Handle &handle);

private:
    friend class VirtualSessionHostControllerTest;
    friend class VirtualSessionHostController;
    // Host-only: both immutable ordered-exit and final cleanup proofs required.
    bool forgetReconciled(const VirtualSessionGuardianClient::Identity &identity);
    struct Runtime {
        Handle handle;
        QProcess process;
        QTimer deadline;
        bool stopping = false;
        bool failed = false;
        std::unique_ptr<VirtualSessionGuardianClient> guardian;
        VirtualSessionGuardianClient::Identity identity;
        QTimer poll;
        bool observedRunning = false;
        bool captureObserved = false;
        bool terminalConfirmed = false;
        bool stopSent = false;
        bool unresolvedIntent = false;
        bool awaitingService = false;
        bool retiring = false;
    };
    void launch(quint32 uid, const Handle &handle);
    Runtime *runtime(const Handle &handle);
    void terminate(Runtime &runtime);
    void queryGuardian(Runtime &runtime);
    void guardianUnavailable(Runtime &runtime);
    void guardianReply(Runtime &runtime, const QString &phase, bool running);
    void notifyUnavailable(const Handle &handle);
    LaunchFactory m_factory;
    int m_readyTimeoutMs;
    int m_stopTimeoutMs;
    VirtualSessionRegistry m_registry;
    std::map<QString, std::unique_ptr<Runtime>> m_runtimes;
    std::function<void(const Handle &)> m_unavailable;
    std::function<void(const Handle &)> m_guardianAvailable;
};
}
