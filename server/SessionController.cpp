// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "SessionController.h"

#include <algorithm>

#include <QAction>
#include <QCoreApplication>
#include <QDBusInterface>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>

#include <KGlobalAccel>
#include <KLocalizedString>

#include <Clipboard.h>
#include <Cursor.h>
#include <InputHandler.h>
#include <PortalSession.h>
#include <RdpConnection.h>
#include <Server.h>

#ifdef WITH_PLASMA_SESSION
#include <PlasmaScreencastV1Session.h>
#endif

#include "TakeoverDetector.h"
#include "VideoStream.h"

using namespace Qt::StringLiterals;

namespace
{
// How long the output list has to hold still before a hot-plug rebuild; see
// SessionController::rebuildMultiSessions(). Comfortably longer than the
// 750 ms settle PlasmaScreencastV1Session's own stream recovery uses, so the
// output churn on a DPMS wake is over before the layout is read.
constexpr int MultiRebuildSettleMs = 2000;
// How long the console takeover detector ignores cursor samples after a
// park or restore has moved outputs around: KWin removes and re-adds
// outputs after enabling them and warps the pointer meanwhile, and those
// samples arrive queued behind the call that caused them (hw batch v6,
// finding C).
constexpr int TakeoverSuspendMs = 3000;

QString layoutSummary(const QVector<KRdp::VideoMonitor> &monitors)
{
    QStringList parts;
    parts.reserve(monitors.size());
    for (const auto &monitor : monitors) {
        parts.push_back(QStringLiteral("%1,%2 %3x%4%5")
                            .arg(monitor.geometry.x())
                            .arg(monitor.geometry.y())
                            .arg(monitor.geometry.width())
                            .arg(monitor.geometry.height())
                            .arg(monitor.primary ? QStringLiteral(" primary") : QString()));
    }
    return parts.join(QStringLiteral("; "));
}
}

class SessionWrapper : public QObject
{
    Q_OBJECT
public:
    SessionWrapper(KRdp::RdpConnection *conn, KStatusNotifierItem *sni, DisplayWakeGuard *displayWakeGuard)
        : connection(conn)
        , m_displayWakeGuard(displayWakeGuard)
    {
        m_sni = sni;
        m_clock.start();

        connect(connection->videoStream(), &KRdp::VideoStream::enabledChanged, this, &SessionWrapper::onVideoStreamEnabledChanged, Qt::QueuedConnection);
        connect(connection->videoStream(), &KRdp::VideoStream::requestedFrameRateChanged, this, &SessionWrapper::onRequestedFrameRateChanged, Qt::QueuedConnection);
        // Emitted from the frame submission thread; the session must act on the main thread.
        connect(connection->videoStream(), &KRdp::VideoStream::keyFrameRequested, this, &SessionWrapper::onKeyFrameRequested, Qt::QueuedConnection);
        // VideoStream (and so this signal, whether from adaptive-quality steering
        // or a cap/adaptive toggle) runs on the main thread like SessionWrapper;
        // queued here defensively, matching the other VideoStream connections above.
        connect(connection->videoStream(), &KRdp::VideoStream::requestedQualityChanged, this, &SessionWrapper::onRequestedQualityChanged, Qt::QueuedConnection);

        connect(connection, &QObject::destroyed, this, &SessionWrapper::onConnectionDestroyed);
    }

    ~SessionWrapper() override
    {
        // Release BEFORE the sessions (and with them the virtual outputs) go
        // away, so KWin never has zero enabled outputs and windows migrate
        // back onto the physical monitors. The guard restores only if
        // applyReplace() touched (or may have touched) the physical outputs;
        // an extend session just drops its snapshot (it never wrote a state
        // file), leaving whatever the user changed on the console meanwhile
        // alone.
        if (outputGuard) {
            // A verified restore parks the virtual output itself (after the
            // outputs have settled); an unverified one leaves a retry
            // behind, which must not try to park an output that is about
            // to disappear with the sessions below.
            outputGuard->release();
            outputGuard->clearParkPlacements();
        }
        holdDisplayWake(false);
    }

    /**
     * Move the virtual outputs to their extend places, beside the physical
     * desktop, once the physical outputs are back. KWin remembers the last
     * arrangement it saw for a set of outputs and replays it when the same
     * set reappears (every virtual output shares one identity, whatever its
     * name), so leaving a virtual output over the physical origin would
     * record an overlapping layout and replay it on the next connect.
     *
     * Only after the restore: KWin pins a lone enabled output to (0,0), so a
     * move while the physical outputs are still off does not stick. A no-op
     * for an extend session, which already sits there.
     */
    void parkVirtualOutputs()
    {
        // Not gated on hasSnapshot(): a verified restore has just dropped it.
        if (!outputGuard || !outputGuard->hasParkPlacements()) {
            return;
        }
        if (!outputGuard->parkVirtualOutputs()) {
            qWarning() << "Could not park the virtual output beside the physical desktop; KWin's placement stands";
        }
        // The samples the move produces are queued behind this call.
        takeover.suspend(m_clock.elapsed() + TakeoverSuspendMs);
    }

    /**
     * Called whenever a virtual session starts, its stream becomes active or
     * it resolves its geometry. Once every session is both active and
     * resolved, apply the policy exactly once.
     *
     * "Active" is the encoded stream running (AbstractSession::streamActive()),
     * which is later than started(): KPipeWire reports a stream active only
     * once its produce thread is up, so the physical outputs are only touched
     * when frames can actually flow from the virtual output.
     */
    void maybeApplyVirtualPolicy()
    {
        if (policyApplied || !outputGuard || sessions.empty()) {
            return;
        }
        const bool allReady = std::all_of(sessions.cbegin(), sessions.cend(), [](const std::unique_ptr<KRdp::AbstractSession> &session) {
            return session->streamActive() && session->outputGeometryResolved();
        });
        if (!allReady) {
            return;
        }
        policyApplied = true;
        const bool attemptedReplace = virtualPolicy == SessionController::VirtualPolicy::Replace;
        if (attemptedReplace) {
            if (outputGuard->applyReplace(virtualPlacements, virtualPrimaryName)) {
                ownsPhysicalLayout = true;
            } else {
                qWarning() << "replace policy could not be applied; continuing as extend";
                virtualPolicy = SessionController::VirtualPolicy::Extend;
            }
        }
        if (!ownsPhysicalLayout && outputGuard->hasSnapshot()) {
            // KWin may have replayed a remembered arrangement for this output
            // set (physical outputs off, or the virtual output overlapping
            // one); put the physical outputs back and give the virtual output
            // its place beside them.
            outputGuard->reconcileExtend();
            parkVirtualOutputs();
        }
        if (attemptedReplace) {
            // Console takeover (Task 6c). Armed once every kscreen-doctor call
            // above has returned, not before: disabling an output makes KWin
            // warp the pointer onto the one that is left, and that sample is
            // already queued behind this slot - it must fall inside the arm
            // delay. Armed for the attempt, not only its success: a replace
            // that failed halfway may have switched a monitor off too, and
            // local motion is then the way to get it back before teardown.
            takeover.armed(m_clock.elapsed());
        }
        // Unconditionally: the guard may be held without an owner (a replace
        // that failed halfway, an extend whose drift could not be re-applied),
        // and the controller's "Restore my monitors" action follows held()
        // as much as ownership.
        Q_EMIT physicalLayoutOwnershipChanged();
    }

