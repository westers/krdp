// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once

#include "ConsoleCameraWire.h"
#include "PipeWireCamera.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace KRdp
{
// The user-session half of RDPECAM. The privileged broker never opens PipeWire.
class ConsoleCameraSession : public QObject
{
    Q_OBJECT
public:
    explicit ConsoleCameraSession(bool desktop, QObject *parent = nullptr)
        : QObject(parent), m_desktop(desktop)
    {
        m_timer.setInterval(250);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            if (!m_ready && m_source.ready()) {
                m_ready = true;
                Q_EMIT result({m_control.generation, m_request, {}});
            } else if (!m_ready && m_deadline.isValid() && m_deadline.elapsed() >= 3000) {
                Q_EMIT result({m_control.generation, m_request, QStringLiteral("camera source unavailable")});
                stop();
                return;
            }
            if (!m_ready) return;
            const bool inUse = m_source.consumerActive();
            const bool capture = inUse;
            if (capture != m_capture || inUse != m_inUse) {
                m_capture = capture;
                m_inUse = inUse;
                Q_EMIT demand({m_control.generation, m_request, capture, inUse});
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
    void request(const ConsoleWorkerWire::CameraPolicy &policy)
    {
        if (!policy.generation || !policy.requestId || policy.generation != m_control.generation
            || policy.requestId <= m_request) {
            Q_EMIT result({policy.generation, policy.requestId, QStringLiteral("stale camera policy")});
            return;
        }
        stop();
        m_request = policy.requestId;
        if (!policy.enabled) {
            Q_EMIT result({m_control.generation, m_request, {}});
            return;
        }
        if (!m_desktop || !m_control.active) {
            Q_EMIT result({m_control.generation, m_request, QStringLiteral("camera requires control of a logged-in desktop")});
            return;
        }
        m_enabled = true;
        m_loopbackDevice = policy.loopbackDevice;
    }
    bool format(const ConsoleWorkerWire::CameraFormat &format)
    {
        if (!m_enabled || !m_control.active || format.generation != m_control.generation
            || format.requestId != m_request || m_source.ready()) return false;
        if (!m_source.start(QStringLiteral("console-%1-%2").arg(format.generation).arg(format.requestId),
                            format.width, format.height, format.fps, m_loopbackDevice)) {
            Q_EMIT result({m_control.generation, m_request, QStringLiteral("camera source startup failed")});
            stop();
            return false;
        }
        m_deadline.start();
        m_timer.start();
        return true;
    }
    bool frame(const ConsoleWorkerWire::CameraFrame &frame)
    {
        if (!m_ready || !m_control.active || frame.generation != m_control.generation || frame.requestId != m_request) return false;
        m_source.writeMjpeg(frame.jpeg);
        return true;
    }
    void stop()
    {
        m_timer.stop();
        m_source.stop();
        m_enabled = m_ready = m_capture = m_inUse = false;
        m_loopbackDevice.clear();
        m_deadline.invalidate();
    }
Q_SIGNALS:
    void result(const KRdp::ConsoleWorkerWire::CameraResult &result);
    void demand(const KRdp::ConsoleWorkerWire::CameraDemand &demand);
private:
    const bool m_desktop;
    bool m_enabled = false;
    bool m_ready = false;
    bool m_capture = false;
    bool m_inUse = false;
    quint64 m_request = 0;
    QString m_loopbackDevice;
    ConsoleWorkerWire::ControlState m_control;
    PipeWireCamera m_source;
    QTimer m_timer;
    QElapsedTimer m_deadline;
};
}
