// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "HostLayoutExecutor.h"

#include <algorithm>
#include <cmath>

#include <QDebug>
#include <QGuiApplication>
#include <QScreen>

using namespace KRdp::LayoutControl;
using namespace KRdp::OutputSnapshot;
using namespace Qt::StringLiterals;

namespace
{
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

QString scaleTag(qreal scale)
{
    return QString::number(qRound(scale * 100.0));
}

bool screenPresent(const QString &name)
{
    const auto screens = QGuiApplication::screens();
    return std::any_of(screens.cbegin(), screens.cend(), [&name](const QScreen *screen) {
        return screen && screen->name() == name;
    });
}
}

HostLayoutExecutor::HostLayoutExecutor(PhysicalOutputGuard *guard, SessionFactory sessionFactory, QObject *parent)
    : QObject(parent)
    , m_guard(guard)
    , m_sessionFactory(std::move(sessionFactory))
{
    m_screenTimer.setInterval(ScreenPollMs);
    connect(&m_screenTimer, &QTimer::timeout, this, &HostLayoutExecutor::pollScreens);
}

HostLayoutExecutor::~HostLayoutExecutor()
{
    releaseAll();
}

QString HostLayoutExecutor::standInName(const QString &realId, const QSize &size, qreal scale)
{
    // Size and scale are part of the name on purpose: KWin remembers an
    // arrangement (scale included) per output identity, and a same-named
    // output at another size would have that replayed onto it. No dots:
    // kscreen-doctor splits `output.<name>.<setting>` on them.
    return u"krdp-si-%1-%2x%3-s%4"_s.arg(realId).arg(size.width()).arg(size.height()).arg(scaleTag(scale));
}

QString HostLayoutExecutor::virtualName(const QString &virtualId, const QSize &size, qreal scale)
{
    QString number = virtualId;
    number.remove(u"virtual-"_s);
    return u"krdp-v%1-%2x%3-s%4"_s.arg(number).arg(size.width()).arg(size.height()).arg(scaleTag(scale));
}

QSize HostLayoutExecutor::presentedSize(const HostMonitor &monitor)
{
    return monitor.standIn && monitor.standInSize ? *monitor.standInSize : monitor.size;
}

