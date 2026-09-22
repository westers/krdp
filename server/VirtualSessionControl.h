// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionSupervisor.h"
#include <QJsonObject>
#include <QHash>
#include <deque>

namespace KRdp
{
/** Server-side virtual-session command dispatch. The broker supplies a unique
 * transport ID and RdpConnection::authenticatedPamUid(), never JSON identity.
 * Release must synchronously revoke that transport's input/media destination.
 * Not advertised until the broker wires capture, release and transport lifetime.
 */
class VirtualSessionControl
{
public:
    using Handle = VirtualSessionRegistry::Handle;
    using Release = std::function<void(quint64, const Handle &)>;
    explicit VirtualSessionControl(VirtualSessionSupervisor &supervisor, Release release)
        : m_supervisor(supervisor), m_release(std::move(release)) {}
    QJsonObject request(std::optional<quint32> authenticatedUid, quint64 client, const QJsonObject &record);
    void disconnected(quint64 client);
    std::optional<Handle> attachment(quint64 client) const;
    void setCreateHandler(std::function<std::optional<Handle>(quint32)> create) { m_create = std::move(create); }

private:
    struct Reply { QJsonObject request; QJsonObject response; };
    struct Transport {
        quint32 uid;
        std::optional<Handle> attached;
        std::deque<Reply> replies;
    };
    QJsonObject dispatch(quint32 uid, quint64 client, Transport &transport, const QJsonObject &request);
    void release(quint64 client, Transport &transport);
    VirtualSessionSupervisor &m_supervisor;
    Release m_release;
    std::function<std::optional<Handle>(quint32)> m_create;
    QHash<quint64, Transport> m_transports;
};
}
