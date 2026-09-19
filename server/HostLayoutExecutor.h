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

#include "LayoutArrangement.h"
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
 * releaseAll() restores. What to create, remove and where everything goes
 * is LayoutArrangement::derive(), pure and tested; this class does the
 * waiting and the talking to KWin.
 *
 * execute() is asynchronous: KWin creates a virtual output some time after
 * it is requested, and the Wayland events that make it a QScreen need the
 * event loop. Before the first output of an apply is requested the displays
 * are woken (through the wake hook) and the physical outputs waited for, so
 * no output is ever created into the remove-and-re-add churn a panel coming
 * out of standby causes. The outputs are then created ONE AT A TIME: the
 * next creator is started only once the previous output has resolved and a
 * settle of CreationGapMs has passed (OPT-047 mitigation 1 - KWin 6.6.6
 * crashed in its DRM backend, `DrmGpu::pageFlipHandler`, when three were
 * requested back to back ~5 s after a release's churn, 2026-09-19; every
 * appearance is an output-set change KWin answers with a re-queried
 * configuration and a modeset). Cost: an N-output creation takes about
 * N x (resolve ~0.3 s + 0.4 s settle) before the arrangement instead of
 * ~0.3 s for all of them - Private (two stand-ins) ~1.4 s, about 0.7 s more
 * than the parallel creation; the arrangement itself is unchanged. An
 * output the apply removes goes only after the arrangement, and its going
 * is followed up: KWin re-queries its remembered configuration for the new
 * output set the moment an output disappears and may replay one that
 * lights the desk, so once the removed outputs are gone the arrangement is
 * read back and re-asserted if it did not hold. The blocking parts are the
 * guard's kscreen-doctor calls and their settles, as everywhere else.
 * finished() carries the applied layout: real monitors at their snapshot
 * positions, virtual outputs at their target positions; KWin's read-back
 * positions only when the arrangement could not be verified.
 */
class HostLayoutExecutor : public QObject
{
    Q_OBJECT
public:
    using SessionFactory = std::function<std::unique_ptr<KRdp::AbstractSession>()>;
    /** Asked to wake the displays (DPMS on) before an output is created; may be empty. */
    using WakeHook = std::function<void()>;
    using VirtualOutput = KRdp::LayoutArrangement::VirtualOutput;

    struct Result {
        QString requester;
        /** The host layout after the apply (current()); owner/you unset. */
        KRdp::LayoutControl::Layout layout;
        /** KWin output names created and removed by this apply (Task 4 diffs on these). */
        QStringList created;
        QStringList removed;
        /** Set when the apply did not fully land; `layout` is still what is there. */
        std::optional<KRdp::LayoutControl::Error> error;
    };

    /**
     * \a sessionFactory creates the creator sessions; pass an EMPTY factory
     * for a session type that cannot create virtual outputs (the portal),
     * and ANY apply - one that would only light or darken real outputs
     * included - is refused `unsupported` by execute() before anything is
     * held or waited for: such a session cannot build per-output streams
     * either, so no layout could be served.
     */
    HostLayoutExecutor(PhysicalOutputGuard *guard, SessionFactory sessionFactory, WakeHook wake, QObject *parent = nullptr);
    ~HostLayoutExecutor() override;