    /**
     * Console takeover (Task 6c): the controller has just given the physical
     * outputs back while this session runs on. From here on the wrapper no
     * longer owns the layout and runs as extend; the guard parked the
     * virtual output beside the physical desktop when the restore verified
     * (or will, when its retry does), so nothing is parked here.
     */
    void adoptExtendPolicy()
    {
        const bool owned = ownsPhysicalLayout;
        ownsPhysicalLayout = false;
        virtualPolicy = SessionController::VirtualPolicy::Extend;
        policyApplied = true;
        // Whatever the trigger was, there is nothing left to detect, and the
        // output churn a restore causes must never read as a second console
        // activity (hw batch v6, finding C).
        takeover.latch();
        if (owned) {
            Q_EMIT physicalLayoutOwnershipChanged();
        }
    }

    /**
     * Console takeover bookkeeping: every pointer motion the single virtual
     * session is about to inject, in the KWin-global logical coordinates it
     * will be injected at, so a cursor sample that lands elsewhere can be
     * told apart from our own. See KRdp::Takeover::Detector.
     */
    void noteInjectedMotion(KRdp::AbstractSession *session, const std::shared_ptr<QEvent> &event)
    {
        if (event->type() != QEvent::MouseMove || takeover.fired()) {
            return;
        }
        // Only what the session will really inject: it drops motion while its
        // stream is down or its virtual output is unplaced (see
        // PlasmaScreencastV1Session::sendEvent()), and a move that never
        // reached the compositor must not become the position the next
        // cursor sample is compared against.
        if (!session->streamActive() || !session->outputGeometryResolved()) {
            return;
        }
        const auto mouseEvent = std::static_pointer_cast<QMouseEvent>(event);
        takeover.injected(session->mapToGlobal(mouseEvent->position()).toPoint(), m_clock.elapsed());
    }

    /**
     * Install \a newSessions (already configured by the controller), replacing
     * whatever was there.
     *
     * \a newLayout is empty in every mode but `multi`: one session, surface
     * index 0, no explicit surface layout and no coordinate translation,
     * exactly as it was before per-monitor surfaces. In `multi` it holds one
     * entry per surface in KWin-global pixel coordinates, in the same order as
     * \a newSessions.
     *
     * Safe to call with sessions that are already installed: every connection
     * made here is tracked and undone first, so dropSession() can re-install
     * the survivors without duplicating their signals.
     */
    void setSessions(std::vector<std::unique_ptr<KRdp::AbstractSession>> &&newSessions, const SessionController::MonitorLayout &newLayout)
    {
        if (newSessions.empty() || !connection) {
            return;
        }

        const bool hadLayout = !layout.isEmpty();
        // Undo exactly what a previous call wired. Destroying the previous
        // sessions would drop most of it anyway, but a session that survives
        // into the new set (dropSession()) would otherwise be connected twice
        // and queue every frame twice.
        for (const auto &handle : std::as_const(m_sessionConnections)) {
            QObject::disconnect(handle);
        }
        m_sessionConnections.clear();

        sessions = std::move(newSessions);
        layout = newLayout;
        if (!(layout.scale > 0.0)) {
            layout.scale = 1.0;
        }

        auto *videoStream = connection->videoStream();
        // The stream needs the layout before the first frame arrives: a frame
        // whose size does not match its surface is dropped. Skipped entirely
        // while no layout is or was configured, so the other modes never touch
        // this code path.
        if (!layout.isEmpty() || hadLayout) {
            videoStream->setMonitorLayout(layout.monitors);
        }

        const bool multi = !layout.isEmpty();
        for (const auto &entry : sessions) {
            auto *session = entry.get();
            m_sessionConnections.append(connect(session, &KRdp::AbstractSession::frameReceived, videoStream, &KRdp::VideoStream::queueFrame));
            m_sessionConnections.append(connect(session, &KRdp::AbstractSession::cursorUpdate, this, &SessionWrapper::onCursorUpdate));
            if (multi) {
                // One monitor going away must not take the whole connection
                // with it; see dropSession().
                m_sessionConnections.append(connect(session, &KRdp::AbstractSession::error, this, [this, session]() {
                    onMonitorSessionError(session);
                }));
                // The layout was derived from logical geometry times a device
                // pixel ratio; only the started session knows the real capture
                // size. See correctSurfaceSize().
                m_sessionConnections.append(connect(session, &KRdp::AbstractSession::started, this, [this, session]() {
                    correctSurfaceSize(session);
                }));
            } else {
                m_sessionConnections.append(connect(session, &KRdp::AbstractSession::error, this, &SessionWrapper::sessionError));
            }
            m_sessionConnections.append(
                connect(session, &KRdp::AbstractSession::clipboardDataChanged, connection->clipboard(), &KRdp::Clipboard::setServerData));
        }

        // Input and clipboard are workspace-wide, so they go through a single
        // session whatever the mode. It is the only session in every mode but
        // multi, where fake input addresses the whole workspace anyway and
        // onInputEvent() picks one with a live stream per event.
        auto *clipboardSession = sessions.front().get();
        if (!multi && outputGuard) {
            // Virtual mode: the same hand-off as below, with the pointer
            // motion about to be injected noted for the console takeover.
            m_sessionConnections.append(
                connect(connection->inputHandler(), &KRdp::InputHandler::inputEvent, this, [this, clipboardSession](const std::shared_ptr<QEvent> &event) {
                    noteInjectedMotion(clipboardSession, event);
                    clipboardSession->sendEvent(event);
                }));
        } else if (!multi) {
            m_sessionConnections.append(
                connect(connection->inputHandler(), &KRdp::InputHandler::inputEvent, clipboardSession, &KRdp::AbstractSession::sendEvent));
        } else {
            m_sessionConnections.append(connect(connection->inputHandler(), &KRdp::InputHandler::inputEvent, this, &SessionWrapper::onInputEvent));
        }
        m_sessionConnections.append(connect(connection->clipboard(), &KRdp::Clipboard::clientDataChanged, clipboardSession, [clipboard = connection->clipboard(), clipboardSession]() {
            clipboardSession->setClipboardData(clipboard->getClipboard());
        }, Qt::QueuedConnection));

        // A rebuild hands brand-new sessions to a stream that is already
        // running, so they need the state the signals above would otherwise
        // have delivered before they existed. On a first build the stream is
        // not enabled yet and none of this runs.
        if (videoStream->enabled()) {
            for (const auto &session : sessions) {
                session->setVideoFrameRate(videoStream->requestedFrameRate());
                if (m_requestedQuality.has_value()) {
                    session->setVideoQuality(m_requestedQuality.value());
                }
                session->requestStreamingEnable(videoStream);
            }
            holdDisplayWake(true);
        }
    }

    /**
     * A monitor session reported an unrecoverable error.
     *
     * Deferred to the event loop because the error can arrive synchronously
     * from inside setSessions() - PlasmaScreencastV1Session::start() emits
     * error() when its output is already gone - and re-entering setSessions()
     * while it is still wiring would pull the vector out from under itself.
     * The session is held by a QPointer because a rebuild may destroy it
     * before the deferred call runs.
     */
    void onMonitorSessionError(KRdp::AbstractSession *failed)
    {
        QPointer<KRdp::AbstractSession> guard(failed);
        QMetaObject::invokeMethod(this, [this, guard]() {
            if (guard) {
                dropSession(guard.data());
            }
        }, Qt::QueuedConnection);
    }

