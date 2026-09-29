// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "systemdservicemanager.h"

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QFileInfo>

using namespace Qt::StringLiterals;

namespace
{
const QString systemdService = u"org.freedesktop.systemd1"_s;
const QString systemdPath = u"/org/freedesktop/systemd1"_s;
const QString managerInterface = u"org.freedesktop.systemd1.Manager"_s;
const QString unitInterface = u"org.freedesktop.systemd1.Unit"_s;
const QString serviceInterface = u"org.freedesktop.systemd1.Service"_s;
const QString propertiesInterface = u"org.freedesktop.DBus.Properties"_s;

QDBusMessage managerCall(const QString &method, const QVariantList &arguments)
{
    auto msg = QDBusMessage::createMethodCall(systemdService, systemdPath, managerInterface, method);
    msg.setArguments(arguments);
    return msg;
}

template<typename Reply, typename Handler>
void whenFinished(const QDBusConnection &bus, const QDBusMessage &msg, QObject *context, Handler handler)
{
    auto watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [watcher, handler]() mutable {
        watcher->deleteLater();
        handler(Reply(*watcher));
    });
}
}

SystemdServiceManager::SystemdServiceManager(const QDBusConnection &bus, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
{
}

void SystemdServiceManager::queryUnit(const QString &unit, StateDone done)
{
    // GetUnitFileState fails for a unit that is not installed; LoadUnit then
    // gives an object path even for a unit that is not running.
    auto bus = m_bus;
    whenFinished<QDBusPendingReply<QString>>(m_bus, managerCall(u"GetUnitFileState"_s, {unit}), this, [this, bus, unit, done](const QDBusPendingReply<QString> &fileState) {
        if (fileState.isError()) {
            done(Coexistence::UnitState{});
            return;
        }
        Coexistence::UnitState state;
        state.known = true;
        state.unitFileState = fileState.value();
        whenFinished<QDBusPendingReply<QDBusObjectPath>>(bus, managerCall(u"LoadUnit"_s, {unit}), this, [this, bus, state, done](const QDBusPendingReply<QDBusObjectPath> &path) mutable {
            if (path.isError()) {
                done(state);
                return;
            }
            const QString objectPath = path.value().path();
            auto get = QDBusMessage::createMethodCall(systemdService, objectPath, propertiesInterface, u"Get"_s);
            get.setArguments({unitInterface, u"ActiveState"_s});
            whenFinished<QDBusPendingReply<QDBusVariant>>(bus, get, this, [this, bus, objectPath, state, done](const QDBusPendingReply<QDBusVariant> &active) mutable {
                if (!active.isError()) {
                    state.activeState = active.value().variant().toString();
                }
                auto pid = QDBusMessage::createMethodCall(systemdService, objectPath, propertiesInterface, u"Get"_s);
                pid.setArguments({serviceInterface, u"MainPID"_s});
                whenFinished<QDBusPendingReply<QDBusVariant>>(bus, pid, this, [state, done](const QDBusPendingReply<QDBusVariant> &mainPid) mutable {
                    if (!mainPid.isError()) {
                        state.mainPid = mainPid.value().variant().toLongLong();
                    }
                    done(state);
                });
            });
        });
    });
}

void SystemdServiceManager::stopUnit(const QString &unit, Done done)
{
    whenFinished<QDBusPendingReply<QDBusObjectPath>>(m_bus, managerCall(u"StopUnit"_s, {unit, u"replace"_s}), this, [done](const QDBusPendingReply<QDBusObjectPath> &reply) {
        done(reply.isError() ? reply.error().message() : QString());
    });
}

void SystemdServiceManager::disableUnitFile(const QString &unit, Done done)
{
    whenFinished<QDBusPendingReply<>>(m_bus, managerCall(u"DisableUnitFiles"_s, {QStringList{unit}, false}), this, [done](const QDBusPendingReply<> &reply) {
        done(reply.isError() ? reply.error().message() : QString());
    });
}

bool SystemdServiceManager::systemUnitInstalled(const QString &unit) const
{
    for (const auto &dir : {u"/etc/systemd/system/"_s, u"/usr/lib/systemd/system/"_s, u"/lib/systemd/system/"_s, u"/usr/local/lib/systemd/system/"_s}) {
        if (QFileInfo::exists(dir + unit)) {
            return true;
        }
    }
    return false;
}
