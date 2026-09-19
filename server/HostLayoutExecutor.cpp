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
// alone, logged).
constexpr int DpmsPollMs = 250;
constexpr int DpmsWakeTimeoutMs = 6000;
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
    m_dpmsTimer.setInterval(DpmsPollMs);
    connect(&m_dpmsTimer, &QTimer::timeout, this, &HostLayoutExecutor::onDpmsPoll);
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

Layout HostLayoutExecutor::current() const
{
    Layout layout;
    layout.you = u"none"_s;
    if (!controlling() || m_layout.monitors.isEmpty()) {
        layout.monitors = readRealMonitors();
        return layout;
    }
    layout.monitors = m_layout.monitors;
    refreshPositions(layout);
    return layout;
}

void HostLayoutExecutor::refreshPositions(Layout &layout) const
{
    const auto outputs = PhysicalOutputGuard::readOutputs();
    if (outputs.isEmpty()) {
        return;
    }
    const auto names = outputNamesFor(layout);
    for (qsizetype i = 0; i < layout.monitors.size(); ++i) {
        // Where the output that carries the monitor really is: the connector
        // for a lit real monitor, its stand-in for a dark one, the virtual
        // output for a virtual monitor. A disabled output's position from
        // kscreen is whatever it last was, so only an enabled one counts.
        const QString &name = names.at(i);
        if (name.isEmpty()) {
            continue;
        }
        const auto it = std::find_if(outputs.cbegin(), outputs.cend(), [&name](const Output &output) {
            return output.name == name;
        });
        if (it != outputs.cend() && it->enabled) {
            layout.monitors[i].position = it->position;
        }
    }
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
    for (const auto &output : std::as_const(pending.derived.creating)) {
        pending.creating.push_back(Creator{output, nullptr});
    }
    pending.needsArrangement = !plan.actions.isEmpty() || pending.derived.physicalDisabled || !pending.creating.empty() || !pending.derived.removing.isEmpty();

    if (pending.needsArrangement) {
        // Held before the first virtual output exists: what KWin does with
        // the physical outputs when one appears is not the layout to
        // restore. Every apply, not only the first, so a configured virtual
        // session's hold is noticed.
        if (!m_guard || !m_guard->available()) {
            return Error{u"unsupported"_s, u"layout control needs kscreen-doctor on the server"_s};
        }
        if (!m_guard->beginLayoutControl()) {
            return Error{u"invalid"_s, u"the server cannot take control of the physical outputs right now (see its log)"_s};
        }
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

    // KWin may already have everything where the arrangement wants it (a
    // re-apply after a takeover restored nothing, say): then there is no
    // call to make and no settle to pay.
    const auto &arrangement = m_pending->derived.arrangement;
    const auto now = PhysicalOutputGuard::readOutputs();
    const bool alreadyArranged = !now.isEmpty() && arrangementMatches(arrangement, now);
    if (alreadyArranged) {
        qInfo() << "Arrangement already in place; no output change needed";
    } else if (!m_guard->applyArrangement(arrangement)) {
        // Whatever KWin did with the command stands until release(); the
        // created outputs go away again, and the layout reported is what
        // is really there.
        for (auto &creator : m_pending->creating) {
            creator.session.reset();
        }
        m_pending->creating.clear();
        finish(Error{u"invalid"_s, u"the compositor did not take the requested arrangement; the layout is as read back"_s});
        return;
    }

    for (auto &creator : m_pending->creating) {
        m_pending->created.push_back(creator.record.name);
        m_outputs.push_back(std::move(creator));
    }
    m_pending->creating.clear();
    m_pending->removed = m_pending->derived.removing;
    destroyOutputs(m_pending->derived.removing);

    // Positions of what remains, as arranged.
    for (auto &creator : m_outputs) {
        const auto entry = std::find_if(arrangement.cbegin(), arrangement.cend(), [&creator](const Arrangement &arranged) {
            return arranged.name == creator.record.name;
        });
        if (entry != arrangement.cend()) {
            creator.record.position = entry->position;
        }
    }
    m_layout = m_pending->target;

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
    m_screenTimer.stop();
    for (auto &creator : m_pending->creating) {
        creator.session.reset();
    }
    m_pending->creating.clear();
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
        abortPending(Error{u"invalid"_s, u"the layout was released while the apply was in progress"_s});
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
    qInfo() << "Layout control released:" << virtualCount << "virtual output(s) removed, physical outputs" << (m_guard && m_guard->held() ? "NOT verifiably restored (the guard retries)" : "restored");
    Q_EMIT layoutChanged();
}
