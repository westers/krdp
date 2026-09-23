// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "VirtualResizeExecutor.h"
#include "ConsoleWorkerWire.h"
#include <QTimer>

namespace KRdp
{
class VirtualResizeSession : public QObject
{
    Q_OBJECT
public:
    explicit VirtualResizeSession(QObject *parent = nullptr, VirtualResizeExecutor::Runner runner = {});
    void setControl(ConsoleWorkerWire::ControlState control);
    void request(const ConsoleWorkerWire::Resize &request);
    void captured(const ConsoleWorkerWire::Outputs &outputs, QSize frameMetadataPixels, QSize encodedPayloadPixels, bool keyframe, quint64 epoch);
    void captureRestartReady(quint64 epoch);
    void captureRestartFailed(quint64 epoch);
    void captureStateChanged(quint64 epoch, bool active);
    quint64 captureEpoch() const { return m_captureEpoch; }
    bool framesAllowed() const { return m_forwardFrames && !m_stopping && !changing() && m_blockedError.isEmpty(); }
    void stop();
    bool inputAllowed() const;
    bool changing() const;
Q_SIGNALS:
    void mutationStarting();
    void keyframeNeeded();
    void captureRefreshNeeded(quint64 epoch);
    void result(const KRdp::ConsoleWorkerWire::ResizeResult &result);
    void stopped(const QString &error);
private:
    friend class VirtualResizeSessionTest;
    void completed(const VirtualResizeExecutor::Result &result);
    void cancel(const QString &error);
    void recover();
    void finishRequest(const QString &error);
    void finishStop();
    void refreshCapture();
    VirtualResizeExecutor m_executor;
    ConsoleWorkerWire::ControlState m_control;
    std::optional<ConsoleWorkerWire::Resize> m_pending;
    std::optional<VirtualResize::Plan> m_plan;
    std::optional<VirtualResize::Snapshot> m_captureTarget;
    QTimer m_deadline;
    QString m_cancelError;
    QString m_blockedError;
    bool m_rollingBack = false;
    bool m_stopping = false;
    bool m_stopEmitted = false;
    quint64 m_captureEpoch = 0;
    enum class CapturePhase { WaitingTeardown, WaitingActive, Active, Invalidated };
    CapturePhase m_capturePhase = CapturePhase::WaitingTeardown;
    bool m_forwardFrames = true;
    bool m_acceptingRequest = false;
};
}
