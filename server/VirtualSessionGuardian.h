// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualSessionSupervisor.h"
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSet>

namespace KRdp
{
/** Desktop-side process owner, independent of individual broker connections.
 * Runs as the desktop UID under an independent service, not as an RDP child.
 * The trusted child must be a namespace leader with descendant cleanup.
 * Status reports process liveness only, never capture readiness. Registry
 * adoption must additionally authenticate the worker and its capture generation.
 */
class VirtualSessionGuardian : public QObject
{
public:
    explicit VirtualSessionGuardian(QObject *parent = nullptr);
    ~VirtualSessionGuardian() override;
    bool start(quint32 uid, const QString &session, const QByteArray &token,
        const QString &socket, const VirtualSessionSupervisor::Launch &launch, QString *error = nullptr);
    QString phase() const { return m_phase; }
    QString incarnation() const { return m_incarnation; }
    qint64 processId() const { return m_process.processId(); }
private:
    void accept();
    QJsonObject request(const QJsonObject &record);
    void stop();
    QLocalServer m_server;
    QProcess m_process;
    QSet<QLocalSocket *> m_clients;
    QString m_session;
    QString m_incarnation;
    QString m_phase = QStringLiteral("absent");
    QByteArray m_token;
    quint32 m_uid = 0;
    bool m_started = false;
};
}
