// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ConsoleWorkerWire.h"
#include <QDBusConnection>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
namespace KRdp {
/** Exact-version native compositor observer, with a bounded, owner-bound lease. */
class KWinPointerCapture : public QObject {
    Q_OBJECT
public:
    explicit KWinPointerCapture(QObject *parent = nullptr);
    ~KWinPointerCapture() override;
    void setControl(ConsoleWorkerWire::ControlState control);
    void request(const QJsonObject &request);
    void stop();
    bool permitsRelativeInput() const { return m_leaseEpoch.isEmpty() || m_permitted; }
Q_SIGNALS:
    void stateChanged(const QJsonObject &state);
private Q_SLOTS:
    void nativeState(const QString &state);
private:
    void initialize(bool load = true);
    void publish(QJsonObject state, const QString &id = {});
    void unavailable(const QString &reason, const QString &id = {});
    void policy(const QJsonObject &request);
    void release(bool refresh = true);
    ConsoleWorkerWire::ControlState m_control;
    QString m_epoch, m_leaseEpoch, m_leaseGeneration;
    quint64 m_serial = 0;
    bool m_stopped = false, m_releasing = false, m_permitted = false, m_faulted = false;
};
}