    /**
     * The host layout right now, owner/you unset: a fresh read of KWin's
     * outputs while nothing is controlled, the applied layout while
     * something is. The applied layout's positions are the targets - a real
     * monitor's is the guard's snapshot position, a virtual output's where
     * the arrangement put it - never a fresh read: KWin's read-back is what
     * the executor checks the arrangement against and re-asserts over, and a
     * plan made from it after a replay would light a real monitor wherever
     * KWin had parked its stand-in. Only while the last apply's arrangement
     * could not be verified does this report KWin's read-back positions
     * instead (the truth for the `layout` record and for input mapping);
     * the targets themselves are never overwritten - see target().
     */
    KRdp::LayoutControl::Layout current() const;
    /**
     * What the next apply is planned from: current(), except while the
     * last apply's arrangement is unverified, when it is the applied
     * layout's targets all the same. The planner's sanitiser judges the
     * TARGET's union, never a read-back's (a replay that put a stand-in at
     * 7680,0 made a 9600 px read-back union refuse every later apply on
     * 2026-09-19, step 5), and the next apply re-asserts the targets
     * rather than build on what KWin did.
     */
    KRdp::LayoutControl::Layout target() const;
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
     * The KWin output each monitor of \a layout is streamed from, parallel
     * to `layout.monitors` (see LayoutArrangement::outputNameFor()); an
     * empty entry for a monitor whose output does not exist. No process is
     * run: the table answers.
     */
    QStringList outputNamesFor(const KRdp::LayoutControl::Layout &layout) const;

Q_SIGNALS:
    void finished(const HostLayoutExecutor::Result &result);
    /** The host layout changed (an apply landed, or everything was released). */
    void layoutChanged();
    /**
     * An apply is about to change KWin's outputs (the guard's
     * aboutToArrange(), relayed): the first arrangement, or a re-assert
     * after a removal or for a restated layout while unverified. The
     * kscreen change can warp the pointer; the controller flags every
     * layout client's takeover detector to sit the apply out from here,
     * whatever the plan's action count said (a genuine no-op never gets
     * here and costs nothing).
     */
    void arrangementStarting();

private:
    struct Creator {
        VirtualOutput record;
        std::unique_ptr<KRdp::AbstractSession> session;
    };
    /** Everything one execute() carries until finished(). */
    struct Pending {
        QString requester;
        KRdp::LayoutControl::Layout target;
        KRdp::LayoutArrangement::Derived derived;
        /** Outputs being created; moved into m_outputs once the arrangement is applied. */
        std::vector<Creator> creating;
        QStringList created;
        QStringList removed;
        /** False when the apply restates the layout as it is: no kscreen-doctor call, nothing created. */
        bool needsArrangement = false;
        /**
         * The creation sequence has begun: the first creator session has
         * been started (or there were none to start); the rest are started
         * one at a time from startNextCreator() as each output resolves.
         */
        bool creationStarted = false;
        bool arranged = false;
        /**
         * This apply's beginLayoutControl() is what took the hold (nothing
         * was held or created before it). An abort before the arrangement
         * then cancels the hold outright: the physical outputs were never
         * touched, so there is nothing to restore (re-review Minor 3).
         */
        bool tookHold = false;
        /** A creator session's start() has been called: KWin may have re-laid outputs out since, so the hold stays. */
        bool anyCreatorStarted = false;
        /** An abort is queued; progress checks stand down. */
        bool abortScheduled = false;
        QElapsedTimer wakeClock;
        /** Since the removed outputs' creator sessions were destroyed (the wait for them to be gone). */
        QElapsedTimer removalClock;
        QElapsedTimer screenClock;
        qint64 screensGoodSince = -1;
    };

    /** Fresh read of KWin's real outputs as host monitors (the idle layout). */
    static QList<KRdp::LayoutControl::HostMonitor> readRealMonitors();
    /** Where virtual outputs go beside the physical desktop while the physical outputs are on. */
    QPoint extendAnchor() const;
    /** Wake the displays if a physical output is off, wait for them, then startCreation(). */
    void prepareCreation();
    void onDpmsPoll();
    /** Settle the physical outputs, then start the first creator; the rest follow one at a time. */
    void startCreation();
    /**
     * Start the creator LayoutArrangement::nextCreatorToStart() names, if
     * any: from startCreation() for the first, from the gap timer for each
     * later one. Nothing happens while a started output is still resolving.
     */
    void startNextCreator();
    /**
     * A created output resolved (or the sequence just started): once every
     * started output has resolved, arm the gap timer for the next creator;
     * once every creator has been started and resolved, run the arrangement.
     */
    void onCreatorProgress();
    /** Queue onCreatorProgress() / abortPending() for the event loop, never from inside a session's signal. */
    void scheduleProgress();
    void scheduleAbort(const KRdp::LayoutControl::Error &error);
    void abortPending(const KRdp::LayoutControl::Error &error);
    /**
     * After the removed outputs' creator sessions are destroyed: wait, from
     * the event loop (the destroy requests need it to reach KWin), until
     * none of them is a QScreen or a kscreen output any more, then
     * reassertAfterRemoval().
     */
    void awaitRemoval();
    void onRemovalPoll();
    /** The removed outputs are gone: the guard reconciles the arrangement without them (re-asserting it when KWin replaced it). */
    void reassertAfterRemoval();
    /** Start polling for the outputs the sessions will need (the last step before finish()). */
    void startScreenWait();
    /** Poll QGuiApplication::screens() for the outputs the sessions will need. */
    void pollScreens();
    void finish(std::optional<KRdp::LayoutControl::Error> error);
    void destroyOutputs(const QStringList &names);
    /** The pending creators' progress, as the pure helpers judge it (started = has a session). */
    static QList<KRdp::LayoutArrangement::CreatorState> creatorStates(const std::vector<Creator> &creating);

    PhysicalOutputGuard *m_guard;
    SessionFactory m_sessionFactory;
    WakeHook m_wake;
    std::vector<Creator> m_outputs;
    /**
     * The layout as last applied, positions as targeted (see current());
     * meaningful while controlling(). Never patched from a read-back.
     */
    KRdp::LayoutControl::Layout m_layout;
    /**
     * The last apply's arrangement could not be verified (the guard's
     * reconcile failed): current() reports KWin's read-back over m_layout
     * until an apply verifies again or everything is released, and the
     * next apply runs the arrangement even when it restates the layout.
     */
    bool m_unverified = false;
    std::optional<Pending> m_pending;
    QTimer m_dpmsTimer;
    /** Single-shot settle between one output resolving and the next creator's start. */
    QTimer m_creationGapTimer;
    QTimer m_removalTimer;
    QTimer m_screenTimer;
};
