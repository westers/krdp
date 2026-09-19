// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "HostLayoutExecutor.h"

#include <algorithm>

#include <QDebug>
#include <QGuiApplication>
#include <QScreen>

using namespace KRdp::LayoutControl;
using namespace KRdp::LayoutArrangement;
using namespace KRdp::OutputSnapshot;
using namespace Qt::StringLiterals;

namespace
{
// Before creating an output: how often `kscreen-doctor --dpms show` is
// polled after the wake for every physical output to report "on", and how
// long at most (a panel coming out of standby answers within a second or
// two; past this the creation goes ahead behind the physical-output settle
// alone, logged). Each poll is a ~100-200 ms child process on the main
// thread, so every 500 ms rather than 250 (re-review Minor 5).
constexpr int DpmsPollMs = 500;
constexpr int DpmsWakeTimeoutMs = 6000;
// After the removed outputs' creator sessions are destroyed: how often to
// look for them to be gone (QScreens first, no process; then one kscreen
// read-back, so an output that never became a QScreen is waited for too),
// and how long at most before the arrangement is checked regardless. KWin
// applies its re-queried configuration before it withdraws the wl_output,
// so once the output is gone the read-back shows what the removal did to
// the rest (hardware finding, 2026-09-19 step 4: ~40 ms after the reset).
constexpr int RemovalPollMs = 500;
constexpr int RemovalTimeoutMs = 5000;
// After the arrangement: how often QGuiApplication::screens() is polled for
// the outputs the sessions will capture, how long they must all have been
// there (a re-enabled physical output is removed and re-added by KWin a few
// seconds after it comes back, so one sighting is not the end of it - same
// window PlasmaScreencastV1Session's own recovery settles on), and how long
// to wait at most before the apply is reported without them.
constexpr int ScreenPollMs = 250;
constexpr int ScreenStableMs = 750;
constexpr int ScreenTimeoutMs = 10000;

QString kindName(ActionKind kind)
{
    switch (kind) {
    case ActionKind::LightReal:
        return u"LightReal"_s;
    case ActionKind::DarkenReal:
        return u"DarkenReal"_s;
    case ActionKind::CreateStandIn:
        return u"CreateStandIn"_s;
    case ActionKind::RemoveStandIn:
        return u"RemoveStandIn"_s;
    case ActionKind::CreateVirtual:
        return u"CreateVirtual"_s;
    case ActionKind::RemoveVirtual:
        return u"RemoveVirtual"_s;
    }
    return u"?"_s;
}

bool screenPresent(const QString &name)
{
    const auto screens = QGuiApplication::screens();
    return std::any_of(screens.cbegin(), screens.cend(), [&name](const QScreen *screen) {
        return screen && screen->name() == name;
    });
}
}

HostLayoutExecutor::HostLayoutExecutor(PhysicalOutputGuard *guard, SessionFactory sessionFactory, WakeHook wake, QObject *parent)
    : QObject(parent)
    , m_guard(guard)
    , m_sessionFactory(std::move(sessionFactory))
    , m_wake(std::move(wake))
{
    if (m_guard) {
        connect(m_guard, &PhysicalOutputGuard::aboutToArrange, this, &HostLayoutExecutor::arrangementStarting);
    }
    m_dpmsTimer.setInterval(DpmsPollMs);
    connect(&m_dpmsTimer, &QTimer::timeout, this, &HostLayoutExecutor::onDpmsPoll);
    m_removalTimer.setInterval(RemovalPollMs);
    connect(&m_removalTimer, &QTimer::timeout, this, &HostLayoutExecutor::onRemovalPoll);
    m_screenTimer.setInterval(ScreenPollMs);
    connect(&m_screenTimer, &QTimer::timeout, this, &HostLayoutExecutor::pollScreens);
}

HostLayoutExecutor::~HostLayoutExecutor()
{
    releaseAll();
}

