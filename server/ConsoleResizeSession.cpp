// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "ConsoleResizeSession.h"
#include <utility>
using namespace KRdp;

ConsoleResizeSession::ConsoleResizeSession(QObject *parent, ConsoleResizeExecutor::Runner runner)
    : QObject(parent), m_executor(this, std::move(runner))
{
    m_captureDeadline.setSingleShot(true);
    m_captureDeadline.setInterval(10000);
    connect(&m_executor, &ConsoleResizeExecutor::changing, this, &ConsoleResizeSession::mutationStarting);
    connect(&m_executor, &ConsoleResizeExecutor::finished, this, &ConsoleResizeSession::completed);
    connect(&m_captureDeadline, &QTimer::timeout, this, [this]() {
        if (!m_pending && !m_recovery.isEmpty()) {
            m_restoreError = QStringLiteral("capture did not reach restored geometry");
            return; // Keep input disabled rather than use stale coordinates.
        }
        m_draining = true;
        finishRequest(QStringLiteral("capture did not reach the requested display geometry"));
        drain();
    });
}

bool ConsoleResizeSession::inputAllowed() const
{
    return m_control.active && !changing() && !m_stopping && m_restoreError.isEmpty();
}

bool ConsoleResizeSession::changing() const
{
    return m_executor.busy() || m_pending.has_value() || m_restoring || m_draining || !m_recovery.isEmpty();
}

void ConsoleResizeSession::setControl(ConsoleWorkerWire::ControlState control)
{
    if (control == m_control) {
        return;
    }
    const bool oldOwner = m_control.active;
    m_control = control;
    if (oldOwner || !control.active) {
        m_executor.cancelBeforeApply();
        m_draining = true;
        if (m_waitingCapture) {
            finishRequest(QStringLiteral("console control changed during resize"));
        }
        drain();
    }
}

void ConsoleResizeSession::request(const ConsoleWorkerWire::Resize &request)
{
    if (!inputAllowed() || request.generation != m_control.generation) {
        Q_EMIT result({request.requestId, request.generation, QStringLiteral("console resize requires current control and an idle worker")});
        return;
    }
    m_pending = request;
    if (!m_executor.resize(request.output, request.pixels, request.scale)) {
        finishRequest(QStringLiteral("physical output executor is busy"));
    }
}

void ConsoleResizeSession::completed(const ConsoleResize::Plan &plan, const QString &error)
{
    if (m_restoring) {
        m_restoring = false;
        if (!error.isEmpty()) {
            m_restoreError = error;
        } else if (plan.observedPixels.isValid() && plan.observedScale > 0) {
            m_recovery.insert(plan.output, plan);
        }
        drain();
        return;
    }
    if (!m_pending) {
        return;
    }
    if (plan.valid() && plan.apply != plan.restore) {
        auto held = plan;
        const auto previous = m_held.constFind(plan.output);
        if (previous != m_held.cend() && previous->mode == plan.previousMode && std::abs(previous->scale - plan.previousScale) < 0.000001) {
            // Repeated Fit restores the pre-connection state, unless someone
            // locally changed the display between requests.
            held.previousMode = previous->previousMode;
            held.previousScale = previous->previousScale;
            held.restore = previous->restore;
        }
        m_held.insert(plan.output, held);
    }
    if (m_draining || m_stopping || !m_control.active || m_pending->generation != m_control.generation) {
        finishRequest(QStringLiteral("console control changed during resize"));
        drain();
    } else if (!error.isEmpty()) {
        m_draining = true; // A failed readback may still have changed the monitor.
        finishRequest(error);
        drain();
    } else {
        m_waitingCapture = true;
        m_captureDeadline.start();
        Q_EMIT keyframeNeeded();
    }
}

void ConsoleResizeSession::captured(const ConsoleWorkerWire::Outputs &outputs, bool keyframe)
{
    if (keyframe && !m_executor.busy() && !m_restoring && !m_draining && !m_recovery.isEmpty()) {
        for (const auto &output : outputs.monitors) {
            const auto expected = m_recovery.constFind(output.name);
            if (expected != m_recovery.cend() && std::abs(output.scale - expected->observedScale) < 0.000001
                && QSize(qRound(output.geometry.width() * output.scale), qRound(output.geometry.height() * output.scale)) == expected->observedPixels) {
                m_recovery.remove(output.name);
            }
        }
        if (m_recovery.isEmpty()) {
            m_captureDeadline.stop();
            if (m_restoreError == QStringLiteral("capture did not reach restored geometry")) {
                m_restoreError.clear(); // A late, correct keyframe can recover safely.
            }
        }
    }
    if (!m_pending || !m_waitingCapture || !keyframe) {
        return;
    }
    for (const auto &output : outputs.monitors) {
        if (output.name == m_pending->output && std::abs(output.scale - m_pending->scale) < 0.000001
            && QSize(qRound(output.geometry.width() * output.scale), qRound(output.geometry.height() * output.scale)) == m_pending->pixels) {
            finishRequest({});
            return;
        }
    }
}

void ConsoleResizeSession::finishRequest(const QString &error)
{
    m_captureDeadline.stop();
    m_waitingCapture = false;
    const auto pending = std::exchange(m_pending, std::nullopt);
    if (pending) {
        Q_EMIT result({pending->requestId, pending->generation, error});
    }
}

void ConsoleResizeSession::stop()
{
    if (m_stopping) {
        return;
    }
    m_stopping = true;
    m_executor.cancelBeforeApply();
    m_draining = true;
    if (m_waitingCapture) {
        finishRequest(QStringLiteral("console worker is stopping"));
    }
    drain();
}

void ConsoleResizeSession::drain()
{
    if (m_executor.busy() || m_pending || m_restoring) {
        return;
    }
    if (!m_held.isEmpty()) {
        const auto first = m_held.begin();
        const auto plan = first.value();
        m_held.erase(first);
        m_restoring = true;
        m_executor.restore(plan);
        return;
    }
    m_draining = false;
    if (m_stopping) {
        m_recovery.clear();
        m_captureDeadline.stop();
        Q_EMIT stopped(m_restoreError);
        return;
    }
    if (!m_recovery.isEmpty()) {
        m_captureDeadline.start();
    }
    Q_EMIT keyframeNeeded();
}