    /**
     * Take \a failed out of the session set and re-install the survivors, so
     * the client keeps the monitors that still work.
     *
     * Only reached in multi mode. The connection is closed only when nothing
     * is left, which is what every other mode does for its single session.
     */
    void dropSession(KRdp::AbstractSession *failed)
    {
        const auto position = std::find_if(sessions.begin(), sessions.end(), [failed](const std::unique_ptr<KRdp::AbstractSession> &entry) {
            return entry.get() == failed;
        });
        if (position == sessions.end()) {
            // Already replaced by a rebuild; whatever went wrong is moot.
            return;
        }
        const auto failedIndex = std::distance(sessions.begin(), position);

        if (layout.isEmpty()) {
            // A rebuild moved this connection off multi mode in between, so
            // the single-session rule applies again.
            Q_EMIT sessionError();
            return;
        }

        const QString failedName = layout.names.value(failedIndex, QStringLiteral("unknown"));

        // Who survives, what their new surface indices are and which of them
        // has to be primary: all of it is KRdp::MultiLayout::dropMonitor(), so
        // the rule is one pure function the tests can state.
        qsizetype oldPrimary = -1;
        for (qsizetype i = 0; i < layout.monitors.size(); ++i) {
            if (layout.monitors.at(i).primary) {
                oldPrimary = i;
                break;
            }
        }
        const auto outcome = KRdp::MultiLayout::dropMonitor(qsizetype(sessions.size()), failedIndex, oldPrimary);

        std::vector<std::unique_ptr<KRdp::AbstractSession>> survivors;
        SessionController::MonitorLayout survivingLayout;
        survivingLayout.scale = layout.scale;
        survivors.reserve(outcome.survivors.size());
        for (const auto old : std::as_const(outcome.survivors)) {
            survivors.push_back(std::move(sessions[size_t(old)]));
            auto monitor = layout.monitors.at(old);
            // The promotion decision is the outcome's, so clear every flag and
            // set exactly the one it named.
            monitor.primary = false;
            survivingLayout.monitors.push_back(monitor);
            survivingLayout.names.push_back(layout.names.value(old));
        }
        if (outcome.primary >= 0 && outcome.primary < survivingLayout.monitors.size()) {
            survivingLayout.monitors[outcome.primary].primary = true;
        }

        if (survivors.empty()) {
            qWarning().noquote() << QStringLiteral("Monitor session %1 (%2) failed: the capture session reported an error; no monitors left, closing the connection")
                                        .arg(failedIndex)
                                        .arg(failedName);
            Q_EMIT sessionError();
            return;
        }

        qWarning().noquote() << QStringLiteral("Monitor session %1 (%2) failed: the capture session reported an error; continuing with %3 monitors")
                                    .arg(failedIndex)
                                    .arg(failedName)
                                    .arg(survivors.size());

        // Surface indices must stay 0..N-1 and match the new layout: a
        // survivor's new index is its position in the outcome's list.
        for (size_t i = 0; i < survivors.size(); ++i) {
            survivors[i]->setMonitorIndex(int(i));
        }

        setSessions(std::move(survivors), survivingLayout);
    }

    /**
     * Put the surface of a just-started session onto the size its frames
     * really have.
     *
     * The layout's sizes come from KRdp::MultiLayout::selectMultiLayout(),
     * which multiplies a screen's LOGICAL geometry by its device pixel ratio
     * and rounds. On a fractional scale that lands a pixel away from what the
     * compositor actually captures (1707 logical at 1.5 rounds to 2561, the
     * capture is 2560), and VideoStream then drops every frame of that monitor
     * for not matching its surface. Only the started session knows the real
     * size, so ask it and patch the entry.
     *
     * The origin is left alone: it is the monitor's place in the RDP desktop,
     * which the layout owns, and re-deriving it here would move the seam.
     * started() is emitted once per session, so this cannot loop.
     */
    void correctSurfaceSize(KRdp::AbstractSession *session)
    {
        if (layout.isEmpty() || !connection) {
            return;
        }

        const auto position = std::find_if(sessions.begin(), sessions.end(), [session](const std::unique_ptr<KRdp::AbstractSession> &entry) {
            return entry.get() == session;
        });
        if (position == sessions.end()) {
            return;
        }
        const auto index = qsizetype(std::distance(sessions.begin(), position));
        if (index >= layout.monitors.size()) {
            return;
        }

        const QSize captured = session->pixelSize();
        auto &geometry = layout.monitors[index].geometry;
        if (captured.isEmpty() || captured == geometry.size()) {
            return;
        }

        qWarning().noquote() << QStringLiteral("Monitor %1 (%2) captures %3x%4, not the %5x%6 the layout derived from its logical size and scale; correcting the surface")
                                    .arg(index)
                                    .arg(layout.names.value(index, QStringLiteral("unknown")))
                                    .arg(captured.width())
                                    .arg(captured.height())
                                    .arg(geometry.width())
                                    .arg(geometry.height());

        geometry.setSize(captured);
        // Re-applying the layout is what rebuilds the surfaces; the stream
        // ignores an unchanged one, so this only resets because it changed.
        connection->videoStream()->setMonitorLayout(layout.monitors);
    }

    void onCursorUpdate(const PipeWireCursor &cursor)
    {
        if (!connection) {
            return;
        }

        // Only the image and its hotspot reach the client: RDP moves the
        // pointer client-side, so there is no position here to translate
        // between the per-monitor sessions and RDP desktop space.
        KRdp::Cursor::CursorUpdate update;
        update.hotspot = cursor.hotspot;
        update.image = cursor.texture;
        connection->cursor()->update(update);

        // The position is where the pointer really is on the captured (here:
        // the virtual) output, in capture pixels; mapped into KWin-global
        // logical space like an injected move it is comparable with the last
        // one the server injected, and a sample far from that with no recent
        // injection can only be the mouse on the console (Task 6c).
        if (outputGuard && !takeover.fired() && sessions.size() == 1) {
            const QPoint global = sessions.front()->mapToGlobal(cursor.position).toPoint();
            if (takeover.observed(global, m_clock.elapsed())) {
                qInfo() << "Console activity detected; restoring the physical outputs (session continues in extend mode)";
                Q_EMIT consoleActivityDetected();
            }
        }
    }

    void onVideoStreamEnabledChanged()
    {
        if (connection->videoStream()->enabled()) {
            for (const auto &session : sessions) {
                session->requestStreamingEnable(connection->videoStream());
            }
            holdDisplayWake(true);
        } else {
            for (const auto &session : sessions) {
                session->requestStreamingDisable(connection->videoStream());
            }
            holdDisplayWake(false);
        }
    }

    // Keeps acquire/release balanced no matter how the wrapper ends.
    void holdDisplayWake(bool hold)
    {
        if (hold == m_holdsDisplayWake) {
            return;
        }
        m_holdsDisplayWake = hold;
        if (hold) {
            m_displayWakeGuard->acquire();
        } else {
            m_displayWakeGuard->release();
        }
    }

    void onRequestedFrameRateChanged()
    {
        for (const auto &session : sessions) {
            session->setVideoFrameRate(connection->videoStream()->requestedFrameRate());
        }
    }

    void onKeyFrameRequested(int monitorIndex)
    {
        if (monitorIndex < 0 || size_t(monitorIndex) >= sessions.size()) {
            // Queued from the frame submission thread, so the session set may
            // have been rebuilt in between. The rebuilt sessions open with an
            // IDR of their own, so there is nothing to ask for.
            return;
        }
        // A stale index that is still in range is not caught above and asks the
        // wrong monitor for a keyframe. Harmless: the cost is one extra IDR on
        // that monitor, and the surface that actually wanted one gets another
        // request with the next frame it sends.
        sessions[monitorIndex]->requestKeyFrame();
    }

    void onRequestedQualityChanged(quint8 quality)
    {
        // Remembered so a session created by a later rebuild starts at the
        // quality the stream already settled on instead of the encoder default.
        m_requestedQuality = quality;
        for (const auto &session : sessions) {
            session->setVideoQuality(quality);
        }
    }

    /**
     * The session multi-monitor input is injected through.
     *
     * Fake input addresses the whole workspace, so any session will do - but
     * only one whose capture stream is up will actually inject: a session
     * whose screencast closed (a DPMS wake re-adds every wl_output) silently
     * drops everything it is handed. Session 0 being the one recovering is
     * exactly the case that used to kill input for the whole connection, so
     * take the first one that is live, and fall back to the front when none
     * is, which keeps the previous behaviour for a moment with every stream
     * down.
     */
    KRdp::AbstractSession *inputSession() const
    {
        for (const auto &session : sessions) {
            if (session->streamActive()) {
                return session.get();
            }
        }
        return sessions.front().get();
    }