QList<HostMonitor> HostLayoutExecutor::readRealMonitors()
{
    QString error;
    const auto outputs = physicalOnly(PhysicalOutputGuard::readOutputs(&error));
    if (!outputs.isEmpty()) {
        return hostMonitorsFrom(outputs);
    }
    // No kscreen-doctor (a portal session, or it failed): Qt's screens are
    // the next best description. Pixel geometry, like the rest.
    qWarning().noquote() << u"KRDPCTL: could not read the outputs from kscreen-doctor (%1); describing QGuiApplication::screens() instead"_s.arg(error);
    QList<HostMonitor> monitors;
    const auto screens = QGuiApplication::screens();
    const auto *primary = QGuiApplication::primaryScreen();
    for (qsizetype i = 0; i < screens.size(); ++i) {
        const auto *screen = screens.at(i);
        if (!screen || isVirtual(screen->name())) {
            continue;
        }
        const qreal ratio = screen->devicePixelRatio();
        monitors.push_back(HostMonitor{
            .id = screen->name(),
            .name = screen->name(),
            .kind = Kind::Real,
            .size = (QSizeF(screen->size()) * ratio).toSize(),
            .position = (QPointF(screen->geometry().topLeft()) * ratio).toPoint(),
            .scale = ratio,
            .primary = primary ? screen == primary : i == 0,
            .lit = true,
        });
    }
    return monitors;
}

bool HostLayoutExecutor::controlling() const
{
    return !m_outputs.empty() || (m_guard && m_guard->layoutControlHeld());
}

bool HostLayoutExecutor::busy() const
{
    return m_pending.has_value();
}

QList<HostLayoutExecutor::VirtualOutput> HostLayoutExecutor::virtualOutputs() const
{
    QList<VirtualOutput> list;
    list.reserve(qsizetype(m_outputs.size()));
    for (const auto &creator : m_outputs) {
        list.push_back(creator.record);
    }
    return list;
}

QStringList HostLayoutExecutor::outputNamesFor(const Layout &layout) const
{
    const auto outputs = virtualOutputs();
    QStringList names;
    names.reserve(layout.monitors.size());
    for (const auto &monitor : layout.monitors) {
        names.push_back(outputNameFor(layout, outputs, monitor.id));
    }
    return names;
}

Layout HostLayoutExecutor::target() const
{
    Layout layout;
    layout.you = u"none"_s;
    if (!controlling() || m_layout.monitors.isEmpty()) {
        layout.monitors = readRealMonitors();
        return layout;
    }
    // The targets, not a read-back (see the header): a re-read here is what
    // turned KWin's replay after a removal into `LightReal DP-1 at 5120,0`
    // on 2026-09-19 (step 4). No process is run.
    layout.monitors = m_layout.monitors;
    return layout;
}

Layout HostLayoutExecutor::current() const
{
    Layout layout = target();
    if (m_unverified && controlling() && !m_layout.monitors.isEmpty()) {
        // The one state in which KWin's positions are the ones to tell: the
        // arrangement is not in place and nothing is about to put it there
        // until the next apply, so the record and the sessions' input
        // mapping follow the outputs as they are.
        layout.monitors = withReadBackPositions(layout, virtualOutputs(), PhysicalOutputGuard::readOutputs()).monitors;
    }
    return layout;
}

QPoint HostLayoutExecutor::extendAnchor() const
{
    const QRect physical = m_guard && m_guard->hasSnapshot() ? enabledUnion(m_guard->physicalOutputs()) : enabledUnion(physicalOnly(PhysicalOutputGuard::readOutputs()));
    return physical.isValid() ? QPoint(physical.left() + physical.width(), physical.top()) : QPoint(0, 0);
}

