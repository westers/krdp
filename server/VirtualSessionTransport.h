// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionControl.h"
#include "ConsoleWorkerEndpoint.h"
#include "ConsoleWorkerSession.h"
#include <RdpConnection.h>
#include <QPointer>
#include <QTimer>

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
    // Private identity seam for socket-free tests; production callers use only
    // RdpConnection's authenticated PAM identity, never record-supplied identity.
    QJsonObject request(const QJsonObject &record, std::optional<quint32> uid);
    void deliverControlRecord(const QJsonObject &record, std::optional<quint32> uid);
    bool bind();
    bool activateBinding(const VirtualSessionRegistry::Handle &handle, QPointer<ConsoleWorkerEndpoint> endpoint);
    bool attachmentMatches(const VirtualSessionRegistry::Handle &handle) const;
    bool authorized() const;
    bool authorized(std::optional<quint32> uid) const;
    bool forwardVideoQuality(quint64 generation, quint8 quality, std::optional<quint32> uid);
    void restoreFixedVideoQuality(std::optional<quint32> uid);
    void stopMicrophone();
    void stopMicrophone(std::optional<quint32> uid);
    QJsonObject microphoneResult(const ConsoleWorkerWire::MicrophoneResult &, std::optional<quint32> uid);
    QJsonObject microphoneTimeout();
    void pumpMicrophone();
    bool forwardMicrophone(const QByteArray &pcm, std::optional<quint32> uid);
    QJsonObject mediaReply(bool ok, const QString &error = {}) const;
    void closed();
    quint64 m_client;
    QPointer<RdpConnection> m_connection;
    QPointer<VirtualSessionControl> m_control;
    Resolve m_resolve;
    quint64 &m_sequence;
    QPointer<ConsoleWorkerEndpoint> m_endpoint;
    std::optional<VirtualSessionRegistry::Handle> m_handle;
    ConsoleWorkerSession m_session;
    QList<QMetaObject::Connection> m_workerConnections;
    bool m_playback = false;
    bool m_silenceHost = false;
    bool m_externalMicrophone = false;
    bool m_microphoneReady = false;
    bool m_revoking = false;
    bool m_mediaDispatch = false;
    quint64 m_controlGeneration = 0;
    quint64 m_nextMicrophoneId = 0;
    ConsoleWorkerWire::MicrophonePolicy m_microphonePolicy;
    QTimer m_microphoneDeadline;
    QTimer m_microphonePump;
    bool m_closing = false;
};
}
