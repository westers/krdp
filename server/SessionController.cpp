// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "SessionController.h"
#include "AudioPriority.h"

#include <algorithm>
#include <limits>

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

#include "LayoutSessionDiff.h"
#include "RemoteMonitorGeometry.h"
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
// How long a multi-output virtual session waits after the policy has been
// applied, or after the last virtual output moved, before it reads the
// outputs' real places back and publishes them to the client; see
// SessionWrapper::adoptActualVirtualLayout(). The QScreen geometry updates
// a kscreen-doctor call causes arrive one output at a time, queued behind
// the (blocking) call itself; one timer restarted on each of them turns a
// burst into one layout reset instead of one per output.
constexpr int VirtualLayoutAdoptMs = 1000;
// How long a client that joined the KRDPCTL channel gets to send its first
// record before the session is built for the configured MonitorMode anyway
// (slice 2c design, §4 "Session build gate").
constexpr int ControlFirstRecordMs = 3000;
// The layout owner's heartbeat (design §3: `ping` every 5 s, 3 missed →
// release). A tick sends a ping and counts the previous one as missed if
// no pong has arrived since, so the release lands 15 s after the last pong.
constexpr int HeartbeatIntervalMs = 5000;
// A layout client's session build retries while an output of the layout is
// not a QScreen yet: every second, for as long as KWin's re-add of a
// physical output is known to take, twice over.
constexpr int LayoutBuildRetryMs = 1000;
constexpr int LayoutBuildRetries = 12;
// A `layout` record owed after a session build goes out once the
// ResetGraphics describing that build has (VideoStream::graphicsReset), so
// the client never reads a layout the wire has not described yet; a client
// with no video stream (the probe without --gfx) gets it after this long.
constexpr int LayoutRecordFallbackMs = 2000;

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
        // negotiatedCodecChanged is emitted from the peer thread (onCapsAdvertise); requestedChromaChanged
        // (S3) runs on the main thread like the other VideoStream signals above but is queued to match.
        connect(connection->videoStream(), &KRdp::VideoStream::negotiatedCodecChanged, this, &SessionWrapper::onNegotiatedCodecChanged, Qt::QueuedConnection);
        connect(connection->videoStream(), &KRdp::VideoStream::requestedChromaChanged, this, &SessionWrapper::onRequestedChromaChanged, Qt::QueuedConnection);

        connect(connection, &QObject::destroyed, this, &SessionWrapper::onConnectionDestroyed);
        // From the frame submission thread; see layoutRecordPending.
        connect(connection->videoStream(), &KRdp::VideoStream::graphicsReset, this, &SessionWrapper::onGraphicsReset, Qt::QueuedConnection);
        layoutRecordTimer.setSingleShot(true);
        layoutRecordTimer.setInterval(LayoutRecordFallbackMs);
        connect(&layoutRecordTimer, &QTimer::timeout, this, &SessionWrapper::onGraphicsReset);

        m_adoptTimer.setSingleShot(true);
        m_adoptTimer.setInterval(VirtualLayoutAdoptMs);
        connect(&m_adoptTimer, &QTimer::timeout, this, &SessionWrapper::adoptActualVirtualLayout);

        controlTimer.setSingleShot(true);
        controlTimer.setInterval(ControlFirstRecordMs);
        layoutRetryTimer.setSingleShot(true);
        layoutRetryTimer.setInterval(LayoutBuildRetryMs);
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
        if (outputGuard && outputGuard->layoutControlHeld()) {
            // The hold is a KRDPCTL layout owner's, not this wrapper's: an
            // extend wrapper (which never holds) closing after an owner's
            // apply took the snapshot must not restore that snapshot under
            // the owner (Task 3 re-review, Minor 1). Its virtual output
            // goes away with its sessions below; the executor's release
            // restores the physical outputs when the owner is done.
            qInfo() << "Virtual session ending while a KRDPCTL layout holds the physical outputs; leaving them to the layout owner";
        } else if (outputGuard) {
            // A verified restore parks the virtual output itself (after the
            // outputs have settled). An unverified one (the outputs did not
            // settle, or one is not connected) leaves a retry behind, which
            // must not try to park an output that is about to disappear with
            // the sessions below - but the physical outputs that ARE back
            // are enabled now, and the arrangement KWin records last for
            // this output set must not be "physicals on, virtual output
            // over their origin", or the next connect replays that overlap
            // (final review, Important 2). So park best effort first.
            if (!outputGuard->release() && outputGuard->held()) {
                outputGuard->parkIfPhysicalEnabled();
            }
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
        // Multi-output: whatever the guard calls above made of the requested
        // positions (or did not, without a snapshot), the RDP layout has to
        // describe where the outputs really are before pointer input may
        // be mapped through it.
        scheduleVirtualLayoutAdoption();
    }

    /**
     * Multi-output virtual session: read the outputs' real places back once
     * they have stopped moving. Restarted by every virtual output geometry
     * change after the policy has been applied (a console takeover parks
     * the outputs beside the physical desktop, a standby panel's re-add
     * replays a stored arrangement), so the client follows every move.
     */
    void scheduleVirtualLayoutAdoption()
    {
        if (layout.isEmpty() || !outputGuard) {
            return;
        }
        m_adoptTimer.start();
    }

    /**
     * KWin may refuse or adjust the requested positions of the virtual
     * outputs, and moves them again later (takeover park, output churn).
     * Whatever it did is what the RDP layout must describe, or pointer
     * input lands off by the difference: read every session's resolved
     * geometry back and, where it differs from the layout the stream was
     * reset with, re-publish. VideoStream::setMonitorLayout() resets the
     * surfaces with the new TS_MONITOR_DEF set, and onInputEvent() reads
     * layout.monitors on every event, so input follows.
     *
     * Only the positions are adopted: the sizes are what the sessions
     * requested (at scale 1, so logical equals capture pixels), and
     * correctSurfaceSize() already owns the one correction a size can need.
     */
    void adoptActualVirtualLayout()
    {
        if (layout.isEmpty() || !connection || !outputGuard) {
            return;
        }
        QVector<KRdp::VideoMonitor> actual = layout.monitors;
        bool allResolved = true;
        for (size_t i = 0; i < sessions.size() && qsizetype(i) < actual.size(); ++i) {
            const auto &session = sessions[i];
            const QRect geometry = session->outputGeometry();
            if (!session->outputGeometryResolved() || !geometry.isValid()) {
                allResolved = false;
                continue;
            }
            actual[qsizetype(i)].geometry.moveTopLeft(geometry.topLeft());
        }
        // From here on the layout describes the outputs as KWin has them,
        // whether or not that is what was asked for, and pointer input may
        // be mapped through it. An output that is away right now (KWin
        // re-adding it) keeps its last place and the next geometry change
        // re-schedules this.
        if (allResolved) {
            virtualLayoutAdopted = true;
            m_loggedGatedVirtualInput = false;
        }
        if (actual == layout.monitors) {
            return;
        }
        qWarning().noquote() << QStringLiteral("KWin placed the virtual outputs differently than the layout describes; adopting: %1").arg(layoutSummary(actual));
        layout.monitors = actual;
        connection->videoStream()->setMonitorLayout(actual);
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
            session->setVideoCodec(videoStream->codecForSessions());
            session->setChromaEnabled(m_chromaEnabled);
            session->setChromaPolicy(m_chromaPolicy);
            m_sessionConnections.append(connect(session, &KRdp::AbstractSession::frameReceived, videoStream, &KRdp::VideoStream::queueFrame));
            m_sessionConnections.append(connect(session, &KRdp::AbstractSession::chromaCapabilityChanged, this, &SessionWrapper::onChromaCapabilityChanged));
            // At most once every 30 s per wrapper: server/*.cpp has no access to the KRDP logging
            // category (plain qInfo()/qWarning() here), and one line per second would flood the journal.
            m_sessionConnections.append(connect(session, &KRdp::AbstractSession::chromaTimingReported, this, [this](const KRdp::ChromaTimingReport &r) {
                if (m_lastCostLog.isValid() && m_lastCostLog.elapsed() < 30000) {
                    return;
                }
                m_lastCostLog.start();
                qInfo().noquote() << QStringLiteral("AVC444 cost: frames %1 aux sent %2 skipped-motion %3 rest-refresh %4 rewrite-failures %5 split=%6 download avg %7 max %8 us split avg %9 max %10 us upload avg %11 max %12 us main queue->packet avg %13 aux %14 us")
                                          .arg(r.frames)
                                          .arg(r.auxSent)
                                          .arg(r.auxSkippedMotion)
                                          .arg(r.auxRestRefresh)
                                          .arg(r.rewriteFailures)
                                          .arg(r.splitVariant)
                                          .arg(r.downloadAvg)
                                          .arg(r.downloadMax)
                                          .arg(r.splitAvg)
                                          .arg(r.splitMax)
                                          .arg(r.uploadAvg)
                                          .arg(r.uploadMax)
                                          .arg(r.encodeMainAvg)
                                          .arg(r.encodeAuxAvg);
            }));
            // The session is passed along: a cursor sample's position is
            // local to the output that session captures (see onCursorUpdate()).
            m_sessionConnections.append(connect(session, &KRdp::AbstractSession::cursorUpdate, this, [this, session](const PipeWireCursor &cursor) {
                onCursorUpdate(session, cursor);
            }));
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
            // The wake goes out before the stream (and, for a virtual
            // session, its output) is requested: an output created while a
            // panel is in DPMS standby is created into KWin's remove-and-
            // re-add churn (OPT-041 finding F; the wake is asynchronous, so
            // this only orders the requests, it does not wait).
            holdDisplayWake(true);
            for (const auto &session : sessions) {
                session->setVideoFrameRate(videoStream->requestedFrameRate());
                if (m_requestedQuality.has_value()) {
                    session->setVideoQuality(m_requestedQuality.value());
                }
                session->requestStreamingEnable(videoStream);
            }
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
            if (old < layout.scales.size()) {
                survivingLayout.scales.push_back(layout.scales.at(old));
            }
            if (old < layout.logicalOrigins.size()) {
                survivingLayout.logicalOrigins.push_back(layout.logicalOrigins.at(old));
            }
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

        if (outputGuard) {
            // A multi-output virtual session: the failed session's virtual
            // output goes away with it, so the placements handed to the
            // guard (replace and park alike) must not name it any more, or
            // the next kscreen-doctor call fails on an output that is not
            // there. The primary follows the outcome's promotion.
            QVector<KRdp::OutputSnapshot::Placement> placements;
            QVector<KRdp::OutputSnapshot::Placement> parkPlacements;
            for (const auto old : std::as_const(outcome.survivors)) {
                if (old < virtualPlacements.size()) {
                    placements.push_back(virtualPlacements.at(old));
                }
                if (old < virtualParkPlacements.size()) {
                    parkPlacements.push_back(virtualParkPlacements.at(old));
                }
            }
            virtualPlacements = placements;
            virtualParkPlacements = parkPlacements;
            if (outcome.primary >= 0 && outcome.primary < survivingLayout.names.size()) {
                virtualPrimaryName = survivingLayout.names.at(outcome.primary);
            }
            if (outputGuard->hasParkPlacements()) {
                outputGuard->setParkPlacements(virtualParkPlacements, virtualPrimaryName);
            }
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
        if (layout.logicalOrigins.size() == layout.monitors.size() && layout.scales.size() == layout.monitors.size()) {
            QVector<KRdp::RemoteMonitorGeometry::Output> outputs;
            outputs.reserve(layout.monitors.size());
            for (qsizetype i = 0; i < layout.monitors.size(); ++i) {
                outputs.push_back({layout.logicalOrigins.at(i), layout.monitors.at(i).geometry.size(), layout.scales.at(i), layout.monitors.at(i).primary});
            }
            layout.monitors = KRdp::RemoteMonitorGeometry::projectToWire(outputs);
        }
        // Re-applying the layout is what rebuilds the surfaces; the stream
        // ignores an unchanged one, so this only resets because it changed.
        connection->videoStream()->setMonitorLayout(layout.monitors);
    }

    /**
     * A cursor sample from \a session, whose captured output the pointer is
     * on (KWin's screencast reports the cursor only to the stream of the
     * output it is over, so with several virtual outputs the samples come
     * from whichever session has it).
     */
    void onCursorUpdate(KRdp::AbstractSession *session, const PipeWireCursor &cursor)
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
        // injection can only be the mouse on the console (Task 6c). Mapped
        // through the emitting session's own place, which for a multi-output
        // session is that output's, not the layout's origin. Meaningless
        // while KWin has not placed (or has removed) that output.
        // A KRDPCTL layout client is watched the same way while its layout
        // keeps a real monitor dark (layoutTakeoverArmed; OPT-044 §4
        // "Takeover at the desk") - except while an apply is in flight,
        // when the arrangement warps the pointer and nothing may be judged.
        // Its samples are mapped through the wrapper's OWN layout table
        // (the same one onInputEvent() injects through), not the session's:
        // a layout session is an output stream, which reads its place once
        // at setup and never follows a move, whereas the table is refreshed
        // by every build - a kept session whose output KWin moved would
        // otherwise put every quiet sample a move's distance from the last
        // injection (Task 4 review, Important 1).
        if (outputGuard && !takeover.fired() && session->outputGeometryResolved()) {
            const QPoint global = session->mapToGlobal(cursor.position).toPoint();
            if (takeover.observed(global, m_clock.elapsed())) {
                qInfo() << "Console activity detected; restoring the physical outputs (session continues in extend mode)";
                Q_EMIT consoleActivityDetected();
            }
        } else if (layoutTakeoverArmed && !layoutApplyInFlight && !takeover.fired()) {
            const auto position = std::find_if(sessions.cbegin(), sessions.cend(), [session](const std::unique_ptr<KRdp::AbstractSession> &entry) {
                return entry.get() == session;
            });
            const qsizetype index = position == sessions.cend() ? -1 : std::distance(sessions.cbegin(), position);
            if (index < 0 || index >= layout.monitors.size()) {
                return;
            }
            const QPoint logicalOrigin = layout.logicalOrigins.size() == layout.monitors.size() ? layout.logicalOrigins.at(index) : layout.monitors.at(index).geometry.topLeft();
            const QRect logicalEntry(logicalOrigin, layout.monitors.at(index).geometry.size());
            const QPoint global = KRdp::LayoutSessions::captureToGlobal(logicalEntry, layout.scales.value(index, layout.scale), cursor.position).toPoint();
            if (takeover.observed(global, m_clock.elapsed())) {
                qInfo() << "Console activity detected; restoring the physical outputs (the KRDPCTL layout is released, its owner kept)";
                Q_EMIT consoleActivityDetected();
            }
        }
    }

    void onVideoStreamEnabledChanged()
    {
        if (connection->videoStream()->enabled()) {
            // Only with something to keep awake: the stream is enabled as
            // soon as drdynvc is ready, whatever the KRDPCTL gate decided,
            // and a soft `query` from a client with the standard add-ins
            // must not switch the desk's monitors on (OPT-044). A build that
            // lands later takes the wake in setSessions(); the release below
            // stays balanced through m_holdsDisplayWake. Before the stream
            // requests, for the same reason as in setSessions().
            if (!sessions.empty()) {
                holdDisplayWake(true);
            }
            for (const auto &session : sessions) {
                session->requestStreamingEnable(connection->videoStream());
            }
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

    void onNegotiatedCodecChanged(KRdp::VideoCodec codec)
    {
        for (const auto &session : sessions) {
            session->setVideoCodec(codec);
        }
    }

    void onRequestedChromaChanged(bool enabled)
    {
        m_chromaEnabled = enabled;
        for (const auto &session : sessions) {
            session->setChromaEnabled(enabled);
        }
    }

    void onChromaCapabilityChanged(bool capable)
    {
        // Every session of a connection runs the same encoder mode; the last report wins. The
        // adaptive rung must not "shed" a chroma stream that is not there (S3 ANDs this in).
        connection->videoStream()->setChromaCapable(capable);
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
            if (outputGuard && !virtualLayoutAdopted) {
                // Multi-output virtual session: until adoptActualVirtualLayout()
                // has read KWin's placement back, the layout is only where the
                // outputs are meant to go - for a replace, over the physical
                // origin, where the physical primary still is until the
                // policy applies. A move mapped through it would land there.
                // Same gate as PlasmaScreencastV1Session::sendEvent() keeps
                // for one unplaced virtual output.
                if (!m_loggedGatedVirtualInput) {
                    m_loggedGatedVirtualInput = true;
                    qInfo() << "Dropping pointer motion until the virtual outputs' placement has been read back";
                }
                return;
            }
            const auto mouseEvent = std::static_pointer_cast<QMouseEvent>(event);
            // RDP desktop space -> wire pixel atlas -> KWin-global logical,
            // which is what org_kde_kwin_fake_input's pointer_motion_absolute
            // takes. originOf() is the exact inverse of the translation
            // VideoStream::setMonitorLayout() applied to this same layout.
            const QPointF position = toGlobalLogical(mouseEvent->position() + QPointF(KRdp::SurfaceLayout::originOf(layout.monitors)));
            auto translated = std::make_shared<QMouseEvent>(QEvent::MouseMove,
                                                            position,
                                                            position,
                                                            position,
                                                            mouseEvent->button(),
                                                            mouseEvent->buttons(),
                                                            mouseEvent->modifiers());
            // Console takeover bookkeeping (Task 6c), the multi-output
            // counterpart of noteInjectedMotion(): only a move that will
            // really be injected (sendGlobalEvent() drops everything while
            // the target's stream is down) may become the reference the
            // next cursor sample is compared against.
            if ((outputGuard || layoutTakeoverArmed) && !takeover.fired() && target->streamActive()) {
                takeover.injected(position.toPoint(), m_clock.elapsed());
            }
            target->sendGlobalEvent(translated);
            return;
        }

        target->sendGlobalEvent(event);
    }

    /**
     * Wire-atlas pixel position to KWin-global logical. Uniform scale:
     * divide. KRDPCTL per-monitor scale: select the containing surface (or
     * nearest in a gap), then map its local pixels from its own logical
     * origin. The wire origin is not a KWin position at mixed scales.
     */
    QPointF toGlobalLogical(const QPointF &pixel) const
    {
        if (layout.scales.size() != layout.monitors.size() || layout.monitors.isEmpty()) {
            return pixel / layout.scale;
        }
        QVector<KRdp::RemoteMonitorGeometry::Output> outputs;
        outputs.reserve(layout.monitors.size());
        for (qsizetype i = 0; i < layout.monitors.size(); ++i) {
            const QPoint origin = layout.logicalOrigins.size() == layout.monitors.size() ? layout.logicalOrigins.at(i) : layout.monitors.at(i).geometry.topLeft();
            outputs.push_back({origin, layout.monitors.at(i).geometry.size(), layout.scales.at(i), layout.monitors.at(i).primary});
        }
        return KRdp::RemoteMonitorGeometry::wireToLogical(pixel, layout.monitors, outputs);
    }

    /**
     * The surfaces for the current layout have gone out (or the fallback
     * fired): the owed `layout` record may follow. See layoutRecordPending.
     */
    void onGraphicsReset()
    {
        if (!layoutRecordPending) {
            return;
        }
        layoutRecordPending = false;
        layoutRecordTimer.stop();
        Q_EMIT layoutRecordDue(this, layoutRecordIsTakeover);
    }

    /**
     * Owe a `layout` (or, with \a takeover, a `takeover`) record to this
     * client, to be sent after the next ResetGraphics (or the fallback). The
     * latest kind owed wins: a `layout` scheduled after a `takeover` that
     * has not gone out yet replaces it (the record's content is read at
     * send time either way).
     */
    void scheduleLayoutRecord(bool takeover)
    {
        layoutRecordPending = true;
        layoutRecordIsTakeover = takeover;
        layoutRecordTimer.start();
    }

    /** Nothing is owed any more (the record went out another way). */
    void cancelLayoutRecord()
    {
        layoutRecordPending = false;
        layoutRecordIsTakeover = false;
        layoutRecordTimer.stop();
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
    /** The `layout` (or `takeover`) record scheduleLayoutRecord() owed may go out now (KRDPCTL, OPT-044). */
    Q_SIGNAL void layoutRecordDue(SessionWrapper *wrapper, bool takeover);

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
    // The same beside the physical desktop: what the guard was handed as its
    // park placements (PhysicalOutputGuard::setParkPlacements()), kept so a
    // dropped session can be taken out of them. Empty without a snapshot.
    QVector<KRdp::OutputSnapshot::Placement> virtualParkPlacements;
    QString virtualPrimaryName;
    bool policyApplied = false;
    bool ownsPhysicalLayout = false; // this wrapper took the snapshot and must restore it
    // Multi-output (Phase B) only: a virtual output never appeared and the
    // wrapper was rebuilt as one output at the desktop size; never twice.
    bool forceSingleVirtual = false;
    // Multi-output only: adoptActualVirtualLayout() has read the outputs'
    // places back at least once, so layout.monitors is where they are and
    // pointer input may be mapped through it (see onInputEvent()).
    bool virtualLayoutAdopted = false;
    // Console takeover (Task 6c); timestamps are m_clock.elapsed().
    KRdp::Takeover::Detector takeover;
    /**
     * KRDPCTL session build gate (OPT-044); see
     * SessionController::onClientDisplayInfo(). Undecided until the
     * capabilities exchange is in; Waiting while a channel client's first
     * record is awaited (controlTimer running); QueryOnly never builds;
     * Built once the configured build ran, for whatever reason.
     */
    enum class ControlGate {
        Undecided,
        Waiting,
        QueryOnly,
        Built,
    };
    ControlGate controlGate = ControlGate::Undecided;
    QTimer controlTimer;
    /** This connection's id on the KRDPCTL channel (`layout.owner`, the executor's `owner` fields, logs). */
    QString controlId;
    /** Sessions were built from the host layout (buildLayoutSessions()); kept out of every configured-mode rebuild. */
    bool layoutClient = false;
    /** See SessionController::buildLayoutSessions(): the rebuild for outputs that were not screens yet. */
    QTimer layoutRetryTimer;
    int layoutRetriesLeft = 0;
    /**
     * A `layout` record is owed and goes out with the next ResetGraphics
     * (VideoStream::setMonitorLayout() only arms the reset; the wire sees it
     * with the next frame), or after LayoutRecordFallbackMs for a client
     * that has no video stream. Set by scheduleLayoutRecord().
     */
    bool layoutRecordPending = false;
    bool layoutRecordIsTakeover = false;
    QTimer layoutRecordTimer;
    /**
     * Desk takeover for a KRDPCTL layout client (OPT-044): the console
     * takeover detector is armed while the layout keeps a real monitor
     * dark, exactly as `outputGuard` arms it for a configured replace
     * session; see SessionController::armLayoutTakeover().
     */
    bool layoutTakeoverArmed = false;
    /**
     * An apply is being executed (any owner's): the arrangement it makes
     * warps the pointer and the samples arrive queued behind it, so none is
     * judged until the build that follows clears this (armLayoutTakeover()).
     */
    bool layoutApplyInFlight = false;
    QElapsedTimer m_clock;
    QPointer<KRdp::RdpConnection> connection;
    KStatusNotifierItem *m_sni;
    DisplayWakeGuard *m_displayWakeGuard;
    bool m_holdsDisplayWake = false;
    std::optional<quint8> m_requestedQuality;
    // Remembered so a session created by a later rebuild starts with the connection's current
    // chroma setting instead of the AbstractSession default (true).
    bool m_chromaEnabled = true;
    // The connection's current AVC444 aux-stream timing policy (OPT-045b): the controller's
    // configured default at connect time, or a client's KRDPCTL `chroma` override once one arrives.
    // Remembered the same way m_chromaEnabled is, so a rebuilt session starts at the right policy
    // instead of ChromaPolicy's struct default.
    KRdp::ChromaPolicy m_chromaPolicy;
    // Throttles the "AVC444 cost" journal line to at most once per wrapper per 30 s.
    QElapsedTimer m_lastCostLog;
    // Everything setSessions() wired, so it can unwire exactly that much.
    QList<QMetaObject::Connection> m_sessionConnections;
    // See scheduleVirtualLayoutAdoption().
    QTimer m_adoptTimer;
    bool m_loggedGatedVirtualInput = false;
};

SessionController::SessionController(KRdp::Server *server, SessionType sessionType)
    : m_server(server)
    , m_sessionType(sessionType)
    , m_layoutExecutor(
          &m_outputGuard,
          // Only the Plasma screencast session can ask KWin for a virtual
          // output; a portal creator would report itself resolved with no
          // output behind it (review Minor 10). No factory at all for the
          // portal, so the executor refuses `unsupported` before it holds
          // or waits for anything (re-review Minor 3).
          sessionType == SessionType::Plasma ? HostLayoutExecutor::SessionFactory([this]() {
              return makeSession();
          })
                                             : HostLayoutExecutor::SessionFactory(),
          [this]() {
              m_displayWakeGuard.wakeNow();
          })
{
    connect(m_server, &KRdp::Server::newConnectionCreated, this, &SessionController::onNewConnection);
    connect(&m_layoutExecutor, &HostLayoutExecutor::finished, this, &SessionController::onLayoutApplied);
    // Whenever the executor is about to change the outputs - the first
    // arrangement, a re-assert after a removal, the re-assert a restated
    // layout gets while unverified - every layout client's detector sits it
    // out until the build that follows (armLayoutTakeover() clears the
    // flag), whatever onControlApply() decided from the plan's action count:
    // a 0-action recovery apply warps the pointer like any other, and the
    // warp samples must not read as a desk takeover of the very apply that
    // puts the layout right. Synchronous on purpose: the guard blocks for
    // the change, and the samples arrive queued behind it.
    connect(&m_layoutExecutor, &HostLayoutExecutor::arrangementStarting, this, [this]() {
        for (const auto &w : m_wrappers) {
            if (w && w->layoutClient) {
                w->layoutApplyInFlight = true;
            }
        }
    });
    m_heartbeatTimer.setInterval(HeartbeatIntervalMs);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &SessionController::onHeartbeatTick);
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
    // down while the guard is still alive. The layout's virtual outputs go
    // after the wrappers that stream them, and their restore before the
    // guard is gone (the executor's destructor would do the same, but the
    // order is the point, so it is spelled out).
    m_wrappers.clear();
    m_layoutExecutor.releaseAll();
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
    switch (layout) {
    case VirtualLayout::Single:
        return u"single"_s;
    case VirtualLayout::Physical:
        return u"physical"_s;
    case VirtualLayout::Client:
        break;
    }
    return u"client"_s;
}