std::optional<Error> HostLayoutExecutor::execute(const Plan &plan, const QString &requester)
{
    if (busy()) {
        return Error{u"invalid"_s, u"another apply is still being applied; try again"_s};
    }
    if (!m_sessionFactory) {
        return Error{u"unsupported"_s, u"this server cannot create virtual outputs"_s};
    }

    for (const auto &action : plan.actions) {
        qInfo().noquote() << u"Layout action (%1): %2 %3 %4x%5@%6 at %7,%8"_s.arg(requester, kindName(action.kind), action.id)
                                 .arg(action.size.width())
                                 .arg(action.size.height())
                                 .arg(action.scale)
                                 .arg(action.position.x())
                                 .arg(action.position.y());
    }

    Pending pending;
    pending.requester = requester;
    pending.target = plan.resulting;
    pending.derived = derive(plan.resulting, virtualOutputs(), requester);
    // An apply that restates the layout as it is still runs the arrangement
    // when the last one could not be verified: re-sending the layout is how
    // a client asks for the targets again after a failed re-assert.
    pending.needsArrangement = !plan.actions.isEmpty() || pending.derived.physicalDisabled || !pending.derived.creating.isEmpty() || !pending.derived.removing.isEmpty() || m_unverified;

    if (pending.needsArrangement) {
        // Held before the first virtual output exists: what KWin does with
        // the physical outputs when one appears is not the layout to
        // restore. Every apply, not only the first, so a configured virtual
        // session's hold is noticed.
        if (!m_guard || !m_guard->available()) {
            return Error{u"unsupported"_s, u"layout control needs kscreen-doctor on the server"_s};
        }
        pending.tookHold = !controlling();
        if (!m_guard->beginLayoutControl()) {
            return Error{u"invalid"_s, u"the server cannot take control of the physical outputs right now (see its log)"_s};
        }
        // The applied layout's real monitors are where the snapshot has
        // them (a lit one is arranged to exactly there; a dark one's place
        // is what its stand-in takes). A plan made from the idle read a
        // moment before the snapshot agrees with it unless the desk was
        // rearranged in between; a later plan is made from m_layout, which
        // already carries these positions.
        if (alignRealMonitorsToSnapshot(pending.target, m_guard->physicalOutputs())) {
            qWarning() << "Layout control: the real monitors moved between the plan and the snapshot; planning against the snapshot positions";
            pending.derived = derive(pending.target, virtualOutputs(), requester);
        }
    }
    for (const auto &output : std::as_const(pending.derived.creating)) {
        pending.creating.push_back(Creator{output, nullptr});
    }

    m_pending = std::move(pending);

    if (m_pending->creating.empty()) {
        m_pending->creationStarted = true;
        onCreatorProgress();
        return std::nullopt;
    }
    prepareCreation();
    return std::nullopt;
}

void HostLayoutExecutor::prepareCreation()
{
    // The controller's ruling after the 2026-09-19 plasmashell stall: no
    // virtual output is ever created while a physical output is DPMS-off.
    // A panel coming out of standby makes KWin remove and re-add the
    // physical outputs, and an output created into that churn gets re-laid
    // out by KWin over a re-added one (OPT-041 finding F). So: wake first,
    // wait for DPMS to read on, then settle the physical outputs, then ask.
    const QStringList off = m_guard->dpmsOffOutputs();
    if (off.isEmpty()) {
        startCreation();
        return;
    }
    qInfo().noquote() << u"Layout control: %1 in DPMS standby; waking the displays before creating outputs"_s.arg(off.join(u", "_s));
    if (m_wake) {
        m_wake();
    }
    m_pending->wakeClock.start();
    m_dpmsTimer.start();
}

void HostLayoutExecutor::onDpmsPoll()
{
    if (!m_pending || m_pending->creationStarted) {
        m_dpmsTimer.stop();
        return;
    }
    const QStringList off = m_guard->dpmsOffOutputs();
    if (!off.isEmpty() && m_pending->wakeClock.elapsed() < DpmsWakeTimeoutMs) {
        return;
    }
    m_dpmsTimer.stop();
    if (!off.isEmpty()) {
        qWarning().noquote() << u"Layout control: %1 still not on %2 ms after the wake; creating the outputs behind the physical-output settle alone"_s.arg(off.join(u", "_s)).arg(DpmsWakeTimeoutMs);
    } else {
        qInfo() << "Layout control: displays awake after" << m_pending->wakeClock.elapsed() << "ms";
    }
    startCreation();
}

