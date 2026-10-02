// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <array>
#include <functional>

struct BrokerServiceState {
    bool known = false;
    QString loadState, activeState, subState, unitFileState;
    quint32 mainPid = 0;
};

// The transport accepts only the two fixed host services. Neither QML nor
// a D-Bus reply can select another unit or a user-manager operation.
class BrokerServiceTransport : public QObject {
    Q_OBJECT
public:
    enum Operation { Start, Stop, Restart, Enable, Disable };
    using QueryDone = std::function<void(BrokerServiceState, QString)>;
    using Done = std::function<void(QString)>;
    using QObject::QObject;
    virtual void query(int route, QueryDone done) = 0;
    virtual void operate(int route, Operation operation, Done done) = 0;
    static QString unit(int route);
Q_SIGNALS:
    void changed();
};

class SystemBrokerServiceTransport : public BrokerServiceTransport {
    Q_OBJECT
public:
    explicit SystemBrokerServiceTransport(const QDBusConnection &bus, QObject *parent = nullptr);
    void query(int route, QueryDone done) override;
    void operate(int route, Operation operation, Done done) override;
private:
    Q_SLOT void jobRemoved(uint id, const QDBusObjectPath &path, const QString &unit, const QString &result);
    Q_SLOT void unitFilesChanged();
    Q_SLOT void unitPropertiesChanged(const QString &interface, const QVariantMap &values, const QStringList &invalidated);
    Q_SLOT void managerOwnerChanged(const QString &, const QString &, const QString &);
    void subscribe();
    void finish(const QString &error);
    QDBusConnection m_bus;
    QTimer m_deadline;
    Done m_done;
    QString m_unit, m_job;
    QMap<QString, QString> m_earlyJobs;
    quint64 m_generation = 0, m_managerGeneration = 0;
    bool m_subscribed = false;
    bool m_subscriptionPending = false;
    std::array<QString, 2> m_unitPaths;
};

class BrokerServices : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList services READ services NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
public:
    explicit BrokerServices(QObject *parent = nullptr);
    // Dependency injection is C++ only. The production module always uses
    // the system bus; there is no QML bus/program/unit selector.
    explicit BrokerServices(BrokerServiceTransport *transport, QObject *parent = nullptr);
    QVariantList services() const;
    bool busy() const;
    Q_INVOKABLE void refresh(bool clearErrors = true);
    Q_INVOKABLE bool perform(const QString &route, const QString &operation);
Q_SIGNALS:
    void changed();
private:
    struct Entry { BrokerServiceState state; bool querying = false, pending = false; QString error; };
    void query(int route, bool clearError);
    BrokerServiceTransport *m_transport;
    std::array<Entry, 2> m_entries;
    bool m_refreshPending = false;
};
