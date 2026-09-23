// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ClientDisplayInfo.h"
#include "DisplayWakeGuard.h"
#include "HostLayoutExecutor.h"
#include "LayoutControl.h"
#include "LayoutOwner.h"
#include "MultiLayout.h"
#include "PhysicalOutputGuard.h"
#include "RdpConnection.h"
#include "VideoCodecSupport.h"
#include <AbstractSession.h>
#include <KStatusNotifierItem>
#include <SurfaceLayout.h>
#include <optional>
#include <vector>

#include <QJsonObject>
#include <QObject>
#include <QPointer>
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
        /** One entry per RDPGFX surface, in a non-overlapping pixel atlas. */
        QVector<KRdp::VideoMonitor> monitors;
        /** Connector name of the screen behind each entry, for logging. */
        QStringList names;
        /** Pixels per logical unit, for turning RDP positions back into input. */
        qreal scale = 1.0;
        /**
         * Per-entry pixels per logical unit, parallel to `monitors`, for a
         * layout whose monitors are at different scales (a `KRDPCTL` layout
         * with a 125 % virtual monitor beside 100 % real ones, OPT-044):
         * each entry's wire geometry is paired with the KWin logical origin
         * in logicalOrigins; input maps through that same entry.
         * Empty for the uniform-scale `multi` layout, where `scale` applies
         * to the whole desktop.
         */
        QVector<qreal> scales;
        /** KWin compositor origins, parallel to the projected RDP surfaces. */
        QVector<QPoint> logicalOrigins;

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
        /**
         * One per client monitor, mirroring the client's own layout (Phase B);
         * one at the desktop size when the client advertises fewer than two
         * usable monitors, or when one of the requested outputs never appears.
         */
        Client,
        /** Always one output, at the client's desktop size. */
        Single,
        /**
         * One per enabled physical output, in the physical layout's own
         * positions and priority order, ignoring the client's own monitor
         * list (OPT-041 S5): what a third-party `/multimon` client needs to
         * see Steve's own desktop arrangement instead of guessing from its
         * own screens. Falls back to Client's rules (and so Client's own
         * fallback to Single) when there is no physical-output snapshot to
         * mirror - a portal session, or a failed `kscreen-doctor -j`.
         */
        Physical,
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
    /** "single" -> Single, "physical" -> Physical, anything else -> Client (case-insensitive). */
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
    void setAudioPriorityDefault(bool enabled);
    void setCodecPreference(KRdp::CodecPreference preference);
    KRdp::CodecPreference codecPreference() const;
    /**
     * AVC444 aux-stream timing default (OPT-045b, design §10 A10.2): `krdpserverrc`'s
     * `Avc444MotionGapMs`/`Avc444RestMs`/`Avc444MaxGapMs`, already validated by the caller. Applies to
     * the next connection only - an active connection keeps whatever policy it already has (the
     * default it started with, or a client's own `chroma` override), exactly like setCodecPreference().
     */
    void setChromaPolicyDefaults(const KRdp::ChromaPolicy &policy);
    KRdp::ChromaPolicy chromaPolicyDefaults() const;
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
    /**
     * The connection's capabilities exchange is in: build the session set
     * for the configured MonitorMode, unless the client joined `KRDPCTL`, in
     * which case nothing is built until its first record (or the 3 s
     * timeout); see onControlRecord() / onControlTimeout().
     */
    void onClientDisplayInfo(SessionWrapper *wrapper);
    /** One `KRDPCTL` record from the wrapper's client (queued from the session thread). */
    void onControlRecord(SessionWrapper *wrapper, const QJsonObject &record);
    /** The `KRDPCTL` client sent nothing in time: configured MonitorMode. */
    void onControlTimeout(SessionWrapper *wrapper);
    /** buildSessions() or buildVirtualSessions(), whichever the configuration asks for. */
    void buildConfiguredSessions(SessionWrapper *wrapper);
    /**
     * `KRDPCTL` `apply` (OPT-044): owner check, plan, execute; the reply and
     * the session build follow in onLayoutApplied(). \a first: this is the
     * record the session build gate was waiting for.
     */
    void onControlApply(SessionWrapper *wrapper, const QJsonObject &record, bool first);
    /**
     * `KRDPCTL` `attach` starts a read-only capture of the physical console.
     * Unlike `apply`, it never creates virtual outputs or changes the host
     * layout.  It is deliberately an explicit client request rather than a
     * consequence of media/codec preflight records.
     */
    void onControlAttach(SessionWrapper *wrapper, const QJsonObject &record, bool first);
    /**
     * `KRDPCTL` `chroma` (OPT-045b, design §10 A10.4): merges the request's present fields over
     * \a wrapper's current AVC444 aux-stream policy, validates the result, and on success applies it
     * to that connection's own sessions only - it never touches layout ownership. \a first: this is
     * the record the session build gate was waiting for (same meaning as onControlApply()'s).
     */
    void onControlChroma(SessionWrapper *wrapper, const QJsonObject &record, bool first);
    void onControlCodec(SessionWrapper *wrapper, const QJsonObject &record);
    void onControlMedia(SessionWrapper *wrapper, const QJsonObject &record);
    /** The executor finished the apply that m_applying started. */
    void onLayoutApplied(const HostLayoutExecutor::Result &result);
    /**
     * Build (or rebuild) \a wrapper's session set from \a layout: one
     * PlasmaScreencastV1Session per host monitor, capturing the KWin output
     * that carries it (the connector, its stand-in, the virtual output),
     * with the RDP layout the monitors' rects and the primary as the layout
     * says. Marks the wrapper a layout client. Monitors whose output is not
     * a QScreen yet are left out and logged.
     *
     * A rebuild is a diff (KRdp::LayoutSessions::diff): a session over an
     * output the new layout still streams from keeps running, one over an
     * output that is gone (or that this apply removed or created, per
     * \a changedOutputs) is dropped, and a session is created for every
     * output that has none. Returns whether a ResetGraphics is expected to
     * follow (the RDP layout changed), which is when the client's `layout`
     * record must wait for it.
     */
    bool buildLayoutSessions(SessionWrapper *wrapper, const KRdp::LayoutControl::Layout &layout, bool retrying = false, const QStringList &changedOutputs = {});
    /** A channel client that does not own the layout: viewer sessions from the current layout plus its `layout` record. */
    void buildAsViewer(SessionWrapper *wrapper);
    /** Rebuild (diff) every layout client but \a except from the current layout and send each its `layout`. */
    void describeLayoutClients(SessionWrapper *except, const QStringList &changedOutputs = {});
    /**
     * Desk takeover for a layout client (OPT-044 §4): arm the wrapper's
     * console takeover detector while \a layout keeps a real monitor dark,
     * latch it otherwise. Called at the end of every build; the detector is
     * replaced only when \a sessionsChanged or the dark/lit state flipped,
     * kept (references dropped if an apply just ended) otherwise.
     */
    void armLayoutTakeover(SessionWrapper *wrapper, const KRdp::LayoutControl::Layout &layout, bool sessionsChanged);
    /** The current layout from \a wrapper's point of view (owner and `you` filled in). */
    KRdp::LayoutControl::Layout layoutFor(const SessionWrapper *wrapper) const;
    /**
     * Send \a wrapper its `layout` (or, with \a takeover, `takeover`) record:
     * owed until its ResetGraphics is out (or the fallback) when
     * \a afterReset says one is coming, right away otherwise.
     */
    void sendLayout(SessionWrapper *wrapper, bool afterReset, bool takeover = false);
    void sendLayoutNow(SessionWrapper *wrapper, bool takeover);
    /** \a id (a layout client) is gone or forfeited the layout: release if it owned, then re-describe the rest. */
    void onLayoutClientGone(const QString &id, const QString &reason);
    /** The owner is released (by whoever decided it): restore the desk and re-describe the remaining layout clients. */
    void finishLayoutRelease(const QString &id, const QString &reason);
    void onHeartbeatTick();
    SessionWrapper *wrapperFor(const QString &controlId) const;
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
    /**
     * Wire one virtual session's lifecycle signals to the wrapper's policy
     * application (started / stream active / geometry resolved) and to the
     * unresolved-output fallback. Shared by the single- and multi-output
     * builds; the connections die with the session object on purpose.
     */
    void connectVirtualSession(KRdp::AbstractSession *session, SessionWrapper *wrapper);
    /**
     * A multi-output virtual build had an output that never appeared: drop
     * the whole set and build the wrapper again as one output at the
     * client's desktop size (the Phase A path). Once per wrapper.
     */
    void rebuildAsSingleVirtual(SessionWrapper *wrapper);
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
    bool m_audioPriorityDefault = false;
    KRdp::CodecPreference m_codecPreference = KRdp::CodecPreference::Auto;
    // AVC444 aux-stream timing default (OPT-045b, design §10 A10.2): applied to every new
    // connection's sessions; a client's own KRDPCTL `chroma` can override it for that connection.
    KRdp::ChromaPolicy m_chromaPolicyDefault;
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
    // KRDPCTL layout control (OPT-044). The executor holds the layout's
    // virtual outputs and restores the physical ones through m_outputGuard,
    // so it lives between the guard and the wrappers: torn down after the
    // wrappers (whose sessions stream its outputs), before the guard.
    LayoutOwner m_layoutOwner;
    HostLayoutExecutor m_layoutExecutor;
    // The heartbeat to the owner: a `ping` every tick; a tick that finds the
    // previous one unanswered counts a miss (LayoutOwner releases at 3).
    QTimer m_heartbeatTimer;
    bool m_pongPending = false;
    // The apply in flight: whose it is.
    QPointer<SessionWrapper> m_applying;
    int m_connectionCounter = 0;

    std::vector<std::unique_ptr<SessionWrapper>> m_wrappers;

    KStatusNotifierItem *m_sni;
    // "Restore my monitors" (tray entry and global shortcut); owned by the
    // SNI's menu.
    QAction *m_restoreAction = nullptr;
};