void HostLayoutExecutor::startCreation()
{
    if (!m_pending || m_pending->creationStarted) {
        return;
    }
    m_pending->creationStarted = true;

    // The churn a wake causes has to be over before an output is requested;
    // the settle inside applyArrangement() is the second guard, for a churn
    // that starts late.
    if (!m_guard->waitForPhysicalPresent()) {
        abortPending(Error{u"invalid"_s, u"the physical outputs did not settle after the display wake; nothing was created"_s});
        return;
    }

    for (auto &creator : m_pending->creating) {
        auto session = m_sessionFactory();
        if (!session) {
            abortPending(Error{u"unsupported"_s, u"this server cannot create virtual outputs"_s});
            return;
        }
        auto *raw = session.get();
        QString requested = creator.record.name;
        requested.remove(0, VirtualPrefix.size());
        raw->setVirtualMonitor(KRdp::VirtualMonitor{requested, creator.record.size, creator.record.scale});
        const QString name = creator.record.name;
        // Deferred, all three: the progress step may destroy creator
        // sessions (a failed arrangement, the removed outputs) and an abort
        // destroys the very session that is emitting, so none of it may run
        // inside a session's signal.
        connect(raw, &KRdp::AbstractSession::outputGeometryChanged, this, [this](const QRect &) {
            scheduleProgress();
        });
        connect(raw, &KRdp::AbstractSession::virtualOutputUnresolved, this, [this, name]() {
            scheduleAbort(Error{u"invalid"_s, u"virtual output %1 did not appear"_s.arg(name)});
        });
        connect(raw, &KRdp::AbstractSession::error, this, [this, name]() {
            if (m_pending && !m_pending->arranged) {
                scheduleAbort(Error{u"invalid"_s, u"virtual output %1 could not be created"_s.arg(name)});
            } else {
                qWarning() << "Creator session of virtual output" << name << "reported an error; the output may be gone";
            }
        });
        creator.session = std::move(session);
        qInfo().noquote() << u"Creating virtual output %1 (%2x%3 @%4) for %5"_s.arg(name).arg(creator.record.size.width()).arg(creator.record.size.height()).arg(creator.record.scale).arg(m_pending->requester);
        m_pending->anyCreatorStarted = true;
        raw->start();
    }
    // start() may have resolved synchronously (KWin replaying a known
    // output); the first progress check runs from the event loop either way.
    scheduleProgress();
}

void HostLayoutExecutor::scheduleProgress()
{
    QMetaObject::invokeMethod(this, &HostLayoutExecutor::onCreatorProgress, Qt::QueuedConnection);
}

void HostLayoutExecutor::scheduleAbort(const Error &error)
{
    if (!m_pending || m_pending->arranged || m_pending->abortScheduled) {
        return;
    }
    m_pending->abortScheduled = true;
    QMetaObject::invokeMethod(this, [this, error]() {
        abortPending(error);
    }, Qt::QueuedConnection);
}

void HostLayoutExecutor::onCreatorProgress()
{
    if (!m_pending || !m_pending->creationStarted || m_pending->arranged || m_pending->abortScheduled) {
        return;
    }
    const bool allResolved = std::all_of(m_pending->creating.cbegin(), m_pending->creating.cend(), [](const Creator &creator) {
        return creator.session && creator.session->outputGeometryResolved();
    });
    if (!allResolved) {
        return;
    }
    m_pending->arranged = true;

    if (!m_pending->needsArrangement) {
        // Nothing to change (an apply that restates the layout as it is):
        // it becomes the applied layout all the same, so the requester's
        // sessions are built from it like any other.
        m_layout = m_pending->target;
        finish(std::nullopt);
        return;
    }

    // The created outputs have appeared and KWin has laid the set out its
    // own way; the guard reads back and asserts the arrangement over that
    // (no call to make when KWin already has everything in place - a
    // re-apply after a takeover restored nothing, say, or a set KWin
    // remembers from an earlier apply of this very layout).
    const auto &arrangement = m_pending->derived.arrangement;
    const int createdCount = int(m_pending->creating.size());
    const QString when = createdCount > 0 ? u"after creating %1 output(s)"_s.arg(createdCount) : u"for the applied layout"_s;
    if (!m_guard->reconcileArrangement(arrangement, when)) {
        // Whatever KWin did with the command stands until release(); the
        // created outputs go away again, and the layout reported is what
        // is really there.
        for (auto &creator : m_pending->creating) {
            creator.session.reset();
        }
        m_pending->creating.clear();
        m_unverified = true;
        finish(Error{u"invalid"_s, u"the compositor did not take the requested arrangement; the layout is as read back"_s});
        return;
    }
    m_unverified = false;

    for (auto &creator : m_pending->creating) {
        m_pending->created.push_back(creator.record.name);
        m_outputs.push_back(std::move(creator));
    }
    m_pending->creating.clear();

    // Positions of what stays, as arranged (the targets: what current()
    // answers from now on, whatever KWin does with the removal below).
    for (auto &creator : m_outputs) {
        const auto entry = std::find_if(arrangement.cbegin(), arrangement.cend(), [&creator](const Arrangement &arranged) {
            return arranged.name == creator.record.name;
        });
        if (entry != arrangement.cend()) {
            creator.record.position = entry->position;
        }
    }
    m_layout = m_pending->target;

    m_pending->removed = m_pending->derived.removing;
    if (m_pending->removed.isEmpty()) {
        startScreenWait();
        return;
    }
    // The removed outputs were parked by the arrangement; now they go. Their
    // going changes KWin's output set again, and KWin answers a changed set
    // with a configuration of its own (a stored one for exactly that set,
    // or the closest stored subset - the lit desk - plus the rest appended)
    // on top of the arrangement just applied. So once they are gone the
    // guard reconciles the arrangement a second time, without them.
    destroyOutputs(m_pending->removed);
    awaitRemoval();
}

