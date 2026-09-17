// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "DisplayWakeGuard.h"
#include "RdpConnection.h"
#include "MultiLayout.h"
#include <AbstractSession.h>
#include <KStatusNotifierItem>
#include <SurfaceLayout.h>
#include <optional>
#include <vector>

#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVector>

class QScreen;

namespace KRdp
{
class AbstractSession;
class Server;
class RdpConnection;
}

class SessionWrapper;

class SessionController : public QObject
{
    Q_OBJECT
public:
    enum class SessionType {
        Portal,
        Plasma,
    };

    /**
     * The monitor layout a connection's sessions are built for.
     *
     * Everything in it is empty/default in every mode but `multi`, which is
     * how the wrapper tells the two apart.
     */
    struct MonitorLayout {
        /** One entry per RDPGFX surface, in KWin-global PIXEL coordinates. */
        QVector<KRdp::VideoMonitor> monitors;
        /** Connector name of the screen behind each entry, for logging. */
        QStringList names;
        /** Pixels per logical unit, for turning RDP positions back into input. */
        qreal scale = 1.0;

        bool isEmpty() const
        {
            return monitors.isEmpty();
        }
    };

    SessionController(KRdp::Server *server, SessionType sessionType);
    ~SessionController() override;

    void setVirtualMonitor(const KRdp::VirtualMonitor &vm);
    /**
     * Which monitor to capture in `primary`/`specific` mode, as an index into
     * QGuiApplication::screens(); `std::nullopt` streams the whole workspace.
     *
     * Not to be confused with KRdp::AbstractSession::setMonitorIndex(), which
     * is the RDPGFX surface index stamped on the frames a session emits. In
     * `MonitorMode=multi` this value is only the fallback target used when
     * multi-monitor streaming turns out not to be usable.
     */
    void setMonitorIndex(const std::optional<int> &index);
    /**
     * Turn `MonitorMode=multi` on or off.
     *
     * When enabled, every connection gets one session (and so one capture
     * stream, one encoder and one RDPGFX surface) per usable monitor instead
     * of the single session every other mode uses. The request is refused —
     * and multiMonitorEnabled() stays false — when fewer than two usable
     * monitors are found, in which case the caller's monitor index applies as
     * usual.
     */
    void setMultiMonitorEnabled(bool enabled);
    /** Whether per-monitor streaming is actually in effect. */
    bool multiMonitorEnabled() const;
    /** The number of monitors (and so surfaces) multi mode uses; 0 when off. */
    int multiMonitorCount() const;
    void setQuality(const std::optional<int> &quality);
    void setAdaptiveQuality(bool enabled);
    void setWakeDisplayOnConnect(bool enabled);
    void refreshDisplayConfiguration();
    /**
     * Recompute the monitor layout and, if it changed, rebuild every live
     * connection's per-monitor sessions from scratch.
     *
     * Hot-plug v1: no per-monitor diffing, the whole session set is replaced.
     * Does nothing unless `MonitorMode=multi` was asked for.
     *
     * The work is deferred by MultiRebuildSettleMs. On a DPMS wake KWin
     * removes and re-adds every output, so the screen list passes through
     * states (one screen, a placeholder, none) that are not the topology the
     * user actually has; rebuilding on those would tear every capture stream
     * down twice while each session's own recovery is already handling the
     * churn. Hot-plug is rare enough that waiting out the churn costs nothing.
     */
    void rebuildMultiSessions();
    void setSNIStatus(const KRdp::RdpConnection::State state);
    void stopFromSNI();

    /**
     * Read KWin's output config to find the connector name of the primary
     * output, or an empty string when it cannot be determined.
     */
    static QString kwinPrimaryOutputName();
    /**
     * The index into QGuiApplication::screens() of the user's primary monitor,
     * preferring KWin's own output config over Qt's idea of the primary screen.
     */
    static std::optional<int> primaryScreenIndex();
    /** Everything computeMultiLayout() read off the live screens. */
    struct MultiLayoutResult {
        /** The surfaces to stream; empty when multi mode is not usable. */
        MonitorLayout layout;
        /** QGuiApplication::screens() index behind each entry of \c layout. */
        QVector<int> streamIndices;
        /** Names of the screens that were left out, for logging. */
        QStringList dropped;
        /** Whether the kept screens disagree about their device pixel ratio. */
        bool mixedScales = false;
    };

    /**
     * Read the live screens and select the `MonitorMode=multi` layout from
     * them, returning an empty layout when fewer than \a minimumCount monitors
     * are usable.
     *
     * All this does is turn QGuiApplication::screens() into
     * KRdp::MultiLayout::ScreenInfo and call
     * KRdp::MultiLayout::selectMultiLayout(); the decision itself is that pure
     * function, which autotests/MultiLayoutTest.cpp covers without a display.
     *
     * The geometries are NOT translated: KRdp::VideoStream::setMonitorLayout()
     * owns the translation into RDP desktop space, and
     * KRdp::SurfaceLayout::originOf() inverts it for the input path.
     */
    static MultiLayoutResult computeMultiLayout(int minimumCount = KRdp::MultiLayout::MinMonitorCount);

private:
    /** What refreshMultiLayout() found. */
    enum class LayoutUpdate {
        /** Too few usable monitors; the previous state is untouched. */
        Unusable,
        /** Usable, and identical to the layout already in use. */
        Unchanged,
        /** Usable and different; the wrappers need rebuilding. */
        Changed,
    };

    void onNewConnection(KRdp::RdpConnection *newConnection);
    std::unique_ptr<KRdp::AbstractSession> makeSession();
    /** Create, configure and install this wrapper's session set. */
    void buildSessions(SessionWrapper *wrapper);
    /** buildSessions() for every live wrapper. */
    void rebuildSessions();
    /**
     * Recompute the layout and, when it changed, rebuild every wrapper.
     * \a topologyChange only picks which of the two log lines is used.
     */
    void applyMultiLayout(bool topologyChange);
    LayoutUpdate refreshMultiLayout();

    KRdp::Server *m_server = nullptr;
    SessionType m_sessionType;
    std::optional<int> m_monitorIndex;
    std::optional<int> m_quality;
    bool m_adaptiveQuality = true;
    std::optional<KRdp::VirtualMonitor> m_virtualMonitor;

    // MonitorMode=multi was asked for, and (m_multiMonitor) is actually in use.
    bool m_multiMonitorRequested = false;
    bool m_multiMonitor = false;
    // The surfaces multi mode streams; empty unless it is in effect.
    MonitorLayout m_layout;
    // QGuiApplication::screens() index each surface captures, same order as
    // m_layout.monitors. Not necessarily 0..N-1: unusable screens are skipped.
    QVector<int> m_streamIndices;
    // A mixed-scale workspace has no single pixels-per-logical-unit ratio;
    // warned about once rather than once per layout recomputation.
    bool m_warnedMixedScales = false;
    // Multi mode was asked for on a portal session, which cannot open one
    // capture stream per output; warned about once, not on every config reload.
    bool m_warnedPortalMulti = false;
    // The screens last left out of the layout, so the reason is logged when it
    // changes rather than on every recomputation.
    QStringList m_droppedScreens;
    // Lets the output list settle before a hot-plug rebuild; see
    // rebuildMultiSessions().
    QTimer m_multiRebuildTimer;

    std::unique_ptr<KRdp::AbstractSession> m_initializationSession;

    // Declared before m_wrappers so it outlives the wrappers that release into it.
    DisplayWakeGuard m_displayWakeGuard;

    std::vector<std::unique_ptr<SessionWrapper>> m_wrappers;

    KStatusNotifierItem *m_sni;
};
