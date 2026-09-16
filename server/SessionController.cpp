// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "SessionController.h"

#include <QAction>
#include <QCoreApplication>
#include <QDBusInterface>
#include <QDebug>
#include <QMenu>

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

class SessionWrapper : public QObject
{
    Q_OBJECT
public:
    SessionWrapper(KRdp::RdpConnection *conn, std::unique_ptr<KRdp::AbstractSession> &&sess, KStatusNotifierItem *sni, DisplayWakeGuard *displayWakeGuard)
        : session(std::move(sess))
        , connection(conn)
        , m_displayWakeGuard(displayWakeGuard)
    {
        m_sni = sni;

        connect(session.get(), &KRdp::AbstractSession::frameReceived, connection->videoStream(), &KRdp::VideoStream::queueFrame);
        connect(session.get(), &KRdp::AbstractSession::cursorUpdate, this, &SessionWrapper::onCursorUpdate);
        connect(session.get(), &KRdp::AbstractSession::error, this, &SessionWrapper::sessionError);
        connect(session.get(), &KRdp::AbstractSession::clipboardDataChanged, connection->clipboard(), &KRdp::Clipboard::setServerData);

        connect(connection->videoStream(), &KRdp::VideoStream::enabledChanged, this, &SessionWrapper::onVideoStreamEnabledChanged, Qt::QueuedConnection);
        connect(connection->videoStream(), &KRdp::VideoStream::requestedFrameRateChanged, this, &SessionWrapper::onRequestedFrameRateChanged, Qt::QueuedConnection);
        // Emitted from the frame submission thread; the session must act on the main thread.
        connect(connection->videoStream(), &KRdp::VideoStream::keyFrameRequested, this, &SessionWrapper::onKeyFrameRequested, Qt::QueuedConnection);
        // VideoStream (and so this signal, whether from adaptive-quality steering
        // or a cap/adaptive toggle) runs on the main thread like SessionWrapper;
        // queued here defensively, matching the other VideoStream connections above.
        connect(connection->videoStream(), &KRdp::VideoStream::requestedQualityChanged, this, &SessionWrapper::onRequestedQualityChanged, Qt::QueuedConnection);
        connect(connection->inputHandler(), &KRdp::InputHandler::inputEvent, session.get(), &KRdp::AbstractSession::sendEvent);
        connect(connection->clipboard(), &KRdp::Clipboard::clientDataChanged, session.get(), [clipboard = connection->clipboard(), this]() {
            session->setClipboardData(clipboard->getClipboard());
        }, Qt::QueuedConnection);

        connect(connection, &QObject::destroyed, this, &SessionWrapper::onConnectionDestroyed);
    }

    ~SessionWrapper() override
    {
        holdDisplayWake(false);
    }

    void onCursorUpdate(const PipeWireCursor &cursor)
    {
        if (!connection) {
            return;
        }

        KRdp::Cursor::CursorUpdate update;
        update.hotspot = cursor.hotspot;
        update.image = cursor.texture;
        connection->cursor()->update(update);
    }

    void onVideoStreamEnabledChanged()
    {
        if (connection->videoStream()->enabled()) {
            session->requestStreamingEnable(connection->videoStream());
            holdDisplayWake(true);
        } else {
            session->requestStreamingDisable(connection->videoStream());
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
        session->setVideoFrameRate(connection->videoStream()->requestedFrameRate());
    }

    void onKeyFrameRequested()
    {
        session->requestKeyFrame();
    }

    void onRequestedQualityChanged(quint8 quality)
    {
        session->setVideoQuality(quality);
    }

    void onConnectionDestroyed()
    {
        Q_EMIT connectionDestroyed(this);
    }

    Q_SIGNAL void sessionError();
    Q_SIGNAL void connectionDestroyed(SessionWrapper *wrapper);

    std::unique_ptr<KRdp::AbstractSession> session;
    QPointer<KRdp::RdpConnection> connection;
    KStatusNotifierItem *m_sni;
    DisplayWakeGuard *m_displayWakeGuard;
    bool m_holdsDisplayWake = false;
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
        if (!wrapper || !wrapper->session) {
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
            wrapper->session->setVideoQuality(m_quality.value());
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

    for (const auto &wrapper : m_wrappers) {
        if (!wrapper || !wrapper->session) {
            continue;
        }

        wrapper->session->setActiveStream(m_monitorIndex.value_or(-1));
        wrapper->session->refreshDisplayConfiguration();
    }
}

void SessionController::onNewConnection(KRdp::RdpConnection *newConnection)
{
    auto wrapper = std::make_unique<SessionWrapper>(newConnection, makeSession(), m_sni, &m_displayWakeGuard);
    if (m_virtualMonitor) {
        wrapper->session->setVirtualMonitor(*m_virtualMonitor);
    } else {
        wrapper->session->setActiveStream(m_monitorIndex.value_or(-1));
    }
    if (m_quality.has_value()) {
        // See setQuality() for why the direct session call is skipped while
        // adaptive quality is on: setQualityCap() (below) is the only path
        // that should ever set the session's quality in that mode.
        if (!m_adaptiveQuality) {
            wrapper->session->setVideoQuality(m_quality.value());
        }
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
