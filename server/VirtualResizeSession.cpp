// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualResizeSession.h"
#include <QPointer>
#include <QScopeGuard>
#include <utility>
#include <cmath>
#include <limits>

using namespace KRdp;
namespace V = KRdp::VirtualResize;

VirtualResizeSession::VirtualResizeSession(QObject *parent, VirtualResizeExecutor::Runner runner)
    : QObject(parent), m_executor(this, std::move(runner))
{
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(10000);
    connect(&m_executor, &VirtualResizeExecutor::mutationStarting, this, &VirtualResizeSession::mutationStarting);
    connect(&m_executor, &VirtualResizeExecutor::finished, this, &VirtualResizeSession::completed);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_pending) {
            cancel(QStringLiteral("capture did not reach requested virtual geometry"));
        } else {
            m_blockedError = QStringLiteral("capture did not reach recovered virtual geometry");
            // Keep target and input gate: a late matching keyframe may recover.
        }
    });
}

bool VirtualResizeSession::changing() const
{
    return m_executor.busy() || m_pending || m_rollingBack || m_captureTarget;
}

bool VirtualResizeSession::inputAllowed() const
{
    return m_control.active && !m_stopping && !changing() && m_blockedError.isEmpty();
}

void VirtualResizeSession::setControl(ConsoleWorkerWire::ControlState control)
{
    if (m_control == control) return;
    m_control = control;
    if (m_pending) cancel(QStringLiteral("virtual control changed during resize"));
    // No retained-success restoration: successful geometry belongs to the desktop.
}

void VirtualResizeSession::request(const ConsoleWorkerWire::Resize &request)
{
    if (m_acceptingRequest || !inputAllowed() || !request.requestId || !request.generation || request.generation != m_control.generation
        || request.output != QStringLiteral("virtual-desktop") || !V::validRequest(request.pixels, request.scale)) {
        Q_EMIT result({request.requestId, request.generation, QStringLiteral("virtual resize requires current control, valid size and an idle worker")});
        return;
    }
    m_cancelError.clear();
    m_forwardFrames = false;
    m_pending = request;
    const QPointer<VirtualResizeSession> alive(this);
    m_acceptingRequest = true;
    const auto accepting = qScopeGuard([alive] { if (alive) alive->m_acceptingRequest = false; });
    // Release held input before discovery too: an exact no-op still gates input
    // while restarting capture, so its later client releases would be ignored.
    Q_EMIT mutationStarting();
    if (!alive || !m_pending || *m_pending != request || m_stopping || !m_control.active
        || m_control.generation != request.generation) return;
    if (!m_executor.resize(request.pixels, request.scale)) finishRequest(QStringLiteral("virtual resize executor unavailable"));
}

void VirtualResizeSession::completed(const VirtualResizeExecutor::Result &result)
{
    if (m_rollingBack) {
        m_rollingBack = false;
        m_plan.reset();
        m_captureTarget = result.observed;
        m_blockedError = result.error;
        if (!m_captureTarget && m_blockedError.isEmpty()) m_blockedError = QStringLiteral("virtual recovery geometry unknown");
        const QPointer<VirtualResizeSession> alive(this);
        if (m_captureTarget && !m_stopping) refreshCapture();
        if (!alive) return;
        finishRequest(m_cancelError.isEmpty() ? QStringLiteral("virtual resize cancelled") : m_cancelError);
        return;
    }
    if (!m_pending) return;
    m_plan = result.plan.valid() ? std::optional(result.plan) : std::nullopt;
    if (!m_cancelError.isEmpty() || !result.error.isEmpty()) {
        if (m_cancelError.isEmpty()) m_cancelError = result.error;
        if (result.mutated && m_plan) {
            recover();
        } else {
            m_plan.reset();
            m_forwardFrames = true; // No mode/scale mutation took place.
            finishRequest(m_cancelError);
        }
        return;
    }
    if (!result.observed) {
        cancel(QStringLiteral("virtual resize lacks verified readback"));
        return;
    }
    m_captureTarget = result.observed;
    refreshCapture();
}

void VirtualResizeSession::cancel(const QString &error)
{
    if (m_cancelError.isEmpty()) m_cancelError = error;
    m_executor.cancelBeforeApply();
    m_deadline.stop();
    if (m_executor.busy() || m_rollingBack) return;
    if (m_plan) recover();
    else {
        // Cancellation from the initial input-release callback precedes any
        // executor work. There is no mutation or capture change to recover.
        if (!m_captureTarget && m_blockedError.isEmpty()) m_forwardFrames = true;
        finishRequest(m_cancelError);
    }
}

void VirtualResizeSession::recover()
{
    m_captureTarget.reset();
    m_rollingBack = true;
    if (!m_plan || !m_executor.rollback(*m_plan)) {
        m_rollingBack = false;
        m_blockedError = QStringLiteral("virtual rollback could not start");
        finishRequest(m_cancelError);
    }
}