void HostLayoutExecutor::awaitRemoval()
{
    m_pending->removalClock.start();
    m_removalTimer.start();
    // The first look happens now only to be cheap when nothing is left to
    // wait for; the destroy requests reach KWin from the event loop, and the
    // kscreen read-back below is what says they did.
    onRemovalPoll();
}

void HostLayoutExecutor::onRemovalPoll()
{
    if (!m_pending || !m_pending->arranged) {
        m_removalTimer.stop();
        return;
    }
    const QStringList &removed = m_pending->removed;
    const bool timedOut = m_pending->removalClock.elapsed() >= RemovalTimeoutMs;
    const bool anyScreen = std::any_of(removed.cbegin(), removed.cend(), &screenPresent);
    if (anyScreen && !timedOut) {
        return;
    }
    if (!timedOut) {
        // No QScreen left: confirm with KWin itself before judging the
        // arrangement, since that read-back is what the judgement is made
        // from and the destroy may not have reached the compositor yet.
        const auto now = PhysicalOutputGuard::readOutputs();
        const bool anyOutput = std::any_of(removed.cbegin(), removed.cend(), [&now](const QString &name) {
            return std::any_of(now.cbegin(), now.cend(), [&name](const Output &output) {
                return output.name == name;
            });
        });
        if (anyOutput) {
            return;
        }
    }
    m_removalTimer.stop();
    if (timedOut) {
        qWarning() << "Removed output(s)" << removed.join(u", "_s) << "still reported" << RemovalTimeoutMs << "ms after their removal was requested; checking the arrangement anyway";
    }
    reassertAfterRemoval();
}

void HostLayoutExecutor::reassertAfterRemoval()
{
    // The same target minus the parked entries, through the same guard path
    // as after the creation; the targets are asked for again as they are,
    // the read-back having only said whether they are still there.
    const auto entries = arrangementWithout(m_pending->derived.arrangement, m_pending->removed);
    if (!m_guard->reconcileArrangement(entries, u"after removing %1 output(s)"_s.arg(m_pending->removed.size()))) {
        // The outputs stay what KWin made of them: current() reports that,
        // the client re-maps, the next apply asks for the targets again and
        // the release restores the desk.
        m_unverified = true;
        finish(Error{u"invalid"_s, u"the compositor did not keep the requested arrangement"_s});
        return;
    }
    startScreenWait();
}

void HostLayoutExecutor::startScreenWait()
{
    m_pending->screenClock.start();
    m_pending->screensGoodSince = -1;
    m_screenTimer.start();
    pollScreens();
}

void HostLayoutExecutor::pollScreens()
{
    if (!m_pending || !m_pending->arranged) {
        m_screenTimer.stop();
        return;
    }
    const auto &needed = m_pending->derived.neededScreens;
    const bool allPresent = std::all_of(needed.cbegin(), needed.cend(), &screenPresent);
    const qint64 elapsed = m_pending->screenClock.elapsed();
    if (allPresent) {
        if (m_pending->screensGoodSince < 0) {
            m_pending->screensGoodSince = elapsed;
        }
        if (elapsed - m_pending->screensGoodSince >= ScreenStableMs) {
            m_screenTimer.stop();
            finish(std::nullopt);
        }
        return;
    }
    m_pending->screensGoodSince = -1;
    if (elapsed >= ScreenTimeoutMs) {
        m_screenTimer.stop();
        QStringList missing;
        for (const auto &name : needed) {
            if (!screenPresent(name)) {
                missing.push_back(name);
            }
        }
        finish(Error{u"invalid"_s, u"output(s) %1 did not come up within %2 ms; the layout is applied but they cannot be streamed yet"_s.arg(missing.join(u", "_s)).arg(ScreenTimeoutMs)});
    }
}

