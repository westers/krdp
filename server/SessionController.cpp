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
// The 780M's VA-API H.264 encoder tops out at a 4096x4096 surface, so a
// monitor larger than that in either direction cannot become a surface (and a
// stream) of its own.
constexpr int MaxEncodeDimension = 4096;
// RDPGFX_RESET_GRAPHICS carries at most 16 monitors and
// VideoStream::setMonitorLayout() rejects a longer layout outright.
constexpr int MaxMonitorCount = 16;
// Below this, per-monitor streaming buys nothing over the single-surface path.
constexpr int MinMultiMonitorCount = 2;
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
     * \a layout is empty in every mode but `multi`: one session, surface index
     * 0, no explicit surface layout and no coordinate translation, exactly as
     * it was before per-monitor surfaces. In `multi` it holds one entry per
     * surface in KWin-global pixel coordinates, in the same order as
     * \a newSessions, and \a layoutScale says how many of those pixels make a
     * logical unit.
     */
    void setSessions(std::vector<std::unique_ptr<KRdp::AbstractSession>> &&newSessions, const QVector<KRdp::VideoMonitor> &layout, qreal layoutScale)
    {
        if (newSessions.empty() || !connection) {
            return;
        }

        const bool hadLayout = !monitorLayout.isEmpty();
        // Destroying the previous sessions drops every connection made below
        // that has one of them as sender or as context object. The input
        // handler is wired to the wrapper itself in multi mode, so that one
        // has to go by hand.
        disconnect(connection->inputHandler(), nullptr, this, nullptr);
        sessions = std::move(newSessions);
        monitorLayout = layout;
        m_layoutScale = layoutScale > 0.0 ? layoutScale : 1.0;

        auto *videoStream = connection->videoStream();
        // The stream needs the layout before the first frame arrives: a frame
        // whose size does not match its surface is dropped. Skipped entirely
        // while no layout is or was configured, so the other modes never touch
        // this code path.
        if (!monitorLayout.isEmpty() || hadLayout) {
            videoStream->setMonitorLayout(monitorLayout);
        }

        for (const auto &entry : sessions) {
            auto *session = entry.get();
            connect(session, &KRdp::AbstractSession::frameReceived, videoStream, &KRdp::VideoStream::queueFrame);
            connect(session, &KRdp::AbstractSession::cursorUpdate, this, &SessionWrapper::onCursorUpdate);
            connect(session, &KRdp::AbstractSession::error, this, &SessionWrapper::sessionError);
            connect(session, &KRdp::AbstractSession::clipboardDataChanged, connection->clipboard(), &KRdp::Clipboard::setServerData);
        }

        // Input and clipboard are workspace-wide, so they go through the first
        // session whatever the mode. It is the only session in every mode but
        // multi, where fake input addresses the whole workspace anyway.
        auto *inputSession = sessions.front().get();
        if (monitorLayout.isEmpty()) {
            connect(connection->inputHandler(), &KRdp::InputHandler::inputEvent, inputSession, &KRdp::AbstractSession::sendEvent);
        } else {
            connect(connection->inputHandler(), &KRdp::InputHandler::inputEvent, this, &SessionWrapper::onInputEvent);
        }
        connect(connection->clipboard(), &KRdp::Clipboard::clientDataChanged, inputSession, [clipboard = connection->clipboard(), inputSession]() {
            inputSession->setClipboardData(clipboard->getClipboard());
        }, Qt::QueuedConnection);

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
     * Multi-monitor input: one pointer position for the whole RDP desktop,
     * injected through a single session.
     */
    void onInputEvent(const std::shared_ptr<QEvent> &event)
    {
        if (sessions.empty()) {
            return;
        }

        // Only pointer motion carries a position that reaches the compositor;
        // buttons, wheel and keys are injected without one (see
        // PlasmaScreencastV1Session::injectNonMotionEvent()).
        if (event->type() == QEvent::MouseMove) {
            const auto mouseEvent = std::static_pointer_cast<QMouseEvent>(event);
            // RDP desktop space -> KWin-global pixels -> KWin-global logical,
            // which is what org_kde_kwin_fake_input's pointer_motion_absolute
            // takes. originOf() is the exact inverse of the translation
            // VideoStream::setMonitorLayout() applied to this same layout.
            const QPointF position = (mouseEvent->position() + QPointF(KRdp::SurfaceLayout::originOf(monitorLayout))) / m_layoutScale;
            auto translated = std::make_shared<QMouseEvent>(QEvent::MouseMove,
                                                            position,
                                                            position,
                                                            position,
                                                            mouseEvent->button(),
                                                            mouseEvent->buttons(),
                                                            mouseEvent->modifiers());
            sessions.front()->sendGlobalEvent(translated);
            return;
        }

        sessions.front()->sendGlobalEvent(event);
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
    QVector<KRdp::VideoMonitor> monitorLayout;
    QPointer<KRdp::RdpConnection> connection;
    KStatusNotifierItem *m_sni;
    DisplayWakeGuard *m_displayWakeGuard;
    bool m_holdsDisplayWake = false;
    qreal m_layoutScale = 1.0;
    std::optional<quint8> m_requestedQuality;
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
    const bool wasRequested = m_multiMonitorRequested;
    m_multiMonitorRequested = enabled;

    if (!enabled) {
        m_multiRebuildTimer.stop();
        if (!m_multiMonitor) {
            return;
        }
        m_multiMonitor = false;
        m_monitorLayout.clear();
        m_streamIndices.clear();
        m_layoutScale = 1.0;
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
    return m_multiMonitor ? int(m_monitorLayout.size()) : 0;
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
                             .arg(m_monitorLayout.size())
                             .arg(layoutSummary(m_monitorLayout));
}

SessionController::LayoutUpdate SessionController::refreshMultiLayout()
{
    QVector<QScreen *> usableScreens;
    auto layout = computeMultiLayout(usableScreens);

    // Getting into multi mode is only worth it with two monitors. Once it is
    // running, a topology change that leaves a single usable monitor keeps a
    // single surface rather than silently moving a live client onto the
    // other code path; only losing every monitor - which the output churn on
    // a DPMS wake does transiently - leaves the previous layout alone.
    const qsizetype minimum = m_multiMonitor ? 1 : MinMultiMonitorCount;
    if (layout.size() < minimum) {
        return LayoutUpdate::Unusable;
    }

    const auto screens = QGuiApplication::screens();
    QVector<int> streamIndices;
    streamIndices.reserve(usableScreens.size());
    for (auto *screen : std::as_const(usableScreens)) {
        streamIndices.push_back(int(screens.indexOf(screen)));
    }

    // The layout is in pixels while fake input takes logical coordinates, so
    // the input path needs the ratio between them. Every screen on this box is
    // at scale 1; a mixed-scale workspace has no single ratio, so the primary's
    // is used and the rest are approximated.
    qreal scale = 1.0;
    for (qsizetype i = 0; i < layout.size(); ++i) {
        if (layout.at(i).primary) {
            scale = usableScreens.at(i)->devicePixelRatio();
            break;
        }
    }
    const bool mixedScales = std::any_of(usableScreens.cbegin(), usableScreens.cend(), [scale](const QScreen *screen) {
        return !qFuzzyCompare(screen->devicePixelRatio(), scale);
    });
    if (mixedScales && !m_warnedMixedScales) {
        m_warnedMixedScales = true;
        qWarning() << "MonitorMode=multi with monitors at different scales; pointer positions use the primary's scale" << scale;
    }

    if (m_multiMonitor && m_monitorLayout == layout && m_streamIndices == streamIndices && qFuzzyCompare(m_layoutScale, scale)) {
        return LayoutUpdate::Unchanged;
    }

    m_monitorLayout = layout;
    m_streamIndices = streamIndices;
    m_layoutScale = scale;
    return LayoutUpdate::Changed;
}

QVector<KRdp::VideoMonitor> SessionController::computeMultiLayout(QVector<QScreen *> &orderedScreens)
{
    orderedScreens.clear();

    QVector<KRdp::VideoMonitor> layout;
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return layout;
    }

    const auto primaryIndex = primaryScreenIndex();
    const QScreen *primary = primaryIndex.has_value() && *primaryIndex >= 0 && *primaryIndex < screens.size() ? screens.at(*primaryIndex) : nullptr;

    for (auto *screen : screens) {
        if (layout.size() >= MaxMonitorCount) {
            qWarning() << "More than" << MaxMonitorCount << "monitors; leaving" << screen->name() << "out of MonitorMode=multi";
            continue;
        }

        const auto logicalGeometry = screen->geometry();
        if (logicalGeometry.isEmpty()) {
            // KWin removes and re-adds every output on a DPMS wake, so a screen
            // can briefly have no geometry. An empty rect would make
            // VideoStream::setMonitorLayout() reject the whole layout, so skip
            // the screen; the next topology change brings it back.
            qDebug() << "Skipping screen" << screen->name() << "with an empty geometry";
            continue;
        }

        const qreal scale = screen->devicePixelRatio();
        const QRect pixelGeometry(QPoint(qRound(logicalGeometry.x() * scale), qRound(logicalGeometry.y() * scale)),
                                  QSize(qRound(logicalGeometry.width() * scale), qRound(logicalGeometry.height() * scale)));

        if (pixelGeometry.width() > MaxEncodeDimension || pixelGeometry.height() > MaxEncodeDimension) {
            qWarning() << "Screen" << screen->name() << pixelGeometry.size() << "is past the" << MaxEncodeDimension
                       << "px VA-API encode limit; leaving it out of MonitorMode=multi";
            continue;
        }

        orderedScreens.push_back(screen);
        layout.push_back(KRdp::VideoMonitor{
            .geometry = pixelGeometry,
            .primary = (screen == primary),
        });
    }

    // setMonitorLayout() wants exactly one primary, and the configured one can
    // have been left out above.
    const auto primaryCount = std::count_if(layout.cbegin(), layout.cend(), [](const KRdp::VideoMonitor &monitor) {
        return monitor.primary;
    });
    if (primaryCount != 1 && !layout.isEmpty()) {
        for (auto &monitor : layout) {
            monitor.primary = false;
        }
        layout.first().primary = true;
    }

    return layout;
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
    QVector<KRdp::VideoMonitor> layout;

    if (m_multiMonitor && !m_monitorLayout.isEmpty()) {
        layout = m_monitorLayout;
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

    wrapper->setSessions(std::move(sessions), layout, m_layoutScale);
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
