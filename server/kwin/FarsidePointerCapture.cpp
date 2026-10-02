// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: GPL-3.0-or-later
#include <plugin.h>
#include <input.h>
#include <input_event.h>
#include <pointer_input.h>
#include <workspace.h>
#include <window.h>
#include <wayland_server.h>
#include <wayland/seat.h>
#include <wayland/surface.h>
#include <wayland/pointerconstraints_v1.h>
#include <effect/effecthandler.h>
#include "upstream/tabbox/tabbox.h"
#include <QDBusContext>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <unistd.h>

namespace {
constexpr auto Path = "/org/kde/KWin/FarsidePointerCapture";
// No input injection, cursor heuristics or permissions changes. This object lives
// inside the exact-version compositor and observes its real constraint objects.
class Bridge final : public KWin::Plugin, public KWin::InputEventFilter, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.farside.PointerCapture1")
public:
    Bridge() : KWin::InputEventFilter(KWin::InputFilterOrder::InputMethod), m_epoch(QUuid::createUuid().toString(QUuid::WithoutBraces)), m_peerWatch(QString(), QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForUnregistration) {
        KWin::input()->installInputEventFilter(this);
        m_expiry.setSingleShot(true); m_expiry.setInterval(6000);
        connect(&m_expiry, &QTimer::timeout, this, &Bridge::restore);
        connect(&m_peerWatch, &QDBusServiceWatcher::serviceUnregistered, this, &Bridge::restore);
        connect(KWin::workspace(), &KWin::Workspace::windowActivated, this, [this] { observe(); });
        connect(KWin::input(), &KWin::InputRedirection::globalPointerChanged, this, [this] { observe(); });
        if (KWin::effects) {
            connect(KWin::effects, &KWin::EffectsHandler::tabBoxAdded, this, [this] { m_tabBox = true; if (!m_peer.isEmpty()) enforce(); publish(); });
            connect(KWin::effects, &KWin::EffectsHandler::tabBoxClosed, this, [this] { m_tabBox = false; queuedObserve(); });
            connect(KWin::effects, &KWin::EffectsHandler::screenLockingChanged, this, [this] { queuedObserve(); });
        }
        QDBusConnection::sessionBus().registerObject(QString::fromLatin1(Path), this,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
        observe();
    }
    ~Bridge() override {
        KWin::input()->uninstallInputEventFilter(this);
        restore();
        QDBusConnection::sessionBus().unregisterObject(QString::fromLatin1(Path));
    }
    bool pointerMotion(KWin::PointerMotionEvent *event) override {
        // Unlocking a constraint does not silence ordinary wl_pointer motion:
        // Xwayland games can still turn it into mouse-look. Keep KWin's cursor
        // position/focus updates, but don't forward remote motion to that game.
        // Physical input, decorations, other windows and actual game menus keep
        // their normal behavior. This filter only exists for our current lease.
        if (m_peer.isEmpty() || m_enabled || blocked() || !event->device
            || !event->device->inherits("KWin::FakeInputDevice")) return false;
        auto focused = KWin::waylandServer()->seat()->focusedPointerSurface();
        return focused && focused->lockedPointer();
    }
public Q_SLOTS:
    QString Snapshot() { observe(); return encoded(); }
    QString SetPolicy(const QString &epoch, const QString &generation, bool enabled) {
        if (!sameUser() || epoch != m_epoch || generation.isEmpty() || generation.size() > 32)
            return error(QStringLiteral("invalid-peer-or-epoch"));
        bool valid = false; const auto number = generation.toULongLong(&valid);
        if (!valid || number == 0 || QString::number(number) != generation) return error(QStringLiteral("invalid-generation"));
        const auto peer = message().service();
        if (!m_peer.isEmpty() && m_peer != peer) return error(QStringLiteral("busy"));
        if (m_peer == peer && !m_generation.isEmpty() && number < m_generation.toULongLong()) return error(QStringLiteral("stale-generation"));
        m_peer = peer; m_generation = generation; m_enabled = enabled;
        m_peerWatch.setWatchedServices({peer}); m_expiry.start();
        enforce(); observe(); return encoded();
    }
    QString Release(const QString &epoch, const QString &generation) {
        if (!sameUser() || epoch != m_epoch || message().service() != m_peer || generation != m_generation)
            return error(QStringLiteral("stale-lease"));
        restore(); return encoded();
    }
Q_SIGNALS:
    void StateChanged(const QString &state);
private:
    bool sameUser() const {
        if (!calledFromDBus()) return false;
        const auto uid = QDBusConnection::sessionBus().interface()->serviceUid(message().service());
        return uid.isValid() && uid.value() == uint(getuid());
    }
    bool blocked() const {
        const auto tabbox = KWin::workspace()->tabbox();
        return m_tabBox || (tabbox && tabbox->isGrabbed()) || KWin::waylandServer()->isScreenLocked();
    }
    void enforce() {
        // KWin disables constraints during Alt+Tab too: never re-enable them
        // during that temporary override or during actual desktop locking.
        KWin::input()->pointer()->setEnableConstraints((m_peer.isEmpty() || m_enabled) && !blocked());
    }
    void restore() {
        if (m_peer.isEmpty()) return;
        m_peer.clear(); m_generation.clear(); m_enabled = true;
        m_expiry.stop(); m_peerWatch.setWatchedServices({});
        enforce(); observe();
    }
    void queuedObserve() {
        if (m_queued) return;
        m_queued = true;
        QTimer::singleShot(0, this, [this] { m_queued = false; if (!m_peer.isEmpty()) enforce(); observe(); });
    }
    void observe() {
        auto active = KWin::workspace()->activeWindow();
        auto surface = active ? active->surface() : nullptr;
        auto focused = KWin::waylandServer()->seat()->focusedPointerSurface();
        if (focused && surface && focused->mainSurface() == surface->mainSurface()) surface = focused;
        auto lock = surface ? surface->lockedPointer() : nullptr;
        if (surface != m_surface || lock != m_lock) {
            for (const auto &c : m_connections) disconnect(c);
            m_connections.clear(); m_surface = surface; m_lock = lock;
            if (surface) {
                m_connections.append(connect(surface, &KWin::SurfaceInterface::pointerConstraintsChanged, this, &Bridge::queuedObserve));
                m_connections.append(connect(surface, &QObject::destroyed, this, &Bridge::queuedObserve));
            }
            if (lock) {
                m_connections.append(connect(lock, &KWin::LockedPointerV1Interface::lockedChanged, this, &Bridge::queuedObserve));
                m_connections.append(connect(lock, &QObject::destroyed, this, &Bridge::queuedObserve));
            }
        }
        publish();
    }
    QJsonObject fields() const {
        return {{QStringLiteral("v"), 1}, {QStringLiteral("epoch"), m_epoch},
            {QStringLiteral("supported"), true}, {QStringLiteral("requested"), bool(m_lock)},
            {QStringLiteral("locked"), m_lock && m_lock->isLocked()},
            {QStringLiteral("permitted"), !m_peer.isEmpty() && m_enabled && !blocked()},
            {QStringLiteral("leased"), !m_peer.isEmpty()}, {QStringLiteral("generation"), m_generation},
            {QStringLiteral("blocked"), blocked()}};
    }
    void publish() {
        const auto next = fields();
        if (next == m_last) return;
        m_last = next; ++m_revision;
        Q_EMIT StateChanged(encoded());
    }
    QString encoded() const {
        auto result = fields(); result.insert(QStringLiteral("revision"), QString::number(m_revision));
        return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    }
    QString error(const QString &reason) const {
        return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("error"), reason}}).toJson(QJsonDocument::Compact));
    }
    QString m_epoch, m_peer, m_generation;
    bool m_enabled = true, m_tabBox = false, m_queued = false;
    quint64 m_revision = 0;
    QTimer m_expiry;
    QDBusServiceWatcher m_peerWatch;
    QPointer<KWin::SurfaceInterface> m_surface;
    QPointer<KWin::LockedPointerV1Interface> m_lock;
    QList<QMetaObject::Connection> m_connections;
    QJsonObject m_last;
};
class Factory final : public KWin::PluginFactory {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PluginFactory_iid FILE "farside-pointer-capture.json")
    Q_INTERFACES(KWin::PluginFactory)
public: std::unique_ptr<KWin::Plugin> create() const override { return std::make_unique<Bridge>(); }
};
}
#include "FarsidePointerCapture.moc"