    /**
     * Multi-monitor input: one pointer position for the whole RDP desktop,
     * injected through a single session.
     */
    void onInputEvent(const std::shared_ptr<QEvent> &event)
    {
        if (sessions.empty()) {
            return;
        }
        auto *target = inputSession();

        // Only pointer motion carries a position that reaches the compositor;
        // buttons, wheel and keys are injected without one (see
        // PlasmaScreencastV1Session::injectNonMotionEvent()).
        if (event->type() == QEvent::MouseMove) {
            const auto mouseEvent = std::static_pointer_cast<QMouseEvent>(event);
            // RDP desktop space -> KWin-global pixels -> KWin-global logical,
            // which is what org_kde_kwin_fake_input's pointer_motion_absolute
            // takes. originOf() is the exact inverse of the translation
            // VideoStream::setMonitorLayout() applied to this same layout.
            const QPointF position = (mouseEvent->position() + QPointF(KRdp::SurfaceLayout::originOf(layout.monitors))) / layout.scale;
            auto translated = std::make_shared<QMouseEvent>(QEvent::MouseMove,
                                                            position,
                                                            position,
                                                            position,
                                                            mouseEvent->button(),
                                                            mouseEvent->buttons(),
                                                            mouseEvent->modifiers());
            target->sendGlobalEvent(translated);
            return;
        }

        target->sendGlobalEvent(event);
    }

    void onConnectionDestroyed()
    {
        Q_EMIT connectionDestroyed(this);
    }

    Q_SIGNAL void sessionError();
    Q_SIGNAL void connectionDestroyed(SessionWrapper *wrapper);
    /** Local pointer motion seen while this wrapper's replace policy holds the physical outputs (Task 6c). */
    Q_SIGNAL void consoleActivityDetected();
    /** ownsPhysicalLayout flipped; the controller's "Restore my monitors" action follows it. */
    Q_SIGNAL void physicalLayoutOwnershipChanged();

    // One entry in every mode but MonitorMode=multi, where there is one per
    // monitor and the index into this vector is the RDPGFX surface index.
    std::vector<std::unique_ptr<KRdp::AbstractSession>> sessions;
    // Empty unless multi-monitor streaming is in effect; see setSessions().
    SessionController::MonitorLayout layout;
    // MonitorMode=virtual bookkeeping; see SessionController::buildVirtualSessions().
    // outputGuard is set only for a wrapper built in virtual mode, which is
    // also how the controller tells such a wrapper apart.
    PhysicalOutputGuard *outputGuard = nullptr;
    SessionController::VirtualPolicy virtualPolicy = SessionController::VirtualPolicy::Replace;
    QVector<KRdp::OutputSnapshot::Placement> virtualPlacements; // intended KWin positions, one per session
    // The same beside the physical desktop is the guard's park placement;
    // see PhysicalOutputGuard::setParkPlacements().
    QString virtualPrimaryName;
    bool policyApplied = false;
    bool ownsPhysicalLayout = false; // this wrapper took the snapshot and must restore it
    // Console takeover (Task 6c); timestamps are m_clock.elapsed().
    KRdp::Takeover::Detector takeover;
    QElapsedTimer m_clock;
    QPointer<KRdp::RdpConnection> connection;
    KStatusNotifierItem *m_sni;
    DisplayWakeGuard *m_displayWakeGuard;
    bool m_holdsDisplayWake = false;
    std::optional<quint8> m_requestedQuality;
    // Everything setSessions() wired, so it can unwire exactly that much.
    QList<QMetaObject::Connection> m_sessionConnections;
};

SessionController::SessionController(KRdp::Server *server, SessionType sessionType)
    : m_server(server)
    , m_sessionType(sessionType)
{
    connect(m_server, &KRdp::Server::newConnectionCreated, this, &SessionController::onNewConnection);
    // Status notification item
    m_sni = new KStatusNotifierItem(u"krdpserver"_s, this);
    auto menu = new QMenu(u"quitMenu"_s);
    // Disable default quit button since it has confirmation dialog
    m_sni->setStandardActionsEnabled(false);
    m_sni->setTitle(i18n("RDP Server"));
    m_sni->setIconByName(u"preferences-system-network-remote"_s);
    m_sni->setStatus(KStatusNotifierItem::Passive);
    // Console takeover (Task 6c): give the physical monitors back by hand,
    // from the tray or - since the tray is on a monitor that is off - with a
    // global shortcut. One action serves both; it is enabled only while a
    // virtual session holds the physical layout (updateRestoreAction()),
    // and KGlobalAccel does not fire a disabled action.
    m_restoreAction = new QAction(i18n("Restore my monitors"), menu);
    m_restoreAction->setObjectName(u"restore-physical-outputs"_s);
    m_restoreAction->setIcon(QIcon::fromTheme(u"video-display"_s));
    m_restoreAction->setEnabled(false);
    m_restoreAction->setProperty("componentName", u"krdpserver"_s);
    m_restoreAction->setProperty("componentDisplayName", i18n("RDP Server"));
    connect(m_restoreAction, &QAction::triggered, this, &SessionController::releasePhysicalOutputs);
    menu->addAction(m_restoreAction);
    const QKeySequence restoreShortcut(Qt::META | Qt::CTRL | Qt::ALT | Qt::Key_R);
    KGlobalAccel::self()->setDefaultShortcut(m_restoreAction, {restoreShortcut});
    KGlobalAccel::self()->setShortcut(m_restoreAction, {restoreShortcut});
    // Queued: restored() is also emitted from inside a wrapper's destructor,
    // i.e. while m_wrappers is being edited, which updateRestoreAction()
    // must not walk. The refresh lands one event-loop turn later, and is
    // dropped if it was the controller's own teardown.
    connect(&m_outputGuard, &PhysicalOutputGuard::restored, this, &SessionController::updateRestoreAction, Qt::QueuedConnection);

    auto quitAction = new QAction(i18n("Quit"), menu);
    quitAction->setIcon(QIcon::fromTheme(QStringLiteral("application-exit")));
    connect(quitAction, &QAction::triggered, this, &SessionController::stopFromSNI);
    menu->addAction(quitAction);
    m_sni->setContextMenu(menu);

    // The wake makes KWin re-add every output, which closes the screencast; the
    // session's own closed-stream recovery re-creates it, so nothing to do here
    // beyond logging. Hook point if a forced refresh ever turns out to be needed.
    connect(&m_displayWakeGuard, &DisplayWakeGuard::displayWakeRequested, this, [](bool succeeded) {
        qDebug() << "Display wake request answered, succeeded:" << succeeded;
    });

    m_multiRebuildTimer.setSingleShot(true);
    m_multiRebuildTimer.setInterval(MultiRebuildSettleMs);
    connect(&m_multiRebuildTimer, &QTimer::timeout, this, [this]() {
        if (m_multiMonitorRequested) {
            applyMultiLayout(true);
        }
    });
}

SessionController::~SessionController() noexcept
{
    // Wrappers restore the physical outputs through m_outputGuard; tear them
    // down while the guard is still alive.
    m_wrappers.clear();
}

void SessionController::setMonitorIndex(const std::optional<int> &index)
{
    if (m_monitorIndex == index) {
        return;
    }

    m_monitorIndex = index;
    qInfo() << "Monitor target changed to"
            << (index.has_value() ? QStringLiteral("monitor:%1").arg(index.value()) : QStringLiteral("workspace"));
    refreshDisplayConfiguration();
}