SessionController::VirtualPolicy SessionController::parseVirtualPolicy(const QString &text)
{
    return text.trimmed().compare(u"extend"_s, Qt::CaseInsensitive) == 0 ? VirtualPolicy::Extend : VirtualPolicy::Replace;
}

SessionController::VirtualLayout SessionController::parseVirtualLayout(const QString &text)
{
    const auto trimmed = text.trimmed();
    if (trimmed.compare(u"single"_s, Qt::CaseInsensitive) == 0) {
        return VirtualLayout::Single;
    }
    if (trimmed.compare(u"physical"_s, Qt::CaseInsensitive) == 0) {
        return VirtualLayout::Physical;
    }
    return VirtualLayout::Client;
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
    if (m_layoutExecutor.controlling()) {
        // The layout is a KRDPCTL client's: give the desk back through the
        // executor (physical outputs restored - the guard's snapshot, held
        // since the first apply - stand-ins removed, extras parked and
        // removed), keep the owner (design §4: a later `apply` re-runs the
        // planner from the restored state) and tell every layout client
        // with `takeover`, owed like a `layout` (after the ResetGraphics
        // that re-describes its sessions over the real outputs). The
        // trigger is the same whether the real mouse moved (the wrapper's
        // detector, armLayoutTakeover()), the tray action or the shortcut.
        if (m_layoutExecutor.busy()) {
            qInfo() << "Console takeover during a KRDPCTL apply: the apply is abandoned";
        }
        qInfo() << "Console takeover: releasing the KRDPCTL layout (owner kept)";
        m_layoutExecutor.releaseAll();
        const auto restored = m_layoutExecutor.current();
        for (const auto &wrapper : m_wrappers) {
            if (!wrapper || !wrapper->connection || !wrapper->layoutClient || wrapper->connection->state() == KRdp::RdpConnection::State::Closed) {
                continue;
            }
            const bool reset = buildLayoutSessions(wrapper.get(), restored);
            sendLayout(wrapper.get(), reset, true);
        }
        updateRestoreAction();
        return;
    }
    // A configured virtual wrapper's guard hold and layout control's are
    // exclusive (beginLayoutControl() refuses over a replace, and a closing
    // extend wrapper leaves layout control's hold alone), and controlling()
    // is true whenever layout control holds, so from here on the hold, if
    // any, is a configured wrapper's own.
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
    if (!restored && m_outputGuard.held()) {
        // Same reasoning as the wrapper's teardown: whatever physical output
        // is back is enabled, and the session runs on with its virtual
        // output; do not leave it over DP-1 (the retry re-parks if it
        // verifies later, harmlessly twice).
        m_outputGuard.parkIfPhysicalEnabled();
    }
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

void SessionController::setAudioPriorityDefault(bool enabled)
{
    m_audioPriorityDefault = enabled;
    for (const auto &wrapper : m_wrappers) {
        if (wrapper && wrapper->connection) {
            wrapper->connection->setAudioPriorityDefault(enabled);
        }
    }
}

void SessionController::setCodecPreference(KRdp::CodecPreference preference)
{
    if (m_codecPreference == preference) {
        return;
    }
    m_codecPreference = preference;
    // Existing connections negotiated already; a re-negotiation would need a GFX reset and a
    // restart of every session for a setting that is not expected to move while streaming.
    qInfo() << "Codec preference" << KRdp::VideoCodecSupport::preferenceName(preference) << "- applies to the next connection; active sessions:" << m_wrappers.size() << "port:" << m_server->port();
}

KRdp::CodecPreference SessionController::codecPreference() const
{
    return m_codecPreference;
}

void SessionController::setChromaPolicyDefaults(const KRdp::ChromaPolicy &policy)
{
    if (m_chromaPolicyDefault == policy) {
        return;
    }
    m_chromaPolicyDefault = policy;
    // Existing connections keep the policy they started with (or a client's own override): only a
    // fresh connection is seeded from this default, in onNewConnection().
    qInfo() << "AVC444 chroma policy defaults: motionGap" << policy.motionGapMs << "rest" << policy.restMs << "maxGap" << policy.maxGapMs << "- applies to the next connection; active sessions:"
            << m_wrappers.size() << "port:" << m_server->port();
}

KRdp::ChromaPolicy SessionController::chromaPolicyDefaults() const
{
    return m_chromaPolicyDefault;
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
        // (which has no stream index to retarget) until it disconnects; a
        // layout client's sessions follow the KRDPCTL layout, not the
        // configured monitor.
        if (!wrapper || wrapper->sessions.empty() || wrapper->outputGuard || wrapper->layoutClient) {
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
        return wrapper && wrapper->connection && !wrapper->layoutClient && !wrapper->outputGuard && !wrapper->sessions.empty()
            && qsizetype(wrapper->sessions.size()) < m_layout.monitors.size();
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
        // A `Virtual-*` output belongs to another krdpserver instance's
        // virtual session (OPT-041); the live multi service must never rebuild
        // its surfaces onto one. Skipping it here (index bookkeeping is
        // unchanged: screenIndices only records the screens actually kept)
        // keeps multi mode to the physical monitors.
        if (!screen || KRdp::OutputSnapshot::isVirtual(screen->name())) {
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
    auto info = KRdp::ClientDisplay::sanitize(wrapper->connection->clientDisplayInfo(), m_virtualFallbackSize);
    // The snapshot taken now is the only one this session may replace on;
    // never a leftover from an earlier session (the guard clears it, and
    // canReplace is keyed on this call's result, not on hasSnapshot()).
    bool snapshotTaken = false;
    if (!m_outputGuard.available()) {
        qWarning() << "kscreen-doctor not found; MonitorMode=virtual runs as extend without layout control";
    } else if (!(snapshotTaken = m_outputGuard.snapshot())) {
        qWarning() << "Could not snapshot the physical outputs; MonitorMode=virtual runs as extend";
    }
    if (m_virtualLayout == VirtualLayout::Physical) {
        // Mirror the physical layout instead of the client's own (OPT-041
        // S5); needs the snapshot just taken above, not a stale one, for the
        // same reason canReplace is keyed on snapshotTaken rather than
        // hasSnapshot().
        if (!snapshotTaken) {
            qWarning() << "VirtualMonitorLayout=physical needs the physical-output snapshot; using the client's layout";
        } else {
            const auto clientMonitorCount = info.monitors.size();
            const auto mirrored = KRdp::OutputSnapshot::toClientDisplayInfo(m_outputGuard.physicalOutputs());
            info = KRdp::ClientDisplay::sanitize(mirrored, m_virtualFallbackSize);
            if (info != mirrored) {
                qWarning() << "MonitorMode=virtual: the physical layout needed sanitizing - would-be" << mirrored.desktopSize << "monitors"
                           << mirrored.monitors.size() << "-> using" << info.desktopSize << "monitors" << info.monitors.size();
            }
            qInfo() << "MonitorMode=virtual: mirroring the physical layout" << info.desktopSize << "monitors" << info.monitors.size() << "(client advertised"
                    << clientMonitorCount << ")";
        }
    }
    const bool canReplace = m_virtualPolicy == VirtualPolicy::Replace && snapshotTaken;

    wrapper->outputGuard = &m_outputGuard;
    wrapper->virtualPolicy = canReplace ? VirtualPolicy::Replace : VirtualPolicy::Extend;
    wrapper->policyApplied = false;
    wrapper->ownsPhysicalLayout = false;
    wrapper->virtualLayoutAdopted = false;
    wrapper->virtualPlacements.clear();
    wrapper->virtualParkPlacements.clear();
    wrapper->virtualPrimaryName.clear();
    m_outputGuard.clearParkPlacements();

    // Where the virtual outputs go: over the physical desktop's origin when
    // they replace the physical outputs, immediately to their right when
    // they extend them (also what KWin does by itself for a never-seen
    // output, but KWin replays whatever arrangement it last saw for this
    // output set). The extend place is also where a replace session's
    // outputs are parked once the physical outputs are back
    // (PhysicalOutputGuard::restore(), SessionWrapper::parkVirtualOutputs()).
    // Known with a snapshot - this session's, or the one the guard still
    // holds because a previous session's restore has not verified yet (a
    // reconnect during the pending retry gets its snapshot() refused): the
    // retry that restores that snapshot parks THIS session's outputs beside
    // it, so it needs the placements too, and the extend reconcile parks
    // when the physical outputs are back on their own. Without any snapshot
    // there is no physical union to sit beside, and no restore that could
    // park anything.
    std::optional<QPoint> extendAnchor;
    if (snapshotTaken || m_outputGuard.held()) {
        const QRect physical = KRdp::OutputSnapshot::enabledUnion(m_outputGuard.physicalOutputs());
        if (physical.isValid()) {
            extendAnchor = KRdp::OutputSnapshot::rightmostEnabledAnchor(m_outputGuard.physicalOutputs());
        }
    }
    const QPoint anchor = canReplace ? QPoint(0, 0) : extendAnchor.value_or(QPoint(0, 0));
    const QString policyLabel = canReplace ? u"replace"_s : u"extend"_s;

    // One virtual output per monitor (Phase B), the client's own (Client) or
    // the physical layout's (Physical, OPT-041 S5) - only when that layout
    // has a usable two-or-more-monitor list (sanitize() keeps either list
    // only with two or more usable entries and exactly one primary; the
    // physical layout is run through sanitize() the same way just above,
    // so it is empty whenever nothing was enabled to mirror or the mirrored
    // layout was not sanitize-usable) and a previous attempt did not have an
    // output that never appeared. Single never opens more than one output.
    const bool multiOutput = m_virtualLayout != VirtualLayout::Single && info.monitors.size() >= 2 && !wrapper->forceSingleVirtual;
    if (!multiOutput) {
        if (wrapper->forceSingleVirtual) {
            qInfo() << "MonitorMode=virtual: falling back to one output of" << info.desktopSize << "for the client's" << info.monitors.size() << "monitors";
        }
        // sanitize() keeps desktopSize usable (an unusable monitor union
        // drops the list and the fallback replaces an unusable size);
        // singleSize() is the belt for the one output an encoder must open.
        const QSize size = KRdp::ClientDisplay::singleSize(info, m_virtualFallbackSize);
        std::vector<std::unique_ptr<KRdp::AbstractSession>> sessions;
        auto session = makeSession();
        const auto name = KRdp::ClientDisplay::virtualMonitorName(0, size);
        session->setVirtualMonitor(KRdp::VirtualMonitor{name, size, 1.0});
        // The RDPGFX surface this session feeds; see AbstractSession::setMonitorIndex().
        session->setMonitorIndex(0);
        wrapper->virtualPrimaryName = KRdp::OutputSnapshot::VirtualPrefix + name;
        wrapper->virtualPlacements.push_back({wrapper->virtualPrimaryName, anchor});
        if (extendAnchor) {
            wrapper->virtualParkPlacements = {{wrapper->virtualPrimaryName, *extendAnchor}};
            m_outputGuard.setParkPlacements(wrapper->virtualParkPlacements, wrapper->virtualPrimaryName);
        }
        connectVirtualSession(session.get(), wrapper);
        // See setQuality() for why the direct call is skipped with adaptive quality on.
        if (m_quality.has_value() && !m_adaptiveQuality) {
            session->setVideoQuality(m_quality.value());
        }
        sessions.push_back(std::move(session));

        qInfo().noquote() << "MonitorMode=virtual: one output" << size << "policy" << policyLabel << "placed at" << anchor;
        wrapper->setSessions(std::move(sessions), MonitorLayout{});
        return;
    }

    // The client's own layout, anchored where the outputs go. The RDP layout
    // is the same rects (KWin-global pixels; every virtual output is at
    // scale 1, so logical equals pixels and the input path's scale is 1):
    // the client gets its own monitor arrangement back in ResetGraphics
    // (OPT-040) and each surface carries one of its monitors. Placements
    // and park placements are the same rects' top-lefts at the two anchors.
    const auto rects = KRdp::ClientDisplay::placement(info.monitors, anchor);
    const auto parkRects = extendAnchor ? KRdp::ClientDisplay::placement(info.monitors, *extendAnchor) : QVector<QRect>();

    std::vector<std::unique_ptr<KRdp::AbstractSession>> sessions;
    sessions.reserve(size_t(info.monitors.size()));
    MonitorLayout layout;
    layout.scale = 1.0;
    for (qsizetype i = 0; i < info.monitors.size(); ++i) {
        const auto &monitor = info.monitors.at(i);
        auto session = makeSession();
        const auto name = KRdp::ClientDisplay::virtualMonitorName(int(i), monitor.geometry.size());
        const QString kwinName = KRdp::OutputSnapshot::VirtualPrefix + name;
        session->setVirtualMonitor(KRdp::VirtualMonitor{name, monitor.geometry.size(), 1.0});
        // The RDPGFX surface this session feeds; see AbstractSession::setMonitorIndex().
        session->setMonitorIndex(int(i));
        wrapper->virtualPlacements.push_back({kwinName, rects.at(i).topLeft()});
        if (!parkRects.isEmpty()) {
            wrapper->virtualParkPlacements.push_back({kwinName, parkRects.at(i).topLeft()});
        }
        if (monitor.primary) {
            wrapper->virtualPrimaryName = kwinName;
        }
        layout.monitors.push_back(KRdp::VideoMonitor{.geometry = rects.at(i), .primary = monitor.primary});
        layout.names.push_back(kwinName);

        connectVirtualSession(session.get(), wrapper);
        // See setQuality() for why the direct call is skipped with adaptive quality on.
        if (m_quality.has_value() && !m_adaptiveQuality) {
            session->setVideoQuality(m_quality.value());
        }
        sessions.push_back(std::move(session));
    }
    if (extendAnchor) {
        m_outputGuard.setParkPlacements(wrapper->virtualParkPlacements, wrapper->virtualPrimaryName);
    }

    qInfo().noquote() << QStringLiteral("MonitorMode=virtual: %1 outputs mirroring the client layout, policy %2, anchor %3,%4: %5")
                             .arg(sessions.size())
                             .arg(policyLabel)
                             .arg(anchor.x())
                             .arg(anchor.y())
                             .arg(layoutSummary(layout.monitors));
    wrapper->setSessions(std::move(sessions), layout);
}

void SessionController::connectVirtualSession(KRdp::AbstractSession *session, SessionWrapper *wrapper)
{
    // Not part of m_sessionConnections on purpose: they die with the session
    // object, and a rebuild never replaces a virtual session.
    connect(session, &KRdp::AbstractSession::started, wrapper, &SessionWrapper::maybeApplyVirtualPolicy);
    connect(session, &KRdp::AbstractSession::streamActiveChanged, wrapper, [wrapper](bool) {
        wrapper->maybeApplyVirtualPolicy();
    });
    connect(session, &KRdp::AbstractSession::outputGeometryChanged, wrapper, [wrapper](const QRect &) {
        // A pointer move or a cursor sample recorded for the takeover
        // detector between a kscreen-doctor call and this geometry update
        // was mapped through the output's old origin, so it is not where
        // the pointer is, and a compositor-driven move must not read as two
        // far samples; the detector drops both references and sits out the
        // churn (Task 6c review Minor 3, final re-review).
        wrapper->takeover.outputMoved(wrapper->m_clock.elapsed());
        if (wrapper->policyApplied) {
            // Multi-output: the outputs moved after the policy (a park, a
            // replayed arrangement); the client's layout has to follow.
            wrapper->scheduleVirtualLayoutAdoption();
            return;
        }
        wrapper->maybeApplyVirtualPolicy();
    });
    connect(session, &KRdp::AbstractSession::virtualOutputUnresolved, wrapper, [this, wrapper]() {
        // Also fires when an output KWin removed mid-session stays away for
        // 5 s; by then the policy has long been applied and stays so.
        if (wrapper->policyApplied) {
            // Not for a sibling of an output that already failed to appear
            // (the multi-output set is still installed while the queued
            // single-output rebuild is pending): that rebuild replaces both.
            const bool rebuildPending = wrapper->forceSingleVirtual && !wrapper->layout.isEmpty();
            if (!rebuildPending) {
                qWarning() << "Virtual output lost mid-session and not back within 5 s: pointer input stays gated until it reappears";
            }
            return;
        }
        // The output may still turn up later; once this has fired the policy
        // is never applied, so a late outputGeometryChanged() changes nothing.
        wrapper->policyApplied = true;
        if (!wrapper->layout.isEmpty()) {
            // One of several: the client is better served by one output at
            // its desktop size than by a layout with a hole in it. Queued,
            // since the rebuild destroys the session this signal came from,
            // and held by QPointer, since the connection may close first.
            // forceSingleVirtual is set here already so a sibling's timeout
            // in the meantime is recognised above.
            qWarning() << "A virtual output never appeared; falling back to a single virtual output";
            wrapper->forceSingleVirtual = true;
            QPointer<SessionWrapper> guard(wrapper);
            QMetaObject::invokeMethod(this, [this, guard]() {
                if (guard) {
                    rebuildAsSingleVirtual(guard.data());
                }
            }, Qt::QueuedConnection);
            return;
        }
        qWarning() << "Virtual output unresolved: pointer input stays gated and the" << (wrapper->virtualPolicy == VirtualPolicy::Replace ? "replace" : "extend")
                   << "policy is not applied";
    });
}

void SessionController::rebuildAsSingleVirtual(SessionWrapper *wrapper)
{
    if (!wrapper || !wrapper->connection || !wrapper->forceSingleVirtual || wrapper->layout.isEmpty()) {
        // Nothing to do, or done already (the layout is empty once the
        // single-output build has replaced the set).
        return;
    }
    // setSessions() replaces the vector wholesale, but buildVirtualSessions()
    // refuses to build over live sessions: drop them first. Their virtual
    // outputs go away with them; the physical layout was never touched,
    // because the policy is only applied once every output has resolved,
    // and the guard's snapshot is retaken by the build (an untouched
    // snapshot is just dropped by snapshot() itself).
    wrapper->sessions.clear();
    buildVirtualSessions(wrapper);
}

void SessionController::rebuildSessions()
{
    for (const auto &wrapper : m_wrappers) {
        // Mode changes apply to the next connection for a virtual wrapper;
        // rebuilding it here would drop the virtual output under the client.
        // A wrapper with no sessions at all has not had its build yet (every
        // build waits for the client's display info, a virtual one for its
        // output, a KRDPCTL client's for its first record) or is query-only
        // and never gets one: building here would either make the pending
        // build a no-op and hand the client a physical capture, or build for
        // a client that asked for nothing.
        // A layout client's sessions come from the KRDPCTL layout, never
        // from the configured mode.
        if (!wrapper || !wrapper->connection || wrapper->outputGuard || wrapper->layoutClient || wrapper->sessions.empty()) {
            continue;
        }
        buildSessions(wrapper.get());
    }
}

void SessionController::onNewConnection(KRdp::RdpConnection *newConnection)
{
    // MonitorMode=virtual's one-client rule is applied where the configured
    // build happens (buildConfiguredSessions()), not here: whether this
    // client joined KRDPCTL - and so follows the owner/viewer rules instead,
    // or only wants a soft `query` - is not known before its capabilities
    // exchange.
    auto wrapper = std::make_unique<SessionWrapper>(newConnection, m_sni, &m_displayWakeGuard);
    wrapper->controlId = u"c%1"_s.arg(++m_connectionCounter);
    // Every mode builds once the capabilities exchange is in (session
    // thread): that is the first point at which the connection knows whether
    // the client joined KRDPCTL, whose clients get no session until their
    // first record (OPT-044), and virtual mode needs the client's desktop
    // size from the same exchange anyway. Nothing is lost for the other
    // modes: sessions only start streaming when the video stream is enabled,
    // which is later still (drdynvc ready), and setSessions() starts them at
    // once if that has already happened. Both signals are emitted from the
    // session thread and queued here in order, so a record cannot overtake
    // the display info; the record connection is made now rather than in
    // onClientDisplayInfo() so nothing can slip through in between.
    connect(newConnection, &KRdp::RdpConnection::clientDisplayInfoReceived, wrapper.get(), [this, wrapper = wrapper.get()]() {
        onClientDisplayInfo(wrapper);
    }, Qt::QueuedConnection);
    connect(newConnection, &KRdp::RdpConnection::controlRecordReceived, wrapper.get(), [this, wrapper = wrapper.get()](const QJsonObject &record) {
        onControlRecord(wrapper, record);
    }, Qt::QueuedConnection);
    connect(&wrapper->controlTimer, &QTimer::timeout, wrapper.get(), [this, wrapper = wrapper.get()]() {
        onControlTimeout(wrapper);
    });
    connect(&wrapper->layoutRetryTimer, &QTimer::timeout, wrapper.get(), [this, wrapper = wrapper.get()]() {
        if (wrapper->layoutClient) {
            buildLayoutSessions(wrapper, m_layoutExecutor.current(), true);
        }
    });
    connect(wrapper.get(), &SessionWrapper::layoutRecordDue, this, &SessionController::sendLayoutNow);
    newConnection->videoStream()->setCodecPreference(m_codecPreference);
    // Seeded from the controller's configured default; a client's own `chroma` (onControlChroma())
    // overrides it for this connection only, before any session is created (setSessions() applies
    // whatever wrapper->m_chromaPolicy holds at that point).
    wrapper->m_chromaPolicy = m_chromaPolicyDefault;
    if (m_quality.has_value()) {
        newConnection->videoStream()->setQualityCap(quint8(m_quality.value()));
    }
    newConnection->videoStream()->setAdaptiveQuality(m_adaptiveQuality);
    newConnection->setAudioPriorityDefault(m_audioPriorityDefault);

    connect(wrapper.get(), &SessionWrapper::connectionDestroyed, this, [this](SessionWrapper *wrapper) {
        const QString id = wrapper->controlId;
        const bool layoutClient = wrapper->layoutClient;
        if (m_applying == wrapper) {
            m_applying = nullptr;
        }
        m_wrappers.erase(std::remove_if(m_wrappers.begin(),
                                        m_wrappers.end(),
                                        [wrapper](std::unique_ptr<SessionWrapper> &entry) {
                                            return entry.get() == wrapper;
                                        }),
                         m_wrappers.end());
        // After the erase: the leaving owner's sessions are gone before the
        // outputs they streamed are, and a release re-describes the others.
        if (layoutClient || m_layoutOwner.roleOf(id) != LayoutOwner::Role::None) {
            onLayoutClientGone(id, u"disconnect"_s);
        }
        updateRestoreAction();
    });
    connect(wrapper.get(), &SessionWrapper::physicalLayoutOwnershipChanged, this, &SessionController::updateRestoreAction);
    // Queued: the detector fires from a cursor-update slot, and the release
    // runs kscreen-doctor synchronously and reshuffles the screens under it.
    // The same slot serves a KRDPCTL layout client whose layout keeps a real
    // monitor dark (armLayoutTakeover()): releasePhysicalOutputs() hands the
    // desk back and broadcasts `takeover`.
    connect(wrapper.get(), &SessionWrapper::consoleActivityDetected, this, &SessionController::releasePhysicalOutputs, Qt::QueuedConnection);

    connect(wrapper.get(), &SessionWrapper::sessionError, this, [newConnection] {
        newConnection->close(KRdp::RdpConnection::CloseReason::None);
    });

    m_wrappers.push_back(std::move(wrapper));
}

void SessionController::onClientDisplayInfo(SessionWrapper *wrapper)
{
    if (!wrapper || !wrapper->connection || wrapper->controlGate != SessionWrapper::ControlGate::Undecided) {
        // A reactivation runs the capabilities callback again; the gate is
        // decided once per connection.
        return;
    }
    if (!wrapper->connection->hasControlChannel()) {
        buildConfiguredSessions(wrapper);
        return;
    }
    wrapper->controlGate = SessionWrapper::ControlGate::Waiting;
    wrapper->controlTimer.start();
    qInfo() << "KRDPCTL: client joined the channel; holding the session build for its first record";
}

void SessionController::onControlRecord(SessionWrapper *wrapper, const QJsonObject &record)
{
    if (!wrapper || !wrapper->connection) {
        return;
    }
    auto *connection = wrapper->connection.data();
    const QString type = record.value(QLatin1String("type")).toString();
    if (type == QLatin1String("audio-priority")) {
        const auto request = KRdp::AudioPriority::parse(record);
        if (!request) {
            connection->sendControlRecord(KRdp::AudioPriority::reply(record, false, u"invalid audio-priority request"_s));
            return;
        }
        connection->setAudioPriority(request->enabled);
        connection->sendControlRecord(KRdp::AudioPriority::reply(record, connection->audioPriorityActive()));
        return; // This policy never consumes the initial layout-selection gate.
    }
    // A record proves the channel: a gate still undecided (cannot happen
    // with the queued ordering, see onNewConnection()) is decided by it.
    const bool first = wrapper->controlGate == SessionWrapper::ControlGate::Undecided || wrapper->controlGate == SessionWrapper::ControlGate::Waiting;
    // `codec` is a preflight capability record: it precedes the initial apply but does not
    // itself decide the layout-control gate.
    const bool codecPreflight = type == QLatin1String("codec") && first;
    if (first && !codecPreflight) {
        wrapper->controlTimer.stop();
    }

    // Spec R1: records are versioned; this server speaks v1 only. Another
    // version's record is not interpreted, whatever its type says.
    const int version = record.value(QLatin1String("v")).toInt();
    if (version != KRdp::LayoutControl::ProtocolVersion) {
        connection->sendControlRecord(
            KRdp::LayoutControl::errorRecord({u"unsupported"_s, u"protocol version %1 is not supported; this server speaks %2"_s.arg(version).arg(KRdp::LayoutControl::ProtocolVersion)}));
        if (first) {
            qInfo() << "KRDPCTL: first record has protocol version" << version << "; using the configured MonitorMode";
            buildConfiguredSessions(wrapper);
        }
        return;
    }

    if (type == QLatin1String("query")) {
        const auto layout = layoutFor(wrapper);
        connection->sendControlRecord(KRdp::LayoutControl::layoutRecord(layout));
        if (first) {
            // Soft query: the layout and nothing else; the connection ends
            // when the client hangs up.
            wrapper->controlGate = SessionWrapper::ControlGate::QueryOnly;
            qInfo().nospace().noquote() << u"KRDPCTL: query → layout ("_s << layout.monitors.size() << u" monitors), no session built"_s;
        } else {
            qInfo().nospace().noquote() << u"KRDPCTL: query → layout ("_s << layout.monitors.size() << u" monitors)"_s;
        }
        return;
    }

    if (type == QLatin1String("apply")) {
        onControlApply(wrapper, record, first);
        return;
    }

    if (type == QLatin1String("attach")) {
        onControlAttach(wrapper, record, first);
        return;
    }

    if (type == QLatin1String("chroma")) {
        onControlChroma(wrapper, record, first);
        return;
    }

    if (type == QLatin1String("codec")) {
        onControlCodec(wrapper, record);
        return;
    }

    if (type == QLatin1String("media")) {
        onControlMedia(wrapper, record);
        return;
    }

    if (type == QLatin1String("pong")) {
        m_layoutOwner.heartbeatOk(wrapper->controlId);
        if (m_layoutOwner.roleOf(wrapper->controlId) == LayoutOwner::Role::Owner) {
            m_pongPending = false;
        }
        if (first) {
            // A pong before any apply: this client speaks the protocol but
            // asked for nothing; the gate closes on the configured mode.
            qInfo() << "KRDPCTL: first record is a pong; using the configured MonitorMode";
            buildConfiguredSessions(wrapper);
        }
        return;
    }

    connection->sendControlRecord(KRdp::LayoutControl::errorRecord({u"unsupported"_s, u"unknown record type \"%1\""_s.arg(type)}));
    if (first) {
        // Whatever this client speaks, its first record was not one of ours;
        // serve it as a client without the channel rather than leave it
        // staring at nothing for the rest of the timeout.
        qInfo() << "KRDPCTL: first record has unknown type" << type << "; using the configured MonitorMode";
        buildConfiguredSessions(wrapper);
    }
}

void SessionController::onControlCodec(SessionWrapper *wrapper, const QJsonObject &record)
{
    auto *connection = wrapper->connection.data();
    const QJsonArray codecs = record.value(QLatin1String("codecs")).toArray();
    const bool adaptive = record.value(QLatin1String("adaptive")).toBool(false);
    // `codecs` is an ordered allow-list, not merely a capability set. An empty list is
    // intentional: the client asked for ordinary AVC. Older clients also sent `prefer`;
    // their HEVC,AV1 order already describes their historical auto choice.
    std::optional<KRdp::VideoCodec> selected;
    QVector<KRdp::VideoCodec> ordered;
    for (const QJsonValue &value : codecs) {
        if (!value.isString()) {
            connection->sendControlRecord(KRdp::LayoutControl::errorRecord({u"invalid"_s, u"codec codecs must be an array of hevc and/or av1"_s}));
            return;
        }
        const QString name = value.toString().trimmed().toLower();
        if (name == QLatin1String("hevc")) {
            ordered.append(KRdp::VideoCodec::Hevc);
            continue;
        }
        if (name == QLatin1String("av1")) {
            ordered.append(KRdp::VideoCodec::Av1);
            continue;
        }
        connection->sendControlRecord(KRdp::LayoutControl::errorRecord({u"invalid"_s, u"codec codecs must be an array of hevc and/or av1"_s}));
        return;
    }
    if (!ordered.isEmpty()) selected = ordered.first();
    connection->videoStream()->setPrivateCodecPolicy(ordered, adaptive);
    connection->sendControlRecord(QJsonObject{{u"type"_s, u"codec"_s}, {u"v"_s, KRdp::LayoutControl::ProtocolVersion}, {u"ok"_s, true}, {u"selected"_s, selected ? QLatin1String(KRdp::VideoCodecSupport::codecName(*selected)) : u"avc"_s}});
    qInfo() << "KRDPCTL: private codec selected" << (selected ? KRdp::VideoCodecSupport::codecName(*selected) : "avc");
}

void SessionController::onControlMedia(SessionWrapper *wrapper, const QJsonObject &record)
{
    auto *connection = wrapper->connection.data();
    const QJsonValue playback = record.value(QLatin1String("playback"));
    const QJsonValue microphone = record.value(QLatin1String("microphone"));
    const QJsonValue camera = record.value(QLatin1String("camera"));
    const QJsonValue silenceHost = record.value(QLatin1String("silenceHost"));
    if (!playback.isBool() || !microphone.isBool() || !camera.isBool() || (!silenceHost.isUndefined() && !silenceHost.isBool())) {
        connection->sendControlRecord(KRdp::LayoutControl::errorRecord({u"invalid"_s, u"media playback, microphone, camera, and silenceHost must be booleans"_s}));
        return;
    }
    // A silent host only makes sense when audio is actually redirected.
    const bool isolated = playback.toBool() && silenceHost.toBool(false);
    connection->setMediaPolicy(playback.toBool(), microphone.toBool(), camera.toBool(), isolated);
    connection->sendControlRecord(QJsonObject{{u"type"_s, u"media"_s}, {u"v"_s, KRdp::LayoutControl::ProtocolVersion}, {u"ok"_s, true}, {u"playback"_s, playback}, {u"microphone"_s, microphone}, {u"camera"_s, camera}, {u"silenceHost"_s, isolated}});
}

void SessionController::onControlTimeout(SessionWrapper *wrapper)
{
    if (!wrapper || !wrapper->connection || wrapper->controlGate != SessionWrapper::ControlGate::Waiting) {
        return;
    }
    qInfo() << "KRDPCTL client sent nothing in 3 s; using the configured MonitorMode";
    buildConfiguredSessions(wrapper);
}

void SessionController::buildConfiguredSessions(SessionWrapper *wrapper)
{
    if (!wrapper || !wrapper->connection) {
        return;
    }
    wrapper->controlGate = SessionWrapper::ControlGate::Built;
    if (wrapper->connection->state() == KRdp::RdpConnection::State::Closed) {
        // The client left while the gate was open (a KRDPCTL client that
        // hung up inside the 3 s, say); the wrapper is about to go away.
        return;
    }
    if (m_virtualMode) {
        // One virtual desktop, one snapshot of the physical outputs: a second
        // configured-mode client would need a second output and would fight
        // over the layout. Any other connection with sessions counts - not
        // one that only asked for a `query`, and not one already closed and
        // waiting to be destroyed, or a client reconnecting right after a
        // drop would be refused. The peer is running by now (this follows
        // the capabilities exchange), so close() is immediate.
        // Known window (accepted): two channel-less clients whose capability
        // exchanges overlap both pass here (neither has sessions yet); the
        // second's snapshot() is then refused and it runs as extend - two
        // virtual desktops, no dark desk. The accept-time check this
        // replaced allowed one, but could not tell a `query` from a client.
        // A KRDPCTL layout in force (or in flight, or merely owned) counts
        // as busy too: the guard cannot serve both (Task 3 review, Important 3).
        const bool busy = std::any_of(m_wrappers.cbegin(),
                                      m_wrappers.cend(),
                                      [wrapper](const std::unique_ptr<SessionWrapper> &w) {
                                          return w && w.get() != wrapper && w->connection && !w->sessions.empty()
                                              && w->connection->state() != KRdp::RdpConnection::State::Closed;
                                      })
            || m_layoutExecutor.controlling() || m_layoutExecutor.busy() || m_layoutOwner.hasOwner();
        if (busy) {
            qWarning() << "MonitorMode=virtual serves one connection at a time; refusing a second client";
            wrapper->connection->close(KRdp::RdpConnection::CloseReason::None);
            return;
        }
        buildVirtualSessions(wrapper);
    } else {
        buildSessions(wrapper);
    }
}

void SessionController::onControlApply(SessionWrapper *wrapper, const QJsonObject &record, bool first)
{
    auto *connection = wrapper->connection.data();
    const QString id = wrapper->controlId;
    // Whatever goes wrong below, a channel client that asked for a layout
    // is served from the layout there is, as a viewer, rather than from the
    // configured mode or nothing at all.
    auto refuse = [&](const KRdp::LayoutControl::Error &error, const QString &why) {
        qInfo().noquote() << u"apply from %1: %2"_s.arg(id, why);
        connection->sendControlRecord(KRdp::LayoutControl::errorRecord(error));
        if (first || !wrapper->layoutClient) {
            buildAsViewer(wrapper);
        }
    };

    const auto request = KRdp::LayoutControl::applyFromJson(record);
    if (!request) {
        refuse({u"invalid"_s, u"malformed apply: monitors must be an array of monitor entries"_s}, u"invalid: malformed apply"_s);
        return;
    }
    // The gate is decided by this record, whatever becomes of it: the
    // session set comes from the layout path from here on (the build itself
    // lands when the executor is done), and a record arriving in between
    // must not be taken for the first.
    wrapper->controlGate = SessionWrapper::ControlGate::Built;
    if (const auto notOwner = m_layoutOwner.canAcquire(id, request->takeoverLayout)) {
        refuse(*notOwner, u"not-owner"_s);
        return;
    }
    // A configured `virtual` client (replace or extend) and layout control
    // share one guard; its teardown would restore its own snapshot under
    // the layout owner (Task 3 review, Important 3).
    const bool legacyVirtualHolds = std::any_of(m_wrappers.cbegin(), m_wrappers.cend(), [](const std::unique_ptr<SessionWrapper> &w) {
        return w && w->outputGuard && w->connection && w->connection->state() != KRdp::RdpConnection::State::Closed;
    });
    if (legacyVirtualHolds) {
        refuse({u"invalid"_s, u"a configured virtual-monitor client holds the physical outputs"_s}, u"invalid: configured virtual-monitor client active"_s);
        return;
    }
    if (m_layoutExecutor.busy()) {
        refuse({u"invalid"_s, u"another apply is still being applied; try again"_s}, u"invalid: apply in progress"_s);
        return;
    }

    // Planned from the applied layout's targets, not from what KWin has
    // right now: the sanitiser must judge the target's union (a read-back
    // after a replay had a stand-in at 7680,0 and refused every apply as
    // over 8192 px, 2026-09-19 step 5), and an apply after a failed
    // re-assert asks for the targets again. Same as current() otherwise.
    const auto current = m_layoutExecutor.target();
    const auto planned = KRdp::LayoutControl::plan(current, *request, id, KRdp::LayoutControl::Caps{});
    if (const auto *error = std::get_if<KRdp::LayoutControl::Error>(&planned)) {
        refuse(*error, u"invalid: %1"_s.arg(error->message));
        return;
    }
    const auto &plan = std::get<KRdp::LayoutControl::Plan>(planned);

    // Ownership moves now, before the executor runs: the first client to
    // apply a valid layout owns it, and a takeover is decided by the request
    // rather than by whether KWin cooperates. A refusal by the executor
    // hands it back below.
    const QString previousOwner = m_layoutOwner.owner();
    m_layoutOwner.tryAcquire(id, request->takeoverLayout);
    const bool tookOver = !previousOwner.isEmpty() && previousOwner != id;
    qInfo().noquote() << u"apply from %1: owner acquired%2 (%3 action(s), %4 monitors)"_s.arg(id, tookOver ? u" by takeover from %1"_s.arg(previousOwner) : (previousOwner == id ? u" (re-apply)"_s : QString()))
                             .arg(plan.actions.size())
                             .arg(plan.resulting.monitors.size());
    m_pongPending = false;
    if (!m_heartbeatTimer.isActive()) {
        m_heartbeatTimer.start();
    }

    // Nothing may read as console activity while the outputs move: the
    // arrangement warps the pointer, and those samples arrive queued behind
    // it. Every layout client's detector sits the apply out from now until
    // the build that follows it (armLayoutTakeover() clears the flag and
    // drops the references a warp may have spoiled); a refusal below does
    // the same at once. A 0-action plan (a debounced re-send that changes
    // nothing) warps nothing, so it must not cost that blind window either
    // - and should the executor arrange after all (a restated layout while
    // its last arrangement is unverified), its arrangementStarting() sets
    // the flag right before the change.
    if (!plan.actions.isEmpty()) {
        for (const auto &w : m_wrappers) {
            if (w && w->layoutClient) {
                w->layoutApplyInFlight = true;
            }
        }
    }
    // Set before execute(): finished() may fire from inside it.
    m_applying = wrapper;
    if (const auto error = m_layoutExecutor.execute(plan, id)) {
        m_applying = nullptr;
        for (const auto &w : m_wrappers) {
            if (w && w->layoutClient) {
                armLayoutTakeover(w.get(), current, false);
            }
        }
        // Ownership goes back to how it was: dropped if this apply took it,
        // returned if it was taken over; a re-applying owner keeps it.
        if (previousOwner != id) {
            m_layoutOwner.release(id);
            if (tookOver) {
                m_layoutOwner.tryAcquire(previousOwner, true);
            }
            if (wrapper->layoutClient) {
                // Still streaming the layout there is: a viewer, not nobody.
                m_layoutOwner.addViewer(id);
            }
        }
        refuse(*error, u"refused by the executor: %1"_s.arg(error->message));
    }
}

void SessionController::onControlAttach(SessionWrapper *wrapper, const QJsonObject &record, bool first)
{
    Q_UNUSED(first)
    auto *connection = wrapper->connection.data();
    const QString target = record.value(QLatin1String("target")).toString();
    if (target != QLatin1String("physical")) {
        connection->sendControlRecord(KRdp::LayoutControl::errorRecord({u"invalid"_s, u"attach target must be physical"_s}));
        return;
    }
    // Codec/media preflight records deliberately do not pick a session mode.
    // `attach` may therefore follow them even though they have closed the
    // first-record gate.  Once anything is streaming, changing the source
    // would be indistinguishable from a layout apply and is refused.
    if (!wrapper->sessions.empty() || wrapper->layoutClient) {
        connection->sendControlRecord(KRdp::LayoutControl::errorRecord({u"invalid"_s, u"attach is only valid before a session is built"_s}));
        return;
    }
    if (m_layoutExecutor.controlling() || m_layoutOwner.hasOwner()) {
        connection->sendControlRecord(KRdp::LayoutControl::errorRecord({u"invalid"_s, u"a layout-controlled session currently owns the console"_s}));
        return;
    }

    wrapper->controlTimer.stop();
    wrapper->controlGate = SessionWrapper::ControlGate::Built;
    // Do not use buildConfiguredSessions(): MonitorMode=virtual intentionally
    // creates an off-screen output.  Direct attach means the existing seat's
    // physical outputs, with no call to the layout executor.
    buildSessions(wrapper);
    connection->sendControlRecord(KRdp::LayoutControl::layoutRecord(layoutFor(wrapper)));
    qInfo().noquote() << u"KRDPCTL: %1 attached to the physical console (%2 session(s))"_s.arg(wrapper->controlId).arg(wrapper->sessions.size());
}

void SessionController::onControlChroma(SessionWrapper *wrapper, const QJsonObject &record, bool first)
{
    auto *connection = wrapper->connection.data();
    const QString id = wrapper->controlId;

    const auto request = KRdp::LayoutControl::chromaFromJson(record);
    if (!request) {
        qInfo().noquote() << u"chroma from %1: invalid: malformed chroma (motionGapMs/restMs/maxGapMs must be numbers)"_s.arg(id);
        connection->sendControlRecord(KRdp::LayoutControl::errorRecord({u"invalid"_s, u"malformed chroma: motionGapMs/restMs/maxGapMs must be numbers when present"_s}));
    } else {
        // Present fields override; an absent one keeps whatever this connection already has (the
        // controller's default if nothing has overridden it yet, or an earlier `chroma` from the
        // same client) - never AbstractSession/ChromaPolicy's own struct defaults.
        KRdp::ChromaPolicy merged = wrapper->m_chromaPolicy;
        if (request->motionGapMs) {
            merged.motionGapMs = *request->motionGapMs;
        }
        if (request->restMs) {
            merged.restMs = *request->restMs;
        }
        if (request->maxGapMs) {
            merged.maxGapMs = *request->maxGapMs;
        }
        if (!merged.isValid()) {
            qInfo().noquote() << u"chroma from %1: invalid: motionGap=%2 rest=%3 maxGap=%4 (need each in [16,5000] and motionGap <= rest <= maxGap)"_s.arg(id)
                                      .arg(merged.motionGapMs)
                                      .arg(merged.restMs)
                                      .arg(merged.maxGapMs);
            connection->sendControlRecord(
                KRdp::LayoutControl::errorRecord({u"invalid"_s, u"chroma policy must have each of motionGapMs/restMs/maxGapMs in [16,5000] and motionGapMs <= restMs <= maxGapMs"_s}));
        } else {
            // Applies to this connection's own sessions only: chroma is not a layout-ownership
            // concept (A10.4), so it is accepted whether or not this client owns the KRDPCTL layout.
            wrapper->m_chromaPolicy = merged;
            for (const auto &session : wrapper->sessions) {
                session->setChromaPolicy(merged);
            }
            qInfo().noquote() << u"chroma policy: motionGap=%1 rest=%2 maxGap=%3 (client)"_s.arg(merged.motionGapMs).arg(merged.restMs).arg(merged.maxGapMs);
        }
    }

    if (first) {
        // A chroma request says nothing about the desired layout; served like `pong` - the
        // configured MonitorMode, with whatever policy update above already landed on the wrapper
        // before its sessions are built.
        qInfo() << "KRDPCTL: first record is a chroma; using the configured MonitorMode";
        buildConfiguredSessions(wrapper);
    }
}

void SessionController::onLayoutApplied(const HostLayoutExecutor::Result &result)
{
    SessionWrapper *wrapper = m_applying.data();
    m_applying = nullptr;
    const bool alive = wrapper && wrapper->connection && wrapper->connection->state() != KRdp::RdpConnection::State::Closed;
    if (!alive && result.error) {
        // The requester left mid-apply and the release its leaving caused
        // abandoned the apply; that release re-describes everyone.
        return;
    }
    if (alive && result.error) {
        wrapper->connection->sendControlRecord(KRdp::LayoutControl::errorRecord(*result.error));
    }
    // What this apply removed or created is what no session may keep
    // streaming across it; everything else keeps running (Task 4).
    const QStringList changed = result.created + result.removed;
    if (alive) {
        // The requester's sessions come from the layout as KWin has it,
        // whether or not everything landed; the `layout` says what that is.
        const bool reset = buildLayoutSessions(wrapper, result.layout, false, changed);
        sendLayout(wrapper, reset);
    }
    // Everyone else on the channel - viewers, and the previous owner after a
    // takeover - follows the same layout, diffed the same way.
    describeLayoutClients(wrapper, changed);
    updateRestoreAction();
}

void SessionController::armLayoutTakeover(SessionWrapper *wrapper, const KRdp::LayoutControl::Layout &layout, bool sessionsChanged)
{
    // Armed exactly while the layout keeps a real monitor dark at the desk
    // (a stand-in or Private): that is when a real mouse moving means
    // someone at the desk wants their screens, as it does under a replace
    // session. With every real monitor lit there is nothing to take over
    // and a real mouse is ordinary use.
    const bool deskDark = m_layoutExecutor.controlling() && std::any_of(layout.monitors.cbegin(), layout.monitors.cend(), [](const KRdp::LayoutControl::HostMonitor &monitor) {
        return monitor.kind == KRdp::LayoutControl::Kind::Real && !monitor.lit;
    });
    const bool applyEnded = wrapper->layoutApplyInFlight;
    wrapper->layoutApplyInFlight = false;
    if (sessionsChanged || deskDark != wrapper->layoutTakeoverArmed) {
        // A fresh detector: its arm delay (KRdp::Takeover::ArmDelayMs) sits
        // out the pointer warps the arrangement just made, its references
        // were mapped through a table that no longer applies, and a
        // takeover that fired earlier (latched) must be able to fire again
        // once the owner's next apply darkens the desk again.
        wrapper->takeover = KRdp::Takeover::Detector{};
        if (deskDark) {
            wrapper->takeover.armed(wrapper->m_clock.elapsed());
        } else {
            wrapper->takeover.latch();
        }
        if (deskDark && !wrapper->layoutTakeoverArmed) {
            qInfo().noquote() << u"KRDPCTL: %1: desk takeover armed (a real monitor is dark)"_s.arg(wrapper->controlId);
        } else if (!deskDark && wrapper->layoutTakeoverArmed) {
            qInfo().noquote() << u"KRDPCTL: %1: desk takeover disarmed (every real monitor is lit)"_s.arg(wrapper->controlId);
        }
    } else if (applyEnded) {
        // Nothing changed for this connection, but an apply just moved
        // other outputs and may have warped the pointer: the detector keeps
        // its arm (no new arm delay) and drops the references it would
        // otherwise judge the next sample against, sitting the churn out
        // (review Minor 3: not a fresh detector on every build - a retry
        // that changed nothing leaves it alone entirely).
        wrapper->takeover.outputMoved(wrapper->m_clock.elapsed());
    }
    wrapper->layoutTakeoverArmed = deskDark;
}

bool SessionController::buildLayoutSessions(SessionWrapper *wrapper, const KRdp::LayoutControl::Layout &layout, bool retrying, const QStringList &changedOutputs)
{
    if (!wrapper || !wrapper->connection) {
        return false;
    }
    wrapper->controlGate = SessionWrapper::ControlGate::Built;
    wrapper->layoutClient = true;
    if (!retrying) {
        wrapper->layoutRetriesLeft = LayoutBuildRetries;
        wrapper->layoutRetryTimer.stop();
    }
    if (wrapper->connection->state() == KRdp::RdpConnection::State::Closed) {
        wrapper->layoutRetryTimer.stop();
        return false;
    }

    const auto screens = QGuiApplication::screens();
    // The output that carries each monitor, by name, resolved once from the
    // executor's table (no process run): a QScreen index is only valid for
    // as long as the list holds still, and the session remembers the name
    // for its own recovery.
    const QStringList outputNames = m_layoutExecutor.outputNamesFor(layout);
    struct Resolved {
        qsizetype monitor;
        QString output;
        int screen;
    };
    QList<Resolved> resolved;
    QStringList wanted;
    QStringList missing;
    for (qsizetype m = 0; m < layout.monitors.size(); ++m) {
        const auto &monitor = layout.monitors.at(m);
        const QString outputName = outputNames.value(m);
        int index = -1;
        for (qsizetype i = 0; i < screens.size(); ++i) {
            if (screens.at(i) && screens.at(i)->name() == outputName) {
                index = int(i);
                break;
            }
        }
        if (outputName.isEmpty() || index < 0) {
            missing.push_back(u"%1 (%2)"_s.arg(monitor.id, outputName.isEmpty() ? u"no output"_s : outputName));
            continue;
        }
        resolved.push_back({m, outputName, index});
        wanted.push_back(outputName);
    }

    // Which of the sessions running now stay: the ones over an output the
    // new layout still streams from, unless this apply removed or created
    // it (LayoutSessions::diff, pure). A real monitor going lit → dark →
    // stood in changes the name under it each time, a virtual monitor's
    // resize likewise, so a name match is a stream that can carry on.
    const QStringList streaming = wrapper->sessions.empty() ? QStringList() : wrapper->layout.names;
    const auto diff = KRdp::LayoutSessions::diff(streaming, wanted, changedOutputs);

    if (!missing.isEmpty()) {
        // An output KWin is still re-adding (a physical output just switched
        // back on takes ~5 s to be a QScreen again) or one that never came:
        // build what is there and come back for the rest.
        if (wrapper->layoutRetriesLeft > 0) {
            --wrapper->layoutRetriesLeft;
            wrapper->layoutRetryTimer.start();
            if (!retrying || !diff.unchanged()) {
                qInfo().noquote() << u"KRDPCTL: %1: %2 not a screen yet; retrying the build (%3 left)"_s.arg(wrapper->controlId, missing.join(u", "_s)).arg(wrapper->layoutRetriesLeft);
            }
        } else {
            wrapper->layoutRetryTimer.stop();
            qWarning().noquote() << u"KRDPCTL: %1: not streaming %2: the output never became a screen"_s.arg(wrapper->controlId, missing.join(u", "_s));
        }
    } else {
        wrapper->layoutRetryTimer.stop();
    }
    if (resolved.isEmpty()) {
        // Nothing streamable right now. The previous sessions, if any,
        // capture outputs of a layout that is gone (a release just removed
        // them); dropping them keeps their recovery from falling back to a
        // workspace capture under the client.
        const bool dropped = !wrapper->sessions.empty();
        if (dropped) {
            qInfo().noquote() << u"KRDPCTL: %1: dropping %2 session(s) of the previous layout"_s.arg(wrapper->controlId).arg(wrapper->sessions.size());
            wrapper->sessions.clear();
            wrapper->layout = {};
        }
        armLayoutTakeover(wrapper, layout, dropped);
        return false;
    }

    // The RDP layout for what is there, before any session is touched, so
    // an unchanged connection can be left exactly as it runs.
    MonitorLayout monitorLayout;
    monitorLayout.scale = 1.0;
    QVector<KRdp::RemoteMonitorGeometry::Output> remoteOutputs;
    for (qsizetype i = 0; i < resolved.size(); ++i) {
        const auto &entry = resolved.at(i);
        const auto &monitor = layout.monitors.at(entry.monitor);
        QSize size = monitor.kind == KRdp::LayoutControl::Kind::Real && monitor.standIn && monitor.standInSize ? *monitor.standInSize : monitor.size;
        const qreal scale = monitor.kind == KRdp::LayoutControl::Kind::Real && monitor.standIn && monitor.standInScale ? *monitor.standInScale : monitor.scale;
        const qsizetype from = diff.source.at(i);
        if (from >= 0 && size_t(from) < wrapper->sessions.size()) {
            // A kept session knows the size it really captures (what
            // correctSurfaceSize() may already have patched its entry to).
            const QSize captured = wrapper->sessions[size_t(from)]->pixelSize();
            if (!captured.isEmpty()) {
                size = captured;
            }
        }
        // Keep KWin's logical origin separate from the RDP surface atlas.
        // Adjacent mixed-scale outputs overlap if pixel sizes are placed at
        // logical origins; projectToWire() packs them after all are known.
        remoteOutputs.push_back({monitor.position, size, scale, monitor.primary});
        monitorLayout.logicalOrigins.push_back(monitor.position);
        monitorLayout.names.push_back(entry.output);
        monitorLayout.scales.push_back(scale > 0.0 ? scale : 1.0);
        if (monitor.primary) {
            monitorLayout.scale = scale > 0.0 ? scale : 1.0;
        }
    }
    monitorLayout.monitors = KRdp::RemoteMonitorGeometry::projectToWire(remoteOutputs);
    // Exactly one primary, whatever the layout says (it always says one, but
    // the primary may be among the missing).
    const bool hasPrimary = std::any_of(monitorLayout.monitors.cbegin(), monitorLayout.monitors.cend(), [](const KRdp::VideoMonitor &monitor) {
        return monitor.primary;
    });
    if (!hasPrimary) {
        monitorLayout.monitors.first().primary = true;
        monitorLayout.scale = monitorLayout.scales.first();
    }

    const bool geometryChanged = monitorLayout.monitors != wrapper->layout.monitors || monitorLayout.scales != wrapper->layout.scales
        || monitorLayout.logicalOrigins != wrapper->layout.logicalOrigins;
    if (diff.unchanged() && !geometryChanged) {
        // Every output the layout wants is already streamed by a running
        // session and the RDP layout is the same: nothing to restart, no
        // ResetGraphics coming (a retry that found nothing new, an apply
        // that changed nothing for this connection).
        if (!retrying) {
            qInfo().noquote() << u"KRDPCTL: %1: layout unchanged for this connection; %2 session(s) kept"_s.arg(wrapper->controlId).arg(wrapper->sessions.size());
        }
        armLayoutTakeover(wrapper, layout, false);
        return false;
    }

    std::vector<std::unique_ptr<KRdp::AbstractSession>> sessions;
    sessions.reserve(size_t(resolved.size()));
    for (qsizetype i = 0; i < resolved.size(); ++i) {
        const auto &entry = resolved.at(i);
        const qsizetype from = diff.source.at(i);
        std::unique_ptr<KRdp::AbstractSession> session;
        if (from >= 0 && size_t(from) < wrapper->sessions.size()) {
            // Kept: the same stream carries on; only its surface index may
            // change (setSessions() re-wires it and leaves a running
            // encoder alone).
            session = std::move(wrapper->sessions[size_t(from)]);
            qInfo().noquote() << u"KRDPCTL: %1: session for %2 kept"_s.arg(wrapper->controlId, entry.output);
        } else {
            session = makeSession();
            session->setActiveStream(entry.screen);
            // See setQuality() for why the direct call is skipped with adaptive quality on.
            if (m_quality.has_value() && !m_adaptiveQuality) {
                session->setVideoQuality(m_quality.value());
            }
        }
        // The RDPGFX surface this session feeds; see AbstractSession::setMonitorIndex().
        session->setMonitorIndex(int(i));
        sessions.push_back(std::move(session));
    }

    qInfo().noquote() << u"KRDPCTL: %1 (%2): %3 session(s) from the host layout: %4 (kept %5, created %6, dropped %7)"_s
                             .arg(wrapper->controlId, LayoutOwner::roleName(m_layoutOwner.roleOf(wrapper->controlId)))
                             .arg(sessions.size())
                             .arg(layoutSummary(monitorLayout.monitors))
                             .arg(diff.kept.size())
                             .arg(diff.created.size())
                             .arg(diff.dropped.size());
    // The dropped sessions (still in wrapper->sessions, beside the moved-out
    // slots) go when setSessions() replaces the vector.
    wrapper->setSessions(std::move(sessions), monitorLayout);
    armLayoutTakeover(wrapper, layout, true);
    if (geometryChanged) {
        // The reset that follows recreates every surface, and a kept
        // session's next frame is a P-frame with no reference picture behind
        // it. VideoStream asks for a keyframe when that frame lands, but its
        // per-surface request limiter (2 s) survives the reset, so a second
        // reset within that window would leave the surface undecodable
        // until the encoder's next organic IDR - 600 frames away (Task 4
        // review, Important 2). Ask now, once per kept session: the private
        // KPipeWire re-feeds the last frame as an IDR at once, so it is the
        // first frame on the new surface; stock KPipeWire restarts the
        // encoded stream, which opens with one. A created session opens
        // with its own IDR and needs nothing.
        for (qsizetype i = 0; i < diff.source.size() && size_t(i) < wrapper->sessions.size(); ++i) {
            if (diff.source.at(i) >= 0) {
                wrapper->sessions[size_t(i)]->requestKeyFrame();
            }
        }
    }
    // A ResetGraphics follows only when the RDP layout changed: same rects
    // over new sessions (a real monitor swapped for its native-size
    // stand-in) keep their surfaces, and the owed record need not wait.
    return geometryChanged;
}

void SessionController::buildAsViewer(SessionWrapper *wrapper)
{
    if (!wrapper || !wrapper->connection) {
        return;
    }
    m_layoutOwner.addViewer(wrapper->controlId);
    const bool reset = buildLayoutSessions(wrapper, m_layoutExecutor.current());
    sendLayout(wrapper, reset);
}

void SessionController::describeLayoutClients(SessionWrapper *except, const QStringList &changedOutputs)
{
    if (m_wrappers.empty()) {
        return;
    }
    const auto layout = m_layoutExecutor.current();
    for (const auto &wrapper : m_wrappers) {
        if (!wrapper || wrapper.get() == except || !wrapper->layoutClient || !wrapper->connection
            || wrapper->connection->state() == KRdp::RdpConnection::State::Closed) {
            continue;
        }
        // Every layout client that is not the owner is a viewer (a no-op for
        // the owner and for a registered viewer).
        m_layoutOwner.addViewer(wrapper->controlId);
        const bool reset = buildLayoutSessions(wrapper.get(), layout, false, changedOutputs);
        sendLayout(wrapper.get(), reset);
    }
}

KRdp::LayoutControl::Layout SessionController::layoutFor(const SessionWrapper *wrapper) const
{
    auto layout = m_layoutExecutor.current();
    layout.owner = m_layoutOwner.owner();
    layout.you = LayoutOwner::roleName(wrapper ? m_layoutOwner.roleOf(wrapper->controlId) : LayoutOwner::Role::None);
    return layout;
}

void SessionController::sendLayout(SessionWrapper *wrapper, bool afterReset, bool takeover)
{
    if (!wrapper || !wrapper->connection) {
        return;
    }
    if (afterReset) {
        // After the surfaces that describe the build have gone out (or the
        // fallback): the record's content is read then, so it is always
        // current.
        wrapper->scheduleLayoutRecord(takeover);
        return;
    }
    // No ResetGraphics is coming for this build (nothing changed on the
    // wire for this connection): the record goes now, and supersedes one
    // still owed from an earlier build.
    wrapper->cancelLayoutRecord();
    sendLayoutNow(wrapper, takeover);
}

void SessionController::sendLayoutNow(SessionWrapper *wrapper, bool takeover)
{
    if (!wrapper || !wrapper->connection || wrapper->connection->state() == KRdp::RdpConnection::State::Closed) {
        return;
    }
    const auto layout = layoutFor(wrapper);
    wrapper->connection->sendControlRecord(takeover ? KRdp::LayoutControl::takeoverRecord(layout) : KRdp::LayoutControl::layoutRecord(layout));
}

SessionWrapper *SessionController::wrapperFor(const QString &controlId) const
{
    for (const auto &wrapper : m_wrappers) {
        if (wrapper && wrapper->controlId == controlId) {
            return wrapper.get();
        }
    }
    return nullptr;
}

void SessionController::onLayoutClientGone(const QString &id, const QString &reason)
{
    if (m_layoutOwner.release(id)) {
        finishLayoutRelease(id, reason);
    }
}

void SessionController::finishLayoutRelease(const QString &id, const QString &reason)
{
    m_heartbeatTimer.stop();
    m_pongPending = false;
    qInfo().noquote() << u"layout released by %1 (%2): restoring the desk"_s.arg(id, reason);
    if (auto *former = wrapperFor(id); former && former->layoutClient) {
        // A former owner that is still connected (heartbeat loss) keeps
        // streaming the restored layout: a viewer, not nobody.
        m_layoutOwner.addViewer(id);
    }
    m_layoutExecutor.releaseAll();
    // Whoever is left on the channel now looks at the restored layout, with
    // no owner.
    describeLayoutClients(nullptr);
    updateRestoreAction();
}

void SessionController::onHeartbeatTick()
{
    const QString owner = m_layoutOwner.owner();
    if (owner.isEmpty()) {
        m_heartbeatTimer.stop();
        return;
    }
    auto *wrapper = wrapperFor(owner);
    if (!wrapper || !wrapper->connection || wrapper->connection->state() == KRdp::RdpConnection::State::Closed) {
        // Its destruction releases; nothing to ping meanwhile.
        return;
    }
    if (m_pongPending) {
        if (m_layoutOwner.heartbeatMissed(owner)) {
            qWarning().noquote() << u"layout owner %1 missed %2 heartbeats"_s.arg(owner).arg(LayoutOwner::MaxMissedHeartbeats);
            finishLayoutRelease(owner, u"heartbeat"_s);
            return;
        }
        qInfo().noquote() << u"layout owner %1 missed a heartbeat (%2 of %3)"_s.arg(owner).arg(m_layoutOwner.missedHeartbeats()).arg(LayoutOwner::MaxMissedHeartbeats);
    }
    wrapper->connection->sendControlRecord(QJsonObject{{u"type"_s, u"ping"_s}});
    m_pongPending = true;
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
