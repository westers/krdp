// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerservices.h"
#include <KLocalizedString>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QPointer>
#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;
namespace {
const QString service = u"org.freedesktop.systemd1"_s;
const QString managerPath = u"/org/freedesktop/systemd1"_s;
const QString manager = u"org.freedesktop.systemd1.Manager"_s;
const QString unitInterface = u"org.freedesktop.systemd1.Unit"_s;
const QString properties = u"org.freedesktop.DBus.Properties"_s;
QDBusMessage call(const QString &method, const QVariantList &arguments = {}, bool authorize = false)
{
    auto message = QDBusMessage::createMethodCall(service, managerPath, manager, method);
    message.setArguments(arguments);
    message.setInteractiveAuthorizationAllowed(authorize);
    return message;
}
template<class Reply, class Handler>
void request(const QDBusConnection &bus, const QDBusMessage &message, QObject *context, Handler handler)
{
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, message.isInteractiveAuthorizationAllowed() ? 120000 : 25000), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [watcher, handler]() mutable {
        watcher->deleteLater(); handler(Reply(*watcher));
    });
}
bool canAutostart(const BrokerServiceState &s)
{
    return s.known && s.loadState == u"loaded"_s
        && QStringList{u"enabled"_s, u"disabled"_s, u"enabled-runtime"_s}.contains(s.unitFileState);
}
bool canRun(const BrokerServiceState &s)
{
    return s.known && s.loadState == u"loaded"_s
        && QStringList{u"enabled"_s, u"enabled-runtime"_s, u"disabled"_s, u"static"_s, u"indirect"_s,
            u"linked"_s, u"linked-runtime"_s, u"alias"_s, u"generated"_s, u"transient"_s}.contains(s.unitFileState);
}
}

QString BrokerServiceTransport::unit(int route)
{
    return route == 0 ? u"farside-console-host.service"_s : route == 1 ? u"farside-virtual-host.service"_s : QString();
}

SystemBrokerServiceTransport::SystemBrokerServiceTransport(const QDBusConnection &bus, QObject *parent)
    : BrokerServiceTransport(parent), m_bus(bus)
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { finish(i18nc("@info", "The operation took too long. Refresh to see the service’s actual state.")); });
    m_bus.connect(service, managerPath, manager, u"JobRemoved"_s, this, SLOT(jobRemoved(uint,QDBusObjectPath,QString,QString)));
    m_bus.connect(service, managerPath, manager, u"UnitFilesChanged"_s, this, SLOT(unitFilesChanged()));
    auto *owner = new QDBusServiceWatcher(service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(owner, &QDBusServiceWatcher::serviceOwnerChanged, this, &SystemBrokerServiceTransport::managerOwnerChanged);
    subscribe();
}

void SystemBrokerServiceTransport::subscribe()
{
    if (m_subscribed || m_subscriptionPending) return;
    m_subscriptionPending = true;
    const auto epoch = m_managerGeneration;
    request<QDBusPendingReply<>>(m_bus, call(u"Subscribe"_s), this, [this, epoch](const auto &reply) {
        if (epoch != m_managerGeneration) return;
        m_subscriptionPending = false;
        m_subscribed = !reply.isError() || reply.error().name() == u"org.freedesktop.systemd1.AlreadySubscribed"_s;
    });
}