void SessionController::setMultiMonitorEnabled(bool enabled)
{
    // Per-monitor streaming needs one capture stream per output, which only the
    // Plasma screencast session can open: the xdg-desktop-portal session hands
    // out the single stream the user picked in the portal dialog, so N sessions
    // would mean N portal prompts for whatever the user happened to choose.
    // Refuse it the same way too few monitors are refused - multi stays off and
    // the configured monitor index applies - and say so once rather than on
    // every config reload.
    if (enabled && m_sessionType != SessionType::Plasma) {
        if (!m_warnedPortalMulti) {
            m_warnedPortalMulti = true;
            qWarning() << "MonitorMode=multi requires the Plasma session (--plasma); using specific";
        }
        enabled = false;
    }

    const bool wasRequested = m_multiMonitorRequested;
    m_multiMonitorRequested = enabled;

    if (!enabled) {
        m_multiRebuildTimer.stop();
        if (!m_multiMonitor) {
            return;
        }
        m_multiMonitor = false;
        m_layout = {};
        m_streamIndices.clear();
        qInfo() << "MonitorMode=multi turned off, rebuilding single-session streams";
        rebuildSessions();
        return;
    }

    applyMultiLayout(false);
    if (!m_multiMonitor && !wasRequested) {
        // Only on the transition into multi, so a quality write that reloads
        // the config does not repeat this every time.
        qWarning() << "MonitorMode=multi needs two usable monitors; using specific";
    }
}

bool SessionController::multiMonitorEnabled() const
{
    return m_multiMonitor;
}

int SessionController::multiMonitorCount() const
{
    return m_multiMonitor ? int(m_layout.monitors.size()) : 0;
}

void SessionController::setVirtualMode(bool enabled)
{
    // Only the Plasma screencast session can ask KWin for a virtual output;
    // the portal session streams whatever the user picked in its dialog.
    if (enabled && m_sessionType != SessionType::Plasma) {
        if (!m_warnedPortalVirtual) {
            m_warnedPortalVirtual = true;
            qWarning() << "MonitorMode=virtual requires the Plasma session (--plasma); ignoring it";
        }
        enabled = false;
    }
    if (m_virtualMode == enabled) {
        return;
    }
    m_virtualMode = enabled;
    qInfo() << "MonitorMode=virtual" << (enabled ? "on" : "off") << "- applies to the next connection";
}

bool SessionController::virtualMode() const
{
    return m_virtualMode;
}

void SessionController::setVirtualPolicy(VirtualPolicy policy)
{
    m_virtualPolicy = policy;
}

void SessionController::setVirtualLayout(VirtualLayout layout)
{
    m_virtualLayout = layout;
}

void SessionController::setVirtualFallbackSize(const QSize &size)
{
    m_virtualFallbackSize = size;
}

SessionController::VirtualPolicy SessionController::virtualPolicy() const
{
    return m_virtualPolicy;
}

SessionController::VirtualLayout SessionController::virtualLayout() const
{
    return m_virtualLayout;
}

QString SessionController::policyName(VirtualPolicy policy)
{
    return policy == VirtualPolicy::Extend ? u"extend"_s : u"replace"_s;
}

QString SessionController::layoutName(VirtualLayout layout)
{
    return layout == VirtualLayout::Single ? u"single"_s : u"client"_s;
}

SessionController::VirtualPolicy SessionController::parseVirtualPolicy(const QString &text)
{
    return text.trimmed().compare(u"extend"_s, Qt::CaseInsensitive) == 0 ? VirtualPolicy::Extend : VirtualPolicy::Replace;
}

SessionController::VirtualLayout SessionController::parseVirtualLayout(const QString &text)
{
    return text.trimmed().compare(u"single"_s, Qt::CaseInsensitive) == 0 ? VirtualLayout::Single : VirtualLayout::Client;
}

std::optional<QSize> SessionController::parseSize(const QString &text)
{
    static const QRegularExpression rx(uR"(^\s*(\d+)\s*x\s*(\d+)\s*$)"_s);
    const auto match = rx.match(text);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    const QSize size(match.capturedView(1).toInt(), match.capturedView(2).toInt());
    return KRdp::ClientDisplay::usable(size) ? std::optional(size) : std::nullopt;
}

bool SessionController::physicalLayoutOwned() const
{
    return std::any_of(m_wrappers.cbegin(), m_wrappers.cend(), [](const std::unique_ptr<SessionWrapper> &wrapper) {
        return wrapper && wrapper->ownsPhysicalLayout;
    });
}

void SessionController::updateRestoreAction()
{
    if (!m_restoreAction) {
        return;
    }
    // held() is the superset: a replace that failed halfway (or an extend
    // whose drift could not be re-applied) leaves no owner but monitors that
    // may be off, and the console needs the action then most of all.
    m_restoreAction->setEnabled(physicalLayoutOwned() || m_outputGuard.held());
}

void SessionController::releasePhysicalOutputs()
{
    // Idempotent: the second trigger (the shortcut after local motion already
    // fired, say) finds nothing to give back.
    if (!physicalLayoutOwned() && !m_outputGuard.held()) {
        qDebug() << "Physical outputs are already the console's; nothing to release";
        updateRestoreAction();
        return;
    }
    // Restores, since the guard is held, waiting for the outputs to settle
    // and parking the virtual output beside them when they have; the state
    // file goes with a verified restore, and an unverified one schedules
    // the guard's own retry (which parks too when it succeeds). Either way
    // the wrappers move on to extend: nothing else will restore the outputs
    // before teardown, and a session that keeps streaming is the point
    // (Steve's decision, Task 6c).
    const bool restored = m_outputGuard.release();
    int continued = 0;
    for (const auto &wrapper : m_wrappers) {
        if (wrapper && wrapper->outputGuard) {
            wrapper->adoptExtendPolicy();
            ++continued;
        }
    }
    if (restored) {
        qInfo() << "Physical outputs released to the console;" << continued << "virtual session(s) continue as extend";
    } else {
        qWarning() << "Physical outputs could not be verifiably restored for the console (the guard retries);" << continued
                   << "virtual session(s) continue as extend regardless";
    }
    updateRestoreAction();
}

void SessionController::setVirtualMonitor(const KRdp::VirtualMonitor &virtualMonitor)
{
    m_virtualMonitor = virtualMonitor;
}

void SessionController::setQuality(const std::optional<int> &quality)
{
    if (m_quality == quality) {
        return;
    }

    m_quality = quality;
    if (!m_quality.has_value()) {
        return;
    }

    for (const auto &wrapper : m_wrappers) {
        if (!wrapper || wrapper->sessions.empty()) {
            continue;
        }
        // With adaptive quality on, let VideoStream own the session's actual
        // quality: setQualityCap() below computes and emits it (queued to the
        // session). Calling session->setVideoQuality() directly here as well
        // would desync the encoder from VideoStream's own d->quality
        // bookkeeping the next time the adaptive loop runs (it would believe
        // quality is still whatever it last computed, while the encoder is
        // actually running at this cap).
        if (!m_adaptiveQuality) {
            for (const auto &session : wrapper->sessions) {
                session->setVideoQuality(m_quality.value());
            }
        }
        if (wrapper->connection) {
            wrapper->connection->videoStream()->setQualityCap(quint8(m_quality.value()));
        }
    }

    // The port disambiguates the log: KConfig's --notify broadcasts by file
    // NAME, so a write to one instance's krdpserverrc reaches every other
    // krdpserver running, each of which then reloads its own config and logs
    // a line like this one. "active sessions: 0" from an idle instance is
    // otherwise indistinguishable from the streaming instance reporting none.
    qInfo() << "Applied runtime quality update:" << m_quality.value() << "active sessions:" << m_wrappers.size() << "port:" << m_server->port();
}

