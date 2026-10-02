// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QVariantMap>

// Root policy metadata only. Passwords are transient pending stdin data and
// never appear in this object's readable properties, arguments or messages.
class BrokerAuthenticationSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap policy READ policy NOTIFY changed)
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool modified READ modified NOTIFY changed)
    Q_PROPERTY(bool lastSaveRequiresRestart READ lastSaveRequiresRestart NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit BrokerAuthenticationSettings(QObject *parent = nullptr);
    // C++-only process injection for protocol/cancellation tests. QML cannot
    // choose an executable, helper path or command line.
    BrokerAuthenticationSettings(const QString &program, const QStringList &arguments, QObject *parent);
    QVariantMap policy() const;
    bool loaded() const { return !m_snapshot.isEmpty(); }
    bool busy() const { return m_process != nullptr; }
    bool modified() const { return m_pending != m_snapshot; }
    bool lastSaveRequiresRestart() const { return m_lastSaveRequiresRestart; }
    QString error() const { return m_error; }
    Q_INVOKABLE bool reload();
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool setPam(const QString &route, const QString &mode, const QStringList &accounts);
    Q_INVOKABLE bool setAlias(const QString &route, const QString &alias, const QString &owner, const QString &password);
    Q_INVOKABLE bool removeAlias(const QString &route, const QString &alias);
    Q_INVOKABLE void discard();
    Q_INVOKABLE bool undoRemoveAlias();
Q_SIGNALS:
    void changed();
private:
    bool start(const QJsonObject &request, bool saving);
    bool editableRoute(const QString &route) const;
    bool reject(const QString &error);
    QString m_program;
    QStringList m_arguments;
    QProcess *m_process = nullptr;
    QJsonObject m_snapshot, m_pending;
    QString m_error;
    bool m_lastSaveRequiresRestart = false;
    QJsonObject m_removedAlias;
    QString m_removedRoute;
};