void SystemBrokerServiceTransport::query(int route, QueryDone done)
{
    const auto name = unit(route);
    if (name.isEmpty()) { done({}, i18nc("@info", "That service is not known.")); return; }
    subscribe();
    const auto epoch = m_managerGeneration;
    const auto originalDone = std::move(done);
    done = [this, epoch, originalDone](BrokerServiceState state, const QString &error) {
        originalDone(epoch == m_managerGeneration ? state : BrokerServiceState{},
            epoch == m_managerGeneration ? error : i18nc("@info", "The system service manager restarted. Refresh to see the current state."));
    };
    request<QDBusPendingReply<QDBusObjectPath>>(m_bus, call(u"LoadUnit"_s, {name}), this, [this, route, name, done](const auto &loaded) {
        if (loaded.isError()) { done({}, loaded.error().message()); return; }
        const auto path = loaded.value().path();
        if (m_unitPaths[route] != path) {
            if (!m_unitPaths[route].isEmpty())
                m_bus.disconnect(service, m_unitPaths[route], properties, u"PropertiesChanged"_s,
                                 this, SLOT(unitPropertiesChanged(QString,QVariantMap,QStringList)));
            m_unitPaths[route] = path;
            m_bus.connect(service, path, properties, u"PropertiesChanged"_s,
                          this, SLOT(unitPropertiesChanged(QString,QVariantMap,QStringList)));
        }
        auto message = QDBusMessage::createMethodCall(service, path, properties, u"GetAll"_s);
        message.setArguments({unitInterface});
        request<QDBusPendingReply<QVariantMap>>(m_bus, message, this, [this, name, path, done](const auto &reply) {
            if (reply.isError()) { done({}, reply.error().message()); return; }
            const auto values = reply.value();
            BrokerServiceState state;
            for (const auto &key : {u"LoadState"_s, u"ActiveState"_s, u"SubState"_s}) {
                if (values.value(key).metaType() != QMetaType::fromType<QString>() || values.value(key).toString().isEmpty()) {
                    done({}, i18nc("@info", "The service’s state could not be read completely.")); return;
                }
            }
            state.known = true;
            state.loadState = values.value(u"LoadState"_s).toString();
            state.activeState = values.value(u"ActiveState"_s).toString();
            state.subState = values.value(u"SubState"_s).toString();
            if (state.loadState == u"not-found"_s) { done(state, {}); return; }
            request<QDBusPendingReply<QString>>(m_bus, call(u"GetUnitFileState"_s, {name}), this, [this, path, state, done](const auto &file) mutable {
                if (file.isError()) { done({}, file.error().message()); return; }
                state.unitFileState = file.value();
                auto pid = QDBusMessage::createMethodCall(service, path, properties, u"Get"_s);
                pid.setArguments({u"org.freedesktop.systemd1.Service"_s, u"MainPID"_s});
                request<QDBusPendingReply<QDBusVariant>>(m_bus, pid, this, [state, done](const auto &reply) mutable {
                    if (reply.isError() || reply.value().variant().metaType() != QMetaType::fromType<quint32>()) {
                        done({}, i18nc("@info", "The service’s process could not be read.")); return;
                    }
                    state.mainPid = reply.value().variant().toUInt(); done(state, {});
                });
            });
        });
    });
}

