// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "VirtualSessionRegistry.h"
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
 * Not yet a persistent service/recovery implementation: destruction explicitly
 * tears down these owned processes. Transport disconnect never destroys this
 * object. Restart adoption requires a separate validated runtime handshake.
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
    std::optional<Handle> recreate(quint32 authenticatedUid, const QString &id);
    QList<VirtualSessionRegistry::Summary> list(quint32 uid) const { return m_registry.list(uid); }
    std::optional<Handle> attach(quint32 uid, const QString &id, quint64 client);
    bool disconnect(const Handle &handle, quint64 client) { return m_registry.disconnect(handle, client); }
    bool stop(quint32 uid, const QString &id);
    bool forget(quint32 uid, const QString &id);
    bool captureReady(const Handle &handle);

private:
    struct Runtime {
        Handle handle;
        QProcess process;
        QTimer deadline;
        bool stopping = false;
        bool failed = false;
    };
    void launch(quint32 uid, const Handle &handle);
    Runtime *runtime(const Handle &handle);
    void terminate(Runtime &runtime);
    LaunchFactory m_factory;
    int m_readyTimeoutMs;
    int m_stopTimeoutMs;
    VirtualSessionRegistry m_registry;
    std::map<QString, std::unique_ptr<Runtime>> m_runtimes;
};
}
