// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "SessionController.h"

#include <algorithm>

#include <QAction>
#include <QCoreApplication>
#include <QDBusInterface>
#include <QDebug>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>

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

#include "VideoStream.h"

using namespace Qt::StringLiterals;

namespace
{
// How long the output list has to hold still before a hot-plug rebuild; see
// SessionController::rebuildMultiSessions(). Comfortably longer than the
// 750 ms settle PlasmaScreencastV1Session's own stream recovery uses, so the
// output churn on a DPMS wake is over before the layout is read.
constexpr int MultiRebuildSettleMs = 2000;

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
        holdDisplayWake(false);
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
        if (!multi) {
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

    // One entry in every mode but MonitorMode=multi, where there is one per
    // monitor and the index into this vector is the RDPGFX surface index.
    std::vector<std::unique_ptr<KRdp::AbstractSession>> sessions;
    // Empty unless multi-monitor streaming is in effect; see setSessions().
    SessionController::MonitorLayout layout;
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

    qInfo() << "Applied runtime quality update:" << m_quality.value() << "active sessions:" << m_wrappers.size();
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

    qInfo() << "Applied runtime adaptive quality update:" << m_adaptiveQuality << "active sessions:" << m_wrappers.size();
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

    if (m_multiMonitorRequested) {
        // Also the path back into multi mode after it was refused for want of
        // a second monitor: a screen plugged in later gets another chance here.
        rebuildMultiSessions();
        if (m_multiMonitor) {
            return;
        }
    }

    for (const auto &wrapper : m_wrappers) {
        if (!wrapper || wrapper->sessions.empty()) {
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

void SessionController::rebuildSessions()
{
    for (const auto &wrapper : m_wrappers) {
        if (!wrapper || !wrapper->connection) {
            continue;
        }
        buildSessions(wrapper.get());
    }
}

void SessionController::onNewConnection(KRdp::RdpConnection *newConnection)
{
    auto wrapper = std::make_unique<SessionWrapper>(newConnection, m_sni, &m_displayWakeGuard);
    buildSessions(wrapper.get());
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
    });

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