void SystemBrokerServiceTransport::operate(int route, Operation operation, Done done)
{
    const auto name = unit(route);
    if (name.isEmpty() || operation < Start || operation > Disable) { done(i18nc("@info", "That operation is not supported.")); return; }
    if (m_done) { done(i18nc("@info", "Another service operation is still running.")); return; }
    m_done = std::move(done); m_unit = name; m_job.clear(); m_earlyJobs.clear();
    const auto generation = ++m_generation;
    m_deadline.start(180000);
    if (operation == Enable || operation == Disable) {
        const auto method = operation == Enable ? u"EnableUnitFiles"_s : u"DisableUnitFiles"_s;
        QVariantList arguments{QStringList{name}, false};
        if (operation == Enable) arguments.append(false); // persistent, no force
        request<QDBusPendingReply<>>(m_bus, call(method, arguments, true), this, [this, generation](const auto &reply) {
            if (!m_done || generation != m_generation) return;
            if (reply.isError()) { finish(reply.error().message()); return; }
            request<QDBusPendingReply<>>(m_bus, call(u"Reload"_s, {}, true), this, [this, generation](const auto &reload) {
                if (!m_done || generation != m_generation) return;
                finish(reload.isError() ? i18nc("@info %1 reason", "The startup setting changed, but the system service manager could not reload: %1", reload.error().message()) : QString());
            });
        });
        return;
    }
    // Subscribe before queueing a job; JobRemoved may precede the method reply.
    const auto queue = [this, operation, name, generation] {
        if (!m_done || generation != m_generation) return;
        const auto method = operation == Start ? u"StartUnit"_s : operation == Stop ? u"StopUnit"_s : u"RestartUnit"_s;
        request<QDBusPendingReply<QDBusObjectPath>>(m_bus, call(method, {name, u"replace"_s}, true), this, [this, generation](const auto &job) {
            if (!m_done || generation != m_generation) return;
            if (job.isError()) { finish(job.error().message()); return; }
            m_job = job.value().path();
            if (!m_job.startsWith(u"/org/freedesktop/systemd1/job/"_s)) { finish(i18nc("@info", "The system service manager gave an unexpected reply. Refresh to see the current state.")); return; }
            if (m_earlyJobs.contains(m_job)) {
                const auto result = m_earlyJobs.value(m_job);
                finish(result == u"done"_s ? QString() : u"Service job did not complete: "_s + result);
            }
        });
    };
    if (m_subscribed) { queue(); return; }
    request<QDBusPendingReply<>>(m_bus, call(u"Subscribe"_s), this, [this, queue, generation](const auto &subscribed) {
        if (!m_done || generation != m_generation) return;
        // Qt may share this connection with another module instance whose
        // subscription is still active. It already provides the needed signals.
        if (subscribed.isError() && subscribed.error().name() != u"org.freedesktop.systemd1.AlreadySubscribed"_s) {
            finish(subscribed.error().message()); return;
        }
        m_subscribed = true; queue();
    });
}

void SystemBrokerServiceTransport::jobRemoved(uint, const QDBusObjectPath &path, const QString &name, const QString &result)
{
    if (name != unit(0) && name != unit(1)) return;
    if (!m_done || name != m_unit) { Q_EMIT changed(); return; }
    if (m_job.isEmpty()) { if (m_earlyJobs.size() < 64) m_earlyJobs.insert(path.path(), result); return; }
    if (path.path() == m_job) finish(result == u"done"_s ? QString() : u"Service job did not complete: "_s + result);
}
void SystemBrokerServiceTransport::unitFilesChanged() { Q_EMIT changed(); }
void SystemBrokerServiceTransport::unitPropertiesChanged(const QString &interface, const QVariantMap &values, const QStringList &invalidated)
{
    if (interface != unitInterface && interface != u"org.freedesktop.systemd1.Service"_s) return;
    for (const auto &key : {u"LoadState"_s, u"ActiveState"_s, u"SubState"_s, u"MainPID"_s}) {
        if (values.contains(key) || invalidated.contains(key)) { Q_EMIT changed(); return; }
    }
}
void SystemBrokerServiceTransport::managerOwnerChanged(const QString &, const QString &, const QString &)
{
    ++m_managerGeneration; m_subscribed = m_subscriptionPending = false;
    if (m_done) finish(i18nc("@info", "The system service manager restarted during the operation. Refresh to see the current state."));
    Q_EMIT changed();
}
void SystemBrokerServiceTransport::finish(const QString &error)
{
    if (!m_done) return;
    ++m_generation;
    m_deadline.stop(); auto done = std::move(m_done); m_done = {}; m_job.clear(); m_earlyJobs.clear(); done(error);
}

BrokerServices::BrokerServices(QObject *parent)
    : BrokerServices(new SystemBrokerServiceTransport(QDBusConnection::systemBus()), parent) { m_transport->setParent(this); }