void SessionController::setAdaptiveQuality(bool enabled)
{
    if (m_adaptiveQuality == enabled) {
        return;
    }

    m_adaptiveQuality = enabled;
    for (const auto &wrapper : m_wrappers) {
        if (!wrapper || !wrapper->connection) {
            continue;
        }
        wrapper->connection->videoStream()->setAdaptiveQuality(m_adaptiveQuality);
    }

    qInfo() << "Applied runtime adaptive quality update:" << m_adaptiveQuality << "active sessions:" << m_wrappers.size() << "port:" << m_server->port();
}

void SessionController::setWakeDisplayOnConnect(bool enabled)
{
    m_displayWakeGuard.setEnabled(enabled);
}

void SessionController::refreshDisplayConfiguration()
{
    if (m_virtualMonitor.has_value()) {
        return;
    }
    // MonitorMode=virtual is not a reason to return here: virtual wrappers
    // are skipped one by one below (the physical outputs come and go by
    // design and they track their own screen), while a physical-mode wrapper
    // that predates a runtime switch to virtual still needs its refresh.

    if (m_multiMonitorRequested) {
        // Also the path back into multi mode after it was refused for want of
        // a second monitor: a screen plugged in later gets another chance here.
        rebuildMultiSessions();
        if (m_multiMonitor) {
            return;
        }
    }

    for (const auto &wrapper : m_wrappers) {
        // A wrapper built while virtual mode was on keeps its virtual output
        // (which has no stream index to retarget) until it disconnects.
        if (!wrapper || wrapper->sessions.empty() || wrapper->outputGuard) {
            continue;
        }

        auto &session = wrapper->sessions.front();
        session->setActiveStream(m_monitorIndex.value_or(-1));
        session->refreshDisplayConfiguration();
    }
}

void SessionController::rebuildMultiSessions()
{
    if (!m_multiMonitorRequested) {
        return;
    }

    // Restarted by every event in a burst, so the layout is only read once the
    // outputs have stopped moving.
    m_multiRebuildTimer.start();
}

void SessionController::applyMultiLayout(bool topologyChange)
{
    if (refreshMultiLayout() != LayoutUpdate::Changed) {
        return;
    }

    m_multiMonitor = true;
    rebuildSessions();
    qInfo().noquote() << (topologyChange ? QStringLiteral("Rebuilt %1 monitor sessions after topology change: %2")
                                         : QStringLiteral("MonitorMode=multi active with %1 monitors: %2"))
                             .arg(m_layout.monitors.size())
                             .arg(layoutSummary(m_layout.monitors));
}

SessionController::LayoutUpdate SessionController::refreshMultiLayout()
{
    // Getting into multi mode is only worth it with two monitors. Once it is
    // running, a topology change that leaves a single usable monitor keeps a
    // single surface rather than silently moving a live client onto the
    // other code path; only losing every monitor - which the output churn on
    // a DPMS wake does transiently - leaves the previous layout alone.
    const int minimum = m_multiMonitor ? 1 : KRdp::MultiLayout::MinMonitorCount;
    const auto result = computeMultiLayout(minimum);

    if (!result.dropped.isEmpty() && result.dropped != m_droppedScreens) {
        qWarning().noquote() << QStringLiteral("MonitorMode=multi cannot use %1 (empty geometry, past the %2 px encode limit, or past %3 monitors)")
                                    .arg(result.dropped.join(QStringLiteral(", ")))
                                    .arg(KRdp::MultiLayout::MaxEncodeDimension)
                                    .arg(KRdp::MultiLayout::MaxMonitorCount);
    }
    m_droppedScreens = result.dropped;

    if (result.layout.isEmpty()) {
        return LayoutUpdate::Unusable;
    }

    if (result.mixedScales && !m_warnedMixedScales) {
        m_warnedMixedScales = true;
        qWarning() << "MonitorMode=multi with monitors at different scales: each monitor's logical origin is multiplied by its own device pixel"
                   << "ratio, so the pixel rects may gap or overlap, and pointer positions use the primary's scale of" << result.layout.scale
                   << "- multi is only exact on a uniform scale";
    }

    // A wrapper that lost a monitor to dropSession() is running fewer sessions
    // than the layout has entries. Nothing about the screens changed, so the
    // Unchanged path would leave that monitor dark for the rest of the
    // connection; count the next topology event as a change instead, which is
    // the chance to bring the dropped monitor back.
    const bool wrapperShortOfMonitors = std::any_of(m_wrappers.cbegin(), m_wrappers.cend(), [this](const std::unique_ptr<SessionWrapper> &wrapper) {
        return wrapper && wrapper->connection && qsizetype(wrapper->sessions.size()) < m_layout.monitors.size();
    });

    if (m_multiMonitor && !wrapperShortOfMonitors && m_layout.monitors == result.layout.monitors && m_streamIndices == result.streamIndices
        && qFuzzyCompare(m_layout.scale, result.layout.scale)) {
        return LayoutUpdate::Unchanged;
    }

    m_layout = result.layout;
    m_streamIndices = result.streamIndices;
    return LayoutUpdate::Changed;
}

SessionController::MultiLayoutResult SessionController::computeMultiLayout(int minimumCount)
{
    MultiLayoutResult result;

    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return result;
    }

    const auto primary = primaryScreenIndex();

    // screenIndices.at(i) is the QGuiApplication::screens() index that infos[i]
    // was read from. Carrying it through is what keeps a surface's
    // setActiveStream() target correct without a second indexOf() lookup, which
    // could return -1 if the screen list moved in between - and -1 means
    // "capture the whole workspace", whose frames no per-monitor surface can
    // take. A null entry is skipped here so it can never contribute an index.
    QVector<int> screenIndices;
    QVector<KRdp::MultiLayout::ScreenInfo> infos;
    infos.reserve(screens.size());
    screenIndices.reserve(screens.size());
    for (qsizetype i = 0; i < screens.size(); ++i) {
        const auto *screen = screens.at(i);
        if (!screen) {
            continue;
        }
        screenIndices.push_back(int(i));
        infos.push_back(KRdp::MultiLayout::ScreenInfo{
            .name = screen->name(),
            .logicalGeometry = screen->geometry(),
            .devicePixelRatio = screen->devicePixelRatio(),
            .primary = primary.has_value() && *primary == int(i),
        });
    }

    QList<qsizetype> kept;
    result.layout.monitors = KRdp::MultiLayout::selectMultiLayout(infos, &result.dropped, &kept, minimumCount);
    if (result.layout.monitors.isEmpty()) {
        return result;
    }

    result.streamIndices.reserve(kept.size());
    result.layout.names.reserve(kept.size());
    for (const auto index : std::as_const(kept)) {
        result.streamIndices.push_back(screenIndices.at(index));
        result.layout.names.push_back(infos.at(index).name);
    }

    // The layout is in pixels while fake input takes logical coordinates, so
    // the input path needs the ratio between them; the primary's is the one
    // used. Every screen on this box is at scale 1.
    for (qsizetype i = 0; i < result.layout.monitors.size(); ++i) {
        if (result.layout.monitors.at(i).primary) {
            result.layout.scale = infos.at(kept.at(i)).devicePixelRatio;
            break;
        }
    }
    result.mixedScales = std::any_of(kept.cbegin(), kept.cend(), [&infos, scale = result.layout.scale](qsizetype index) {
        return !qFuzzyCompare(infos.at(index).devicePixelRatio, scale);
    });

    return result;
}