void HostLayoutExecutor::abortPending(const Error &error)
{
    if (!m_pending) {
        return;
    }
    m_dpmsTimer.stop();
    m_removalTimer.stop();
    m_screenTimer.stop();
    for (auto &creator : m_pending->creating) {
        creator.session.reset();
    }
    m_pending->creating.clear();
    if (m_pending->tookHold && !m_pending->anyCreatorStarted && !m_pending->arranged && m_outputs.empty() && m_guard) {
        // Nothing has changed since this apply took the hold: no output was
        // created (the settle-timeout and portal-factory aborts land here,
        // before the first start()) and the arrangement never ran, so the
        // hold is dropped rather than spent on a needless restore at the
        // owner's disconnect (re-review Minor 3). Once a creator has been
        // started the hold stays: KWin may have replayed a remembered
        // arrangement the moment the known output appeared, and the
        // snapshot is what undoes that at release.
        m_guard->cancelLayoutControl();
    }
    finish(error);
}

void HostLayoutExecutor::finish(std::optional<Error> error)
{
    if (!m_pending) {
        return;
    }
    Result result;
    result.requester = m_pending->requester;
    result.created = m_pending->created;
    result.removed = m_pending->removed;
    result.error = std::move(error);
    m_pending.reset();
    if (!controlling()) {
        // Nothing held and nothing created: the idle layout is the fresh
        // read, not a remembered one.
        m_layout = {};
        m_unverified = false;
    }
    result.layout = current();
    if (result.error) {
        qWarning().noquote() << u"Layout apply from %1 did not fully land: %2"_s.arg(result.requester, result.error->message);
    } else {
        qInfo().noquote() << u"Layout apply from %1 landed: %2 monitors, %3 output(s) created, %4 removed"_s.arg(result.requester)
                                 .arg(result.layout.monitors.size())
                                 .arg(result.created.size())
                                 .arg(result.removed.size());
    }
    Q_EMIT layoutChanged();
    Q_EMIT finished(result);
}

void HostLayoutExecutor::destroyOutputs(const QStringList &names)
{
    for (const auto &name : names) {
        const auto it = std::find_if(m_outputs.begin(), m_outputs.end(), [&name](const Creator &creator) {
            return creator.record.name == name;
        });
        if (it != m_outputs.end()) {
            qInfo() << "Removing virtual output" << name;
            m_outputs.erase(it);
        }
    }
}

void HostLayoutExecutor::releaseAll()
{
    if (m_pending) {
        abortPending(Error{u"released"_s, u"the layout was released while the apply was in progress"_s});
    }
    if (!controlling()) {
        return;
    }
    const int virtualCount = int(m_outputs.size());

    if (m_guard) {
        // Park the virtual outputs beside the restored physical desktop as
        // part of the restore (the guard does it once the physical outputs
        // have settled): KWin records the last arrangement it saw for an
        // output set, and physical outputs on with a stand-in still over
        // their origin is the overlap that hung plasmashell on 2026-09-17.
        // Then they can go. Everything goes, so the snapshot's edge is the
        // right anchor here (unlike an apply's park, which must clear the
        // outputs that stay - LayoutArrangement::derive()).
        QVector<Placement> placements;
        QPoint park = extendAnchor();
        for (const auto &creator : m_outputs) {
            placements.push_back({creator.record.name, park});
            park.rx() += logicalWidth(creator.record.size, creator.record.scale);
        }
        if (!placements.isEmpty()) {
            m_guard->setParkPlacements(placements, placements.first().name);
        }
        if (!m_guard->release() && m_guard->held()) {
            m_guard->parkIfPhysicalEnabled();
        }
        m_guard->clearParkPlacements();
    }

    m_outputs.clear();
    m_layout = {};
    m_unverified = false;
    qInfo() << "Layout control released:" << virtualCount << "virtual output(s) removed, physical outputs" << (m_guard && m_guard->held() ? "NOT verifiably restored (the guard retries)" : "restored");
    Q_EMIT layoutChanged();
}