void VirtualResizeSession::refreshCapture()
{
    if (m_captureEpoch == std::numeric_limits<quint64>::max()) {
        m_blockedError = QStringLiteral("virtual capture epoch exhausted");
        m_captureTarget.reset();
        m_plan.reset();
        finishRequest(m_blockedError);
        return;
    }
    ++m_captureEpoch;
    m_capturePhase = CapturePhase::WaitingTeardown;
    m_deadline.start();
    Q_EMIT captureRefreshNeeded(m_captureEpoch);
}

void VirtualResizeSession::captureRestartReady(quint64 epoch)
{
    if (!epoch || epoch != m_captureEpoch || !m_captureTarget) return;
    // This acknowledgement is emitted only after producer teardown (nodeId=0),
    // before starting its replacement. A merely inactive/starting producer is
    // not proof that queued old activation/packet notifications are exhausted.
    if (m_capturePhase == CapturePhase::WaitingTeardown) m_capturePhase = CapturePhase::WaitingActive;
}

void VirtualResizeSession::captureRestartFailed(quint64 epoch)
{
    if (!epoch || epoch != m_captureEpoch || !m_captureTarget) return;
    m_capturePhase = CapturePhase::Invalidated;
    if (m_pending && m_plan) {
        cancel(QStringLiteral("could not restart virtual capture after resize"));
        return;
    }
    // Recovery has no further rollback/restart loop. Refuse frames and future
    // requests until the worker is replaced; late notifications cannot revive it.
    m_deadline.stop();
    m_captureTarget.reset();
    m_forwardFrames = false;
    m_blockedError = QStringLiteral("could not restart virtual capture after recovery");
    finishRequest(m_blockedError);
}

void VirtualResizeSession::captureStateChanged(quint64 epoch, bool active)
{
    if (!epoch || epoch != m_captureEpoch || !m_captureTarget) return;
    if (!active) {
        if (m_capturePhase == CapturePhase::Active) m_capturePhase = CapturePhase::Invalidated;
    } else if (m_capturePhase == CapturePhase::WaitingActive) {
        m_capturePhase = CapturePhase::Active;
        Q_EMIT keyframeNeeded();
    }
}

void VirtualResizeSession::captured(const ConsoleWorkerWire::Outputs &outputs, QSize frameMetadataPixels, QSize encodedPayloadPixels, bool keyframe, quint64 epoch)
{
    if (!keyframe || !epoch || epoch != m_captureEpoch || m_capturePhase != CapturePhase::Active || !m_captureTarget
        || m_executor.busy() || m_rollingBack || outputs.monitors.size() != 1) return;
    if (m_pending && !m_cancelError.isEmpty()) return; // Recovery cannot acknowledge the cancelled request.
    const auto &output = outputs.monitors.first();
    const auto &target = *m_captureTarget;
    // A fresh producer can initialize its encoder at old PipeWire dimensions
    // before frame-size metadata advances. Require independently validated H.264
    // keyframe dimensions as well as the epoch and metadata. Candidate payload
    // validation does not replace end-to-end client/visual acceptance.
    // Fractional logical geometry is rounded.
    if (output.name != target.name || !V::frameScaleMatches(output.scale, target.scale, output.geometry.size())
        || frameMetadataPixels != target.current.pixels || encodedPayloadPixels != target.current.pixels
        || output.geometry.topLeft() != QPoint(0, 0)
        || std::abs(output.geometry.width() - encodedPayloadPixels.width() / target.scale) > 1.0
        || std::abs(output.geometry.height() - encodedPayloadPixels.height() / target.scale) > 1.0) return;
    m_deadline.stop();
    m_confirmedScale = target.scale;
    m_captureTarget.reset();
    m_plan.reset();
    m_blockedError.clear();
    m_forwardFrames = true;
    // A recovery frame can reopen input, never acknowledge a cancelled request.
    if (m_pending) finishRequest({});
}

void VirtualResizeSession::finishRequest(const QString &error)
{
    const auto pending = std::exchange(m_pending, std::nullopt);
    const QPointer<VirtualResizeSession> alive(this);
    if (pending) Q_EMIT result({pending->requestId, pending->generation, error});
    if (!alive) return;
    if (m_stopping) {
        finishStop();
    } else if (m_captureTarget) {
        Q_EMIT keyframeNeeded();
    }
}

void VirtualResizeSession::stop()
{
    if (m_stopping) return;
    m_stopping = true;
    if (m_pending) cancel(QStringLiteral("virtual worker is stopping"));
    else finishStop();
}

void VirtualResizeSession::finishStop()
{
    if (m_stopEmitted || m_executor.busy() || m_pending || m_rollingBack) return;
    m_stopEmitted = true;
    m_deadline.stop();
    m_captureTarget.reset();
    Q_EMIT stopped(m_blockedError);
}