std::optional<int> SessionController::primaryScreenIndex()
{
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return std::nullopt;
    }

    // Try KWin's output config first (authoritative for KDE Plasma).
    const auto primaryName = kwinPrimaryOutputName();
    if (!primaryName.isEmpty()) {
        for (int i = 0; i < screens.size(); ++i) {
            if (screens.at(i)->name() == primaryName) {
                return i;
            }
        }
        qWarning() << "KWin primary output" << primaryName << "not found in Qt screen list";
    }
    // Fallback to Qt's primaryScreen().
    const auto primary = QGuiApplication::primaryScreen();
    const auto primaryIndex = screens.indexOf(primary);
    if (primaryIndex < 0) {
        return std::nullopt;
    }
    return int(primaryIndex);
}

/**
 * Read KWin's output config to find the connector name of the primary output.
 *
 * KDE Plasma stores monitor priorities in ~/.config/kwinoutputconfig.json.
 * The output with priority 0 in the active setup is the user's primary.
 * Qt's QGuiApplication::primaryScreen() doesn't reflect this in headless
 * service contexts, so we read the config file directly.
 *
 * Returns the connector name (e.g. "DP-1") or an empty string on failure.
 */
QString SessionController::kwinPrimaryOutputName()
{
    const QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/kwinoutputconfig.json");
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qInfo() << "Could not open KWin output config at" << configPath;
        return {};
    }

    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse KWin output config:" << parseError.errorString();
        return {};
    }

    if (!doc.isArray()) {
        qWarning() << "KWin output config is not a JSON array";
        return {};
    }

    const auto root = doc.array();

    // Find the "outputs" and "setups" sections.
    QJsonArray outputsArray;
    QJsonArray setupsArray;
    for (const auto &entry : root) {
        const auto obj = entry.toObject();
        const auto name = obj.value(u"name"_s).toString();
        if (name == u"outputs"_s) {
            outputsArray = obj.value(u"data"_s).toArray();
        } else if (name == u"setups"_s) {
            setupsArray = obj.value(u"data"_s).toArray();
        }
    }

    if (outputsArray.isEmpty() || setupsArray.isEmpty()) {
        qInfo() << "KWin output config missing outputs or setups section";
        return {};
    }

    // Build connector name list from the global outputs array.
    QStringList connectorNames;
    connectorNames.reserve(outputsArray.size());
    for (const auto &output : outputsArray) {
        connectorNames.append(output.toObject().value(u"connectorName"_s).toString());
    }

    // Get the set of currently connected Qt screen names.
    const auto screens = QGuiApplication::screens();
    QSet<QString> currentScreenNames;
    currentScreenNames.reserve(screens.size());
    for (const auto *screen : screens) {
        currentScreenNames.insert(screen->name());
    }

    // Find the setup whose enabled outputs match the current Qt screens.
    for (const auto &setupEntry : setupsArray) {
        const auto setupOutputs = setupEntry.toObject().value(u"outputs"_s).toArray();

        // Collect connector names for enabled outputs in this setup.
        QSet<QString> enabledNames;
        for (const auto &setupOutput : setupOutputs) {
            const auto outputObj = setupOutput.toObject();
            if (!outputObj.value(u"enabled"_s).toBool(true)) {
                continue;
            }
            const int outputIndex = outputObj.value(u"outputIndex"_s).toInt(-1);
            if (outputIndex >= 0 && outputIndex < connectorNames.size()) {
                enabledNames.insert(connectorNames.at(outputIndex));
            }
        }

        if (enabledNames != currentScreenNames) {
            continue;
        }

        // This setup matches. Find the output with priority 0.
        for (const auto &setupOutput : setupOutputs) {
            const auto outputObj = setupOutput.toObject();
            if (outputObj.value(u"priority"_s).toInt(-1) == 0) {
                const int outputIndex = outputObj.value(u"outputIndex"_s).toInt(-1);
                if (outputIndex >= 0 && outputIndex < connectorNames.size()) {
                    return connectorNames.at(outputIndex);
                }
            }
        }

        // Matched setup but no priority-0 entry found.
        qInfo() << "KWin setup matched but no priority-0 output found";
        return {};
    }

    qInfo() << "No KWin setup matches current screens:" << currentScreenNames;
    return {};
}

void SessionController::buildSessions(SessionWrapper *wrapper)
{
    std::vector<std::unique_ptr<KRdp::AbstractSession>> sessions;
    MonitorLayout layout;

    if (m_multiMonitor && !m_layout.isEmpty()) {
        layout = m_layout;
        sessions.reserve(m_streamIndices.size());
        for (qsizetype i = 0; i < m_streamIndices.size(); ++i) {
            auto session = makeSession();
            session->setActiveStream(m_streamIndices.at(i));
            // The RDPGFX surface this session feeds, not a monitor target:
            // see AbstractSession::setMonitorIndex().
            session->setMonitorIndex(int(i));
            sessions.push_back(std::move(session));
        }
    } else {
        auto session = makeSession();
        if (m_virtualMonitor) {
            session->setVirtualMonitor(*m_virtualMonitor);
        } else {
            session->setActiveStream(m_monitorIndex.value_or(-1));
        }
        sessions.push_back(std::move(session));
    }

    // See setQuality() for why the direct session call is skipped while
    // adaptive quality is on: setQualityCap() is the only path that should
    // ever set the session's quality in that mode.
    if (m_quality.has_value() && !m_adaptiveQuality) {
        for (const auto &session : sessions) {
            session->setVideoQuality(m_quality.value());
        }
    }

    wrapper->setSessions(std::move(sessions), layout);
}

