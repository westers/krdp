// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ClientDisplayInfo.h"
#include "DisplayWakeGuard.h"
#include "MultiLayout.h"
#include "PhysicalOutputGuard.h"
#include "RdpConnection.h"
#include <AbstractSession.h>
#include <KStatusNotifierItem>
#include <SurfaceLayout.h>
#include <optional>
#include <vector>

#include <QObject>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QVector>

class QAction;
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

    /**
     * What `MonitorMode=virtual` does with the physical outputs while a client
     * is connected (OPT-041).
     */
    enum class VirtualPolicy {
        /** Switch them off for the session and put them back on disconnect. */
        Replace,
        /** Leave them alone; the virtual output sits next to them. */
        Extend,
    };
    /** How many virtual outputs `MonitorMode=virtual` creates for a client. */
    enum class VirtualLayout {
        /** One per client monitor (Phase B); one at the desktop size until then. */
        Client,
        /** Always one output, at the client's desktop size. */
        Single,
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
    /**
     * Turn `MonitorMode=virtual` on or off: every new connection gets one
     * virtual output sized to the client's desktop instead of a capture of a
     * physical one. Takes effect for the next connection; sessions that are
     * already running keep whatever mode they were built in. Refused on a
     * portal session, which cannot create outputs.
     */
    void setVirtualMode(bool enabled);
    bool virtualMode() const;
    void setVirtualPolicy(VirtualPolicy policy);
    VirtualPolicy virtualPolicy() const;
    void setVirtualLayout(VirtualLayout layout);
    VirtualLayout virtualLayout() const;
    /** The config spelling of a parsed value, for logs. */
    static QString policyName(VirtualPolicy policy);
    static QString layoutName(VirtualLayout layout);
    /** Output size used when the client advertises no usable desktop size. */
    void setVirtualFallbackSize(const QSize &size);
    /** "extend" -> Extend, anything else -> Replace. */
    static VirtualPolicy parseVirtualPolicy(const QString &text);
    /** "single" -> Single, anything else -> Client. */
    static VirtualLayout parseVirtualLayout(const QString &text);
    /** "1920x1080" -> QSize(1920, 1080); nullopt when malformed or outside ClientDisplay::usable(). */
    static std::optional<QSize> parseSize(const QString &text);
    /**
     * Give the physical outputs back to the console right now: restore the
     * snapshot and turn every virtual session that replaced them into an
     * extend session for the rest of its life (its virtual output is parked
     * beside the physical desktop and keeps streaming).
     *
     * The console takeover (Task 6c), triggered by local pointer motion seen
     * in the screencast's cursor metadata (SessionWrapper's
     * KRdp::Takeover::Detector), by the tray's "Restore my monitors" action
     * and by its global shortcut (Meta+Ctrl+Alt+R). Idempotent.
     */
    void releasePhysicalOutputs();
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
    /** Whether any virtual wrapper currently holds the physical layout replaced. */
    bool physicalLayoutOwned() const;
    /** Enable "Restore my monitors" exactly while there is something to give back. */
    void updateRestoreAction();
    /** Create, configure and install this wrapper's session set. */
    void buildSessions(SessionWrapper *wrapper);
    /**
     * `MonitorMode=virtual`: build the wrapper's session set from the display
     * the client advertised. Runs once the connection's capabilities exchange
     * has delivered that information, not on connect.
     */
    void buildVirtualSessions(SessionWrapper *wrapper);
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

    // MonitorMode=virtual (OPT-041); see setVirtualMode().
    bool m_virtualMode = false;
    VirtualPolicy m_virtualPolicy = VirtualPolicy::Replace;
    VirtualLayout m_virtualLayout = VirtualLayout::Client;
    QSize m_virtualFallbackSize{1920, 1080};
    // Virtual mode was asked for on a portal session; warned about once.
    bool m_warnedPortalVirtual = false;

    std::unique_ptr<KRdp::AbstractSession> m_initializationSession;

    // Declared before m_wrappers so it outlives the wrappers that release into it.
    DisplayWakeGuard m_displayWakeGuard;
    // Same rule: the wrappers restore the physical outputs through it, so it
    // has to be alive when they are torn down (see ~SessionController()).
    PhysicalOutputGuard m_outputGuard;

    std::vector<std::unique_ptr<SessionWrapper>> m_wrappers;

    KStatusNotifierItem *m_sni;
    // "Restore my monitors" (tray entry and global shortcut); owned by the
    // SNI's menu.
    QAction *m_restoreAction = nullptr;
};
