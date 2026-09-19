// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <AbstractSession.h>

#include "LayoutControl.h"
#include "OutputSnapshot.h"
#include "PhysicalOutputGuard.h"

/**
 * Turns a LayoutControl::Plan into KWin's output set (slice 2c, OPT-044,
 * design §4 "Executor").
 *
 * The executor owns every output the layout adds to the host: a *stand-in*
 * (a virtual output at a real monitor's position, at the size and scale the
 * client asked for, or at the real one's native size when the real monitor
 * is merely dark - KWin cannot render, let alone screencast, a disabled
 * output, so "dark at the desk, still streamed" is only ever a virtual
 * output standing in for it, exactly what `MonitorMode=virtual`'s replace
 * policy does) and every *extra* virtual monitor. Each is held by its own
 * creator session (KRdp::AbstractSession::setVirtualMonitor(), never
 * streamed), so the output lives exactly as long as this table says and
 * any connection - owner or viewer - streams it as an ordinary output by
 * name. The physical outputs are changed through PhysicalOutputGuard, whose
 * snapshot (taken before the first virtual output is created) is what
 * releaseAll() restores.
 *
 * execute() is asynchronous: KWin creates a virtual output some time after
 * it is requested, and the Wayland events that make it a QScreen need the
 * event loop. The one blocking part is the guard's kscreen-doctor call and
 * its settle, as everywhere else. finished() carries the layout as KWin
 * reports it afterwards.
 */
class HostLayoutExecutor : public QObject
{
    Q_OBJECT
public:
    using SessionFactory = std::function<std::unique_ptr<KRdp::AbstractSession>()>;

    /** One output the executor created and holds. */
    struct VirtualOutput {
        /** The HostMonitor it stands for: a real monitor's id for a stand-in, "virtual-<n>" otherwise. */
        QString monitorId;
        /** KWin's name for it, prefix included ("Virtual-krdp-si-DP-1-1920x1080-s100"). */
        QString name;
        /** The connection whose apply created it. */
        QString owner;
        QSize size;
        qreal scale = 1.0;
        /** Where the arrangement put it (KWin logical position). */
        QPoint position;
        bool standIn = false;
    };

    struct Result {
        QString requester;
        /** The host layout as KWin reports it after the apply; owner/you unset. */
        KRdp::LayoutControl::Layout layout;
        /** KWin output names created and removed by this apply (Task 4 diffs on these). */
        QStringList created;
        QStringList removed;
        /** Set when the apply did not fully land; `layout` is still what is there. */
        std::optional<KRdp::LayoutControl::Error> error;
    };

    HostLayoutExecutor(PhysicalOutputGuard *guard, SessionFactory sessionFactory, QObject *parent = nullptr);
    ~HostLayoutExecutor() override;

    /**
     * The host layout right now, owner/you unset: a fresh read of KWin's
     * outputs while nothing is controlled, the applied layout (positions
     * re-read from KWin) while something is.
     */
    KRdp::LayoutControl::Layout current() const;
    /** Whether anything is held: the guard, or a virtual output. */
    bool controlling() const;
    /** An execute() is in flight. */
    bool busy() const;
    /**
     * Start applying \a plan on behalf of \a requester. An immediate refusal
     * (busy, no kscreen-doctor, the guard cannot take the snapshot) comes
     * back as the error and nothing else happens; otherwise finished()
     * follows, possibly from inside this call when there is nothing to wait
     * for. Every action is logged once.
     */
    std::optional<KRdp::LayoutControl::Error> execute(const KRdp::LayoutControl::Plan &plan, const QString &requester);
    /**
     * Give the desk back: the physical outputs restored through the guard
     * (the virtual outputs parked beside them first, so KWin never records
     * an overlap), then every virtual output removed. Blocks for the
     * restore's settle. An apply in flight is abandoned first. Idempotent.
     */
    void releaseAll();
    QList<VirtualOutput> virtualOutputs() const;
    /**
     * The KWin output a host monitor of the current layout is streamed
     * from: the connector for a lit real monitor, its stand-in's name for a
     * dark or stood-in one, the virtual output's name for a virtual monitor.
     * Empty when \a monitorId is not in the layout.
     */
    QString outputNameFor(const QString &monitorId) const;

Q_SIGNALS:
    void finished(const HostLayoutExecutor::Result &result);
    /** The host layout changed (an apply landed, or everything was released). */
    void layoutChanged();

private:
    struct Creator {
        VirtualOutput record;
        std::unique_ptr<KRdp::AbstractSession> session;
    };
    /** Everything one execute() carries until finished(). */
    struct Pending {
        QString requester;
        KRdp::LayoutControl::Layout target;
        QList<KRdp::OutputSnapshot::Arrangement> arrangement;
        /** Outputs being created; moved into m_outputs once the arrangement is applied. */
        std::vector<Creator> creating;
        /** Names of outputs to remove once the arrangement (which parks them) is applied. */
        QStringList removing;
        /** KWin output names the built sessions will need as QScreens. */
        QStringList neededScreens;
        QStringList created;
        QStringList removed;
        /** False when the apply restates the layout as it is: no kscreen-doctor call, nothing created. */
        bool needsArrangement = false;
        bool arranged = false;
        /** An abort is queued; progress checks stand down. */
        bool abortScheduled = false;
        QElapsedTimer screenClock;
        qint64 screensGoodSince = -1;
    };

    static QString standInName(const QString &realId, const QSize &size, qreal scale);
    static QString virtualName(const QString &virtualId, const QSize &size, qreal scale);
    /** The stand-in size/scale a real monitor of \a monitor presents: its own when dark, the stand-in's when stood in. */
    static QSize presentedSize(const KRdp::LayoutControl::HostMonitor &monitor);
    static qreal presentedScale(const KRdp::LayoutControl::HostMonitor &monitor);
    /** Fresh read of KWin's real outputs as host monitors (the idle layout). */
    static QList<KRdp::LayoutControl::HostMonitor> readRealMonitors();
    /** Where virtual outputs go beside the physical desktop while the physical outputs are on. */
    QPoint extendAnchor() const;
    /** Every created output has resolved (or one failed): run the arrangement. */
    void onCreatorProgress();
    /** Queue onCreatorProgress() / abortPending() for the event loop, never from inside a session's signal. */
    void scheduleProgress();
    void scheduleAbort(const KRdp::LayoutControl::Error &error);
    void abortPending(const KRdp::LayoutControl::Error &error);
    /** Poll QGuiApplication::screens() for the outputs the sessions will need. */
    void pollScreens();
    /** Patch \a layout's positions from a fresh read of KWin's outputs. */
    void refreshPositions(KRdp::LayoutControl::Layout &layout) const;
    void finish(std::optional<KRdp::LayoutControl::Error> error);
    void destroyOutputs(const QStringList &names);

    PhysicalOutputGuard *m_guard;
    SessionFactory m_sessionFactory;
    std::vector<Creator> m_outputs;
    /** The layout as last applied; meaningful while controlling(). */
    KRdp::LayoutControl::Layout m_layout;
    std::optional<Pending> m_pending;
    QTimer m_screenTimer;
};
