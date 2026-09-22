// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionControl.h"
#include "ConsoleWorkerEndpoint.h"
#include "ConsoleWorkerSession.h"
#include <RdpConnection.h>
#include <QPointer>

namespace KRdp
{
/** One authenticated RDP transport attached to a managed desktop.
 * Construct before RdpConnection's queued initialization. Host owns control,
 * a global monotonic worker-control sequence and endpoint resolution. Its
 * VirtualSessionControl release hook MUST call revoke() for the named client
 * before returning (including a stop requested by another owner transport).
 * Does not create desktops, advertise capabilities or install a listener.
 */
class VirtualSessionTransport : public QObject
{
public:
    using Resolve = std::function<ConsoleWorkerEndpoint *(const VirtualSessionRegistry::Handle &)>;
    VirtualSessionTransport(quint64 client, RdpConnection *connection, VirtualSessionControl &control,
                            Resolve resolve, quint64 &controlSequence, QObject *parent = nullptr);
    ~VirtualSessionTransport() override;
    QJsonObject request(const QJsonObject &record);
    void revoke();
    void unavailable();

private:
    friend class VirtualSessionTransportTest;
    bool bind();
    bool authorized() const;
    void closed();
    quint64 m_client;
    QPointer<RdpConnection> m_connection;
    VirtualSessionControl &m_control;
    Resolve m_resolve;
    quint64 &m_sequence;
    QPointer<ConsoleWorkerEndpoint> m_endpoint;
    std::optional<VirtualSessionRegistry::Handle> m_handle;
    ConsoleWorkerSession m_session;
    QList<QMetaObject::Connection> m_workerConnections;
    bool m_playback = false;
};
}
