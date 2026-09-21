// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include "ConsoleMicrophoneWire.h"
#include "PipeWireMicrophone.h"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace KRdp
{
// Owned and called only on the worker event-loop thread, never in the root host.
class ConsoleMicrophoneSession : public QObject
{
    Q_OBJECT
public:
    explicit ConsoleMicrophoneSession(bool desktop, QObject *parent = nullptr)
        : QObject(parent), m_desktop(desktop)
    {
        m_timer.setInterval(20);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            const auto state = m_source.state();
            if (state == PipeWireMicrophone::State::Failed
                || (!m_ready && m_deadline.elapsed() >= 3000)) {
                finish(QStringLiteral("microphone source unavailable"));
                stop();
            } else if (!m_ready && state == PipeWireMicrophone::State::Ready) {
                m_ready = true;
                finish({});
            }
        });
    }
    void setControl(ConsoleWorkerWire::ControlState control)
    {
        if (control == m_control) return;
        stop();
        if (control.generation != m_control.generation) m_request = 0;
        m_control = control;
    }
    void request(const ConsoleWorkerWire::MicrophonePolicy &policy)
    {
        if (!policy.generation || !policy.requestId || policy.generation != m_control.generation
            || policy.requestId <= m_request) {
            Q_EMIT result({policy.generation, policy.requestId, QStringLiteral("stale microphone policy")});
            return;
        }
        stop();
        m_request = policy.requestId;
        if (!policy.enabled) { finish({}); return; }
        if (!m_desktop || !m_control.active) {
            finish(QStringLiteral("microphone requires control of a logged-in desktop"));
            return;
        }
        if (!m_source.start(QStringLiteral("console-%1-%2").arg(policy.generation).arg(policy.requestId))) {
            finish(QStringLiteral("microphone source startup failed"));
            stop();
            return;
        }
        m_deadline.start();
        m_timer.start();
    }
    bool audio(const ConsoleWorkerWire::MicrophoneAudio &audio)
    {
        if (!m_ready || !m_control.active || audio.generation != m_control.generation
            || audio.requestId != m_request || audio.pcm.isEmpty() || audio.pcm.size() > 3840 || audio.pcm.size() % 4
            || m_source.state() != PipeWireMicrophone::State::Ready) return false;
        m_source.write(audio.pcm);
        return true;
    }
    void stop()
    {
        m_ready = false;
        m_timer.stop();
        m_source.stop();
    }
Q_SIGNALS:
    void result(const ConsoleWorkerWire::MicrophoneResult &result);
private:
    void finish(const QString &error) { Q_EMIT result({m_control.generation, m_request, error}); }
    const bool m_desktop;
    bool m_ready = false;
    quint64 m_request = 0;
    ConsoleWorkerWire::ControlState m_control;
    PipeWireMicrophone m_source;
    QTimer m_timer;
    QElapsedTimer m_deadline;
};
}
