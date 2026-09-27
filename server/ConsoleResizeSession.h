// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "ConsoleResizeExecutor.h"
#include "ConsoleWorkerWire.h"
#include "OutputRestoreJournal.h"
#include <QMap>
#include <QTimer>

namespace KRdp
{
class ConsoleResizeSession : public QObject
{
    Q_OBJECT
public:
    explicit ConsoleResizeSession(QObject *parent = nullptr, ConsoleResizeExecutor::Runner runner = {});
    void setControl(ConsoleWorkerWire::ControlState control);
    void request(const ConsoleWorkerWire::Resize &request);
    void captured(const ConsoleWorkerWire::Outputs &outputs, bool keyframe);
    void stop();
    /**
     * Record every applied Fit in the shared output-restore journal before
     * the mode changes, and drop it once restored (AUD-C-3). A crash leaves
     * the entry for the next worker to replay. Null disables journaling.
     */
    void setJournal(OutputRestoreJournal *journal, const QString &session);
    bool inputAllowed() const;
    bool changing() const;
Q_SIGNALS:
    void result(const KRdp::ConsoleWorkerWire::ResizeResult &result);
    void mutationStarting();
    void keyframeNeeded();
    void stopped(const QString &restorationError);
private:
    void completed(const ConsoleResize::Plan &plan, const QString &error);
    void finishRequest(const QString &error);
    void drain();
    void journalApplying(const ConsoleResize::Plan &plan, bool restoring);
    void writeJournal();
    ConsoleResizeExecutor m_executor;
    OutputRestoreJournal *m_journal = nullptr;
    QString m_journalSession;
    // Outputs whose Fit is (or may be) applied and not yet verifiably
    // restored: previousMode/previousScale are the originals to put back.
    QMap<QString, ConsoleResize::Plan> m_journaled;
    ConsoleWorkerWire::ControlState m_control;
    std::optional<ConsoleWorkerWire::Resize> m_pending;
    QMap<QString, ConsoleResize::Plan> m_held;
    QMap<QString, ConsoleResize::Plan> m_recovery;
    QTimer m_captureDeadline;
    bool m_waitingCapture = false;
    bool m_draining = false;
    bool m_restoring = false;
    bool m_stopping = false;
    QString m_restoreError;
};
}
