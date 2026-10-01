// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "BrokerHostSettings.h"
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QUrl>
#include <QVariantList>

// One immutable route per model. Switching pages never discards another route's
// drafts. Only public metadata is readable; imported key bytes remain in C++.
class BrokerHostSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString scope READ scope CONSTANT)
    Q_PROPERTY(QVariantMap values READ values NOTIFY changed)
    Q_PROPERTY(QVariantMap unitDefaults READ unitDefaults CONSTANT)
    Q_PROPERTY(QVariantList definitions READ definitions CONSTANT)
    Q_PROPERTY(QVariantMap metadata READ metadata NOTIFY changed)
    Q_PROPERTY(QVariantMap importMetadata READ importMetadata NOTIFY changed)
    Q_PROPERTY(QString tlsMode READ tlsMode NOTIFY changed)
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool modified READ modified NOTIFY changed)
    Q_PROPERTY(bool canSave READ canSave NOTIFY changed)
    Q_PROPERTY(bool applicationRequired READ applicationRequired NOTIFY changed)
    Q_PROPERTY(bool outcomeUnknown READ outcomeUnknown NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    using Scope = KRdp::BrokerHostSettings::Scope;
    explicit BrokerHostSettings(Scope scope, QObject *parent = nullptr);
    // Test-only executable/timing injection in C++; unavailable to QML.
    BrokerHostSettings(Scope scope, const QString &program, const QStringList &arguments, int timeoutMs, QObject *parent = nullptr);
    ~BrokerHostSettings() override;
    QString scope() const;
    QVariantMap values() const { return m_pending; }
    QVariantMap unitDefaults() const;
    QVariantList definitions() const;
    QVariantMap metadata() const;
    QVariantMap importMetadata() const { return m_importMetadata; }
    QString tlsMode() const { return m_tlsMode; }
    bool loaded() const { return !m_snapshot.isEmpty(); }
    bool busy() const { return m_process != nullptr; }
    bool modified() const;
    bool canSave() const;
    bool applicationRequired() const { return m_applicationRequired; }
    bool outcomeUnknown() const { return m_outcomeUnknown; }
    QString error() const;
    Q_INVOKABLE bool reload();
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool setValue(const QString &key, const QString &value);
    Q_INVOKABLE bool inherit(const QString &key);
    Q_INVOKABLE bool chooseTls(const QString &mode);
    Q_INVOKABLE bool importTls(const QUrl &certificate, const QUrl &key);
    Q_INVOKABLE void clearTlsImport();
    Q_INVOKABLE void defaults();
    Q_INVOKABLE void discard();
Q_SIGNALS:
    void changed();
private:
    QString validationError() const;
    bool reject(const QString &error);
    bool start(QJsonObject request, bool saving);
    void clearImport();
    Scope m_scope;
    QString m_program;
    QStringList m_arguments;
    int m_timeoutMs;
    QProcess *m_process = nullptr;
    QJsonObject m_snapshot;
    QVariantMap m_pending, m_importMetadata;
    QByteArray m_certificate, m_key;
    QString m_tlsMode = QStringLiteral("keep"), m_error;
    bool m_applicationRequired = false, m_outcomeUnknown = false;
};
