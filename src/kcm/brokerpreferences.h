// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "BrokerUserSettings.h"
#include <QObject>
#include <QVariantList>

class BrokerPreferences : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap values READ values NOTIFY changed)
    Q_PROPERTY(QStringList lockedKeys READ lockedKeys NOTIFY changed)
    Q_PROPERTY(QVariantList definitions READ definitions CONSTANT)
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool modified READ modified NOTIFY changed)
    Q_PROPERTY(bool canSave READ canSave NOTIFY changed)
    Q_PROPERTY(bool reconnectRequired READ reconnectRequired NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit BrokerPreferences(QObject *parent = nullptr);
    // Tests inject a private directory in C++; QML cannot select a file/UID.
    explicit BrokerPreferences(const QString &directory, QObject *parent = nullptr);
    QVariantMap values() const { return m_pending; }
    QStringList lockedKeys() const { return m_locked; }
    QVariantList definitions() const;
    // Heading of a page section named by the definitions' `section` ("advanced" for the advanced group).
    Q_INVOKABLE QString sectionTitle(const QString &section) const;
    bool loaded() const { return m_loaded; }
    bool modified() const { return m_loaded && m_pending != m_snapshot; }
    bool canSave() const;
    bool reconnectRequired() const { return m_reconnect; }
    QString error() const;
    bool representsDefaults() const;
    Q_INVOKABLE bool reload();
    Q_INVOKABLE bool setValue(const QString &key, const QString &value);
    Q_INVOKABLE bool inherit(const QString &key);
    Q_INVOKABLE void defaults();
    Q_INVOKABLE void discard();
    Q_INVOKABLE bool save();
Q_SIGNALS:
    void changed();
private:
    struct Read { QByteArray document; bool absent = false; QString error; };
    Read read() const;
    bool reject(const QString &error);
    void adopt(const Read &read);
    QString m_directory;
    QByteArray m_document;
    QVariantMap m_snapshot, m_pending;
    QStringList m_locked;
    QString m_error;
    bool m_loaded = false, m_absent = false, m_reconnect = false;
};