qreal HostLayoutExecutor::presentedScale(const HostMonitor &monitor)
{
    return monitor.standIn && monitor.standInScale ? *monitor.standInScale : monitor.scale;
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
    return !m_outputs.empty() || (m_guard && m_guard->held());
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

QString HostLayoutExecutor::outputNameFor(const QString &monitorId) const
{
    const auto layout = m_layout.monitors.isEmpty() ? current() : m_layout;
    const auto it = std::find_if(layout.monitors.cbegin(), layout.monitors.cend(), [&monitorId](const HostMonitor &monitor) {
        return monitor.id == monitorId;
    });
    if (it == layout.monitors.cend()) {
        return {};
    }
    if (it->kind == Kind::Real && it->lit) {
        return it->id;
    }
    const auto output = std::find_if(m_outputs.cbegin(), m_outputs.cend(), [&monitorId](const Creator &creator) {
        return creator.record.monitorId == monitorId;
    });
    return output == m_outputs.cend() ? QString() : output->record.name;
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
    for (auto &monitor : layout.monitors) {
        // Where the output that carries the monitor really is: the connector
        // for a lit real monitor, its stand-in for a dark one, the virtual
        // output for a virtual monitor. A disabled output's position from
        // kscreen is whatever it last was, so only an enabled one counts.
        const QString name = outputNameFor(monitor.id);
        if (name.isEmpty()) {
            continue;
        }
        const auto it = std::find_if(outputs.cbegin(), outputs.cend(), [&name](const Output &output) {
            return output.name == name;
        });
        if (it != outputs.cend() && it->enabled) {
            monitor.position = it->position;
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

    // The output set the resulting layout needs: which real outputs are on,
    // which virtual outputs exist and where everything sits. Primary first,
    // so it takes priority 1.
    QList<HostMonitor> ordered = plan.resulting.monitors;
    std::stable_partition(ordered.begin(), ordered.end(), [](const HostMonitor &monitor) {
        return monitor.primary;
    });
    std::vector<VirtualOutput> wanted;
    for (const auto &monitor : std::as_const(ordered)) {
        if (monitor.kind == Kind::Real) {
            if (monitor.lit) {
                pending.arrangement.push_back({monitor.id, true, monitor.position});
                pending.neededScreens.push_back(monitor.id);
                continue;
            }
            pending.arrangement.push_back({monitor.id, false, QPoint()});
            VirtualOutput standIn;
            standIn.monitorId = monitor.id;
            standIn.size = presentedSize(monitor);
            standIn.scale = presentedScale(monitor);
            standIn.name = VirtualPrefix + standInName(monitor.id, standIn.size, standIn.scale);
            standIn.owner = requester;
            standIn.position = monitor.position;
            standIn.standIn = monitor.standIn;
            wanted.push_back(standIn);
        } else {
            VirtualOutput extra;
            extra.monitorId = monitor.id;
            extra.size = monitor.size;
            extra.scale = monitor.scale;
            extra.name = VirtualPrefix + virtualName(monitor.id, extra.size, extra.scale);
            extra.owner = monitor.owner;
            extra.position = monitor.position;
            wanted.push_back(extra);
        }
        const auto &output = wanted.back();
        pending.arrangement.push_back({output.name, true, output.position});
        pending.neededScreens.push_back(output.name);
    }

    // What exists already stays; what does not is created; what is no
    // longer wanted is parked out of the way by the same arrangement (a
    // stand-in over a real output that comes back on would otherwise be
    // the overlap KWin records) and removed once it has applied.
    QPoint park = extendAnchor();
    for (const auto &creator : m_outputs) {
        const bool stillWanted = std::any_of(wanted.cbegin(), wanted.cend(), [&creator](const VirtualOutput &output) {
            return output.name == creator.record.name;
        });
        if (!stillWanted) {
            pending.removing.push_back(creator.record.name);
            pending.arrangement.push_back({creator.record.name, true, park});
            park.rx() += int(std::ceil(creator.record.size.width() / creator.record.scale));
        }
    }
    for (const auto &output : wanted) {
        const bool exists = std::any_of(m_outputs.cbegin(), m_outputs.cend(), [&output](const Creator &creator) {
            return creator.record.name == output.name;
        });
        if (!exists) {
            pending.creating.push_back(Creator{output, nullptr});
        }
    }

    const bool physicalTouched = std::any_of(pending.arrangement.cbegin(), pending.arrangement.cend(), [](const Arrangement &entry) {
        return !isVirtual(entry.name) && !entry.enabled;
    });
    if (!plan.actions.isEmpty() || physicalTouched || !pending.creating.empty()) {
        // Held before the first virtual output exists: what KWin does with
        // the physical outputs when one appears is not the layout to
        // restore. Every apply, not only the first, so a legacy session's
        // pending restore is noticed.
        if (!m_guard || !m_guard->available()) {
            return Error{u"unsupported"_s, u"layout control needs kscreen-doctor on the server"_s};
        }
        if (!m_guard->beginLayoutControl()) {
            return Error{u"invalid"_s, u"the server cannot take control of the physical outputs right now (see its log)"_s};
        }
    }

    pending.needsArrangement = !plan.actions.isEmpty() || physicalTouched || !pending.creating.empty() || !pending.removing.isEmpty();
    m_pending = std::move(pending);

    if (m_pending->creating.empty()) {
        onCreatorProgress();
        return std::nullopt;
    }

    for (auto &creator : m_pending->creating) {
        auto session = m_sessionFactory();
        if (!session) {
            abortPending(Error{u"unsupported"_s, u"this server cannot create virtual outputs"_s});
            return std::nullopt;
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
        qInfo().noquote() << u"Creating virtual output %1 (%2x%3 @%4) for %5"_s.arg(name).arg(creator.record.size.width()).arg(creator.record.size.height()).arg(creator.record.scale).arg(requester);
        raw->start();
    }
    // start() may have resolved synchronously (KWin replaying a known
    // output); the first progress check runs from the event loop either way.
    scheduleProgress();
    return std::nullopt;
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
    if (!m_pending || m_pending->arranged || m_pending->abortScheduled) {
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

    if (!m_guard->applyArrangement(m_pending->arrangement)) {
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
    m_pending->removed = m_pending->removing;
    destroyOutputs(m_pending->removing);
    m_pending->removing.clear();

    // Positions of what remains, as arranged.
    for (auto &creator : m_outputs) {
        const auto entry = std::find_if(m_pending->arrangement.cbegin(), m_pending->arrangement.cend(), [&creator](const Arrangement &arranged) {
            return arranged.name == creator.record.name;
        });
        if (entry != m_pending->arrangement.cend()) {
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
    const bool allPresent = std::all_of(m_pending->neededScreens.cbegin(), m_pending->neededScreens.cend(), &screenPresent);
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
        for (const auto &name : std::as_const(m_pending->neededScreens)) {
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
        // Then they can go.
        QVector<Placement> placements;
        QPoint park = extendAnchor();
        for (const auto &creator : m_outputs) {
            placements.push_back({creator.record.name, park});
            park.rx() += int(std::ceil(creator.record.size.width() / creator.record.scale));
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