void SessionController::buildVirtualSessions(SessionWrapper *wrapper)
{
    if (!wrapper || !wrapper->connection || !wrapper->sessions.empty()) {
        return;
    }
    const auto info = KRdp::ClientDisplay::sanitize(wrapper->connection->clientDisplayInfo(), m_virtualFallbackSize);
    // The snapshot taken now is the only one this session may replace on;
    // never a leftover from an earlier session (the guard clears it, and
    // canReplace is keyed on this call's result, not on hasSnapshot()).
    bool snapshotTaken = false;
    if (!m_outputGuard.available()) {
        qWarning() << "kscreen-doctor not found; MonitorMode=virtual runs as extend without layout control";
    } else if (!(snapshotTaken = m_outputGuard.snapshot())) {
        qWarning() << "Could not snapshot the physical outputs; MonitorMode=virtual runs as extend";
    }
    const bool canReplace = m_virtualPolicy == VirtualPolicy::Replace && snapshotTaken;

    wrapper->outputGuard = &m_outputGuard;
    wrapper->virtualPolicy = canReplace ? VirtualPolicy::Replace : VirtualPolicy::Extend;
    wrapper->policyApplied = false;
    wrapper->ownsPhysicalLayout = false;
    wrapper->virtualPlacements.clear();
    m_outputGuard.clearParkPlacements();

    if (m_virtualLayout == VirtualLayout::Client && !info.monitors.isEmpty()) {
        qInfo() << "Client advertises" << info.monitors.size() << "monitors; multi-output virtual layout lands in Task 7, using one output of" << info.desktopSize;
    }

    // Where the virtual output goes: over the physical desktop's origin when
    // it replaces the physical outputs, immediately to their right when it
    // extends them (also what KWin does by itself for a never-seen output,
    // but KWin replays whatever arrangement it last saw for this output set).
    // The extend place is also where a replace session's output is parked
    // once the physical outputs are back (PhysicalOutputGuard::restore(),
    // SessionWrapper::parkVirtualOutputs()). Known only with a snapshot:
    // without one there is no physical union to sit beside, and no restore
    // that could park anything.
    std::optional<QPoint> extendAnchor;
    if (snapshotTaken) {
        const QRect physical = KRdp::OutputSnapshot::enabledUnion(m_outputGuard.physicalOutputs());
        if (physical.isValid()) {
            extendAnchor = QPoint(physical.left() + physical.width(), physical.top());
        }
    }
    const QPoint anchor = canReplace ? QPoint(0, 0) : extendAnchor.value_or(QPoint(0, 0));

    std::vector<std::unique_ptr<KRdp::AbstractSession>> sessions;
    auto session = makeSession();
    const auto name = KRdp::ClientDisplay::virtualMonitorName(0, info.desktopSize);
    session->setVirtualMonitor(KRdp::VirtualMonitor{name, info.desktopSize, 1.0});
    // The RDPGFX surface this session feeds; see AbstractSession::setMonitorIndex().
    session->setMonitorIndex(0);
    wrapper->virtualPrimaryName = KRdp::OutputSnapshot::VirtualPrefix + name;
    wrapper->virtualPlacements.push_back({wrapper->virtualPrimaryName, anchor});
    if (extendAnchor) {
        m_outputGuard.setParkPlacements({{wrapper->virtualPrimaryName, *extendAnchor}}, wrapper->virtualPrimaryName);
    }

    // Not part of m_sessionConnections on purpose: they die with the session
    // object, and a rebuild never replaces a virtual session.
    connect(session.get(), &KRdp::AbstractSession::started, wrapper, &SessionWrapper::maybeApplyVirtualPolicy);
    connect(session.get(), &KRdp::AbstractSession::streamActiveChanged, wrapper, [wrapper](bool) {
        wrapper->maybeApplyVirtualPolicy();
    });
    connect(session.get(), &KRdp::AbstractSession::outputGeometryChanged, wrapper, [wrapper](const QRect &) {
        // A pointer move recorded for the takeover detector between a
        // kscreen-doctor call and this geometry update was mapped through
        // the output's old origin, so it is not where the pointer is; the
        // next move becomes the reference (Task 6c review, Minor 3).
        wrapper->takeover.forgetInjected();
        wrapper->maybeApplyVirtualPolicy();
    });
    connect(session.get(), &KRdp::AbstractSession::virtualOutputUnresolved, wrapper, [wrapper]() {
        // Also fires when an output KWin removed mid-session stays away for
        // 5 s; by then the policy has long been applied and stays so.
        if (wrapper->policyApplied) {
            qWarning() << "Virtual output lost mid-session and not back within 5 s: pointer input stays gated until it reappears";
            return;
        }
        // The output may still turn up later; once this has fired the policy
        // is never applied, so a late outputGeometryChanged() changes nothing.
        qWarning() << "Virtual output unresolved: pointer input stays gated and the" << (wrapper->virtualPolicy == VirtualPolicy::Replace ? "replace" : "extend")
                   << "policy is not applied";
        wrapper->policyApplied = true;
    });
    // See setQuality() for why the direct call is skipped with adaptive quality on.
    if (m_quality.has_value() && !m_adaptiveQuality) {
        session->setVideoQuality(m_quality.value());
    }
    sessions.push_back(std::move(session));

    qInfo() << "MonitorMode=virtual: one output" << info.desktopSize << "policy" << (canReplace ? "replace" : "extend") << "placed at" << anchor;
    wrapper->setSessions(std::move(sessions), MonitorLayout{});
}

void SessionController::rebuildSessions()
{
    for (const auto &wrapper : m_wrappers) {
        // Mode changes apply to the next connection for a virtual wrapper;
        // rebuilding it here would drop the virtual output under the client.
        // A wrapper with no sessions at all is a virtual one whose deferred
        // build (buildVirtualSessions(), once the client's display info is
        // in) has not run yet: building a physical session for it here would
        // make that build a no-op and hand the client a physical capture.
        if (!wrapper || !wrapper->connection || wrapper->outputGuard || wrapper->sessions.empty()) {
            continue;
        }
        buildSessions(wrapper.get());
    }
}

void SessionController::onNewConnection(KRdp::RdpConnection *newConnection)
{
    if (m_virtualMode) {
        // One virtual desktop, one snapshot of the physical outputs: a second
        // client would need a second output and would fight over the layout.
        // Any live connection counts, including one still in its capability
        // exchange whose sessions are not built yet - but not one that has
        // already closed and is only waiting to be destroyed, or a client
        // reconnecting right after a drop would be refused.
        const bool busy = std::any_of(m_wrappers.cbegin(), m_wrappers.cend(), [](const std::unique_ptr<SessionWrapper> &w) {
            return w && w->connection && w->connection->state() != KRdp::RdpConnection::State::Closed;
        });
        if (busy) {
            qWarning() << "MonitorMode=virtual serves one connection at a time; refusing a second client";
            // The FreeRDP peer does not exist yet - RdpConnection::initialize()
            // is queued behind the signal that got us here - so close() now
            // would be a silent no-op and the client would get a session with
            // no wrapper (a black desktop). Close it as soon as it is running.
            connect(newConnection, &KRdp::RdpConnection::stateChanged, newConnection, [newConnection](KRdp::RdpConnection::State state) {
                if (state == KRdp::RdpConnection::State::Running) {
                    qInfo() << "Closing the refused second client";
                    newConnection->close(KRdp::RdpConnection::CloseReason::None);
                }
            });
            return;
        }
    }

    auto wrapper = std::make_unique<SessionWrapper>(newConnection, m_sni, &m_displayWakeGuard);
    if (m_virtualMode) {
        // The client's desktop size is only known after the capabilities
        // exchange (session thread); build once it arrives.
        connect(newConnection, &KRdp::RdpConnection::clientDisplayInfoReceived, wrapper.get(), [this, wrapper = wrapper.get()]() {
            buildVirtualSessions(wrapper);
        }, Qt::QueuedConnection);
    } else {
        buildSessions(wrapper.get());
    }
    if (m_quality.has_value()) {
        newConnection->videoStream()->setQualityCap(quint8(m_quality.value()));
    }
    newConnection->videoStream()->setAdaptiveQuality(m_adaptiveQuality);

    connect(wrapper.get(), &SessionWrapper::connectionDestroyed, this, [this](SessionWrapper *wrapper) {
        m_wrappers.erase(std::remove_if(m_wrappers.begin(),
                                        m_wrappers.end(),
                                        [wrapper](std::unique_ptr<SessionWrapper> &entry) {
                                            return entry.get() == wrapper;
                                        }),
                         m_wrappers.end());
        updateRestoreAction();
    });
    connect(wrapper.get(), &SessionWrapper::physicalLayoutOwnershipChanged, this, &SessionController::updateRestoreAction);
    // Queued: the detector fires from a cursor-update slot, and the release
    // runs kscreen-doctor synchronously and reshuffles the screens under it.
    connect(wrapper.get(), &SessionWrapper::consoleActivityDetected, this, &SessionController::releasePhysicalOutputs, Qt::QueuedConnection);

    connect(wrapper.get(), &SessionWrapper::sessionError, this, [newConnection] {
        newConnection->close(KRdp::RdpConnection::CloseReason::None);
    });

    m_wrappers.push_back(std::move(wrapper));
}

void SessionController::stopFromSNI()
{
    // Uses dbus to stop the server service, like in the KCM
    // This kills all krdpserver instances, like a "panic button"
    QDBusInterface unit(u"org.freedesktop.systemd1"_s,
                        u"/org/freedesktop/systemd1/unit/app_2dorg_2ekde_2ekrdpserver_2eservice"_s,
                        u"org.freedesktop.systemd1.Unit"_s);

    unit.asyncCall(u"Stop"_s);
    QCoreApplication::quit();
}

std::unique_ptr<KRdp::AbstractSession> SessionController::makeSession()
{
#ifdef WITH_PLASMA_SESSION
    if (m_sessionType == SessionType::Plasma) {
        return std::make_unique<KRdp::PlasmaScreencastV1Session>();
    } else
#endif
    {
        return std::make_unique<KRdp::PortalSession>();
    }
}

#include "SessionController.moc"