BrokerServices::BrokerServices(BrokerServiceTransport *transport, QObject *parent)
    : QObject(parent), m_transport(transport)
{
    connect(transport, &BrokerServiceTransport::changed, this, [this] {
        if (busy()) { m_refreshPending = true; return; }
        for (int i = 0; i < 2; ++i) query(i, false);
    });
}
bool BrokerServices::busy() const { return std::any_of(m_entries.cbegin(), m_entries.cend(), [](const auto &e) { return e.pending || e.querying; }); }
QVariantList BrokerServices::services() const
{
    QVariantList list;
    for (int i = 0; i < 2; ++i) {
        const auto &e = m_entries[i]; const auto &s = e.state;
        const bool idle = !busy();
        list.append(QVariantMap{{u"route"_s, i == 0 ? u"console"_s : u"virtual"_s}, {u"unit"_s, BrokerServiceTransport::unit(i)},
            {u"known"_s, s.known}, {u"loadState"_s, s.loadState}, {u"activeState"_s, s.activeState}, {u"subState"_s, s.subState},
            {u"unitFileState"_s, s.unitFileState}, {u"mainPid"_s, s.mainPid}, {u"busy"_s, e.pending || e.querying}, {u"error"_s, e.error},
            {u"autostart"_s, s.unitFileState == u"enabled"_s}, {u"canAutostart"_s, idle && canAutostart(s)},
            {u"canStart"_s, idle && canRun(s) && (s.activeState == u"inactive"_s || s.activeState == u"failed"_s)},
            {u"canStop"_s, idle && s.known && QStringList{u"active"_s, u"activating"_s, u"reloading"_s}.contains(s.activeState)},
            {u"canRestart"_s, idle && canRun(s) && (s.activeState == u"active"_s || s.activeState == u"failed"_s)}});
    }
    return list;
}
void BrokerServices::refresh(bool clearErrors)
{
    if (busy()) { m_refreshPending = true; return; }
    for (int i = 0; i < 2; ++i) query(i, clearErrors);
}
void BrokerServices::query(int route, bool clearError)
{
    auto &entry = m_entries[route]; entry.querying = true; if (clearError) entry.error.clear(); Q_EMIT changed();
    QPointer self(this);
    m_transport->query(route, [self, route](BrokerServiceState state, const QString &error) {
        if (!self) return;
        auto &e = self->m_entries[route]; e.state = state; e.querying = e.pending = false;
        if (!error.isEmpty()) e.error = error;
        Q_EMIT self->changed();
        if (!self->busy() && std::exchange(self->m_refreshPending, false)) {
            for (int i = 0; i < 2; ++i) self->query(i, false);
        }
    });
}
bool BrokerServices::perform(const QString &route, const QString &operation)
{
    const int index = route == u"console"_s ? 0 : route == u"virtual"_s ? 1 : -1;
    if (index < 0 || busy()) return false;
    const auto row = services()[index].toMap();
    BrokerServiceTransport::Operation action;
    if (operation == u"start"_s && row.value(u"canStart"_s).toBool()) action = BrokerServiceTransport::Start;
    else if (operation == u"stop"_s && row.value(u"canStop"_s).toBool()) action = BrokerServiceTransport::Stop;
    else if (operation == u"restart"_s && row.value(u"canRestart"_s).toBool()) action = BrokerServiceTransport::Restart;
    else if (operation == u"enable"_s && row.value(u"canAutostart"_s).toBool() && !row.value(u"autostart"_s).toBool()) action = BrokerServiceTransport::Enable;
    else if (operation == u"disable"_s && row.value(u"canAutostart"_s).toBool() && row.value(u"autostart"_s).toBool()) action = BrokerServiceTransport::Disable;
    else return false;
    auto &e = m_entries[index]; e.pending = true; e.error.clear(); Q_EMIT changed();
    QPointer self(this);
    m_transport->operate(index, action, [self, index](const QString &error) {
        if (!self) return;
        self->m_entries[index].error = error;
        self->query(index, false); // Success/denial/timeout all require actual readback.
    });
    return true;
}
