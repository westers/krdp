// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QTimer>

namespace KRdp {
/** One bounded authenticated exchange, never ownership of the desktop process.
 * Identity comes from trusted launch/recovery state, NOT RDP JSON. Even status
 * requires the expected incarnation: this client cannot adopt by pathname/PID.
 * Successful status proves guardian process state, not capture readiness.
 */
class VirtualSessionGuardianClient : public QObject
{
    Q_OBJECT
public:
    struct Identity {
        quint32 uid = 0;
        QString session;
        QString incarnation;
        QString socket;
        QByteArray token;
    };
    enum class Operation { Status, Stop };
    explicit VirtualSessionGuardianClient(QObject *parent = nullptr, int timeoutMs = 3000);
    ~VirtualSessionGuardianClient() override;
    // Returns false without socket activity for invalid identity or a busy client.
    bool request(const Identity &identity, Operation operation);
    bool busy() const { return m_busy; }
Q_SIGNALS:
    void received(const QString &phase, bool processRunning);
    void failed(const QString &message);
private:
    void fail(const QString &message);
    void read();
    QLocalSocket m_socket;
    QTimer m_deadline;
    Identity m_identity;
    Operation m_operation = Operation::Status;
    QByteArray m_input;
    QString m_requestId;
    bool m_busy = false;
};
}
