// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionLogin.h"
#include "VirtualSessionLoginTag.h"
#include <QTest>
#include <QDBusConnection>
#include <QDBusVirtualObject>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QScopeGuard>
#include <functional>
using namespace Qt::StringLiterals;
using KRdp::VirtualSessionLogin;

namespace {
QVariant ownerTuple() {
    QDBusArgument value; value.beginStructure(); value << quint32(1000) << QDBusObjectPath(u"/org/freedesktop/login1/user/_1000"_s); value.endStructure();
    return QVariant::fromValue(value);
}
QVariant seatTuple() {
    QDBusArgument value; value.beginStructure(); value << QString() << QDBusObjectPath(u"/"_s); value.endStructure();
    return QVariant::fromValue(value);
}
class LoginFixture : public QDBusVirtualObject {
public:
    QVariantMap session{
        {u"Id"_s, u"c42"_s}, {u"Service"_s, u"krdp-virtual-session"_s}, {u"Type"_s, u"wayland"_s},
        {u"Class"_s, u"background"_s}, {u"State"_s, u"online"_s}, {u"TTY"_s, QString()},
        {u"Display"_s, QString()}, {u"Desktop"_s, u"krdp-fixture"_s}, {u"Scope"_s, u"session-c42.scope"_s}, {u"Leader"_s, quint32(2345)},
        {u"VTNr"_s, quint32(0)}, {u"User"_s, ownerTuple()}, {u"Seat"_s, seatTuple()}};
    QVariantMap user{{u"UID"_s, quint32(1000)}, {u"RuntimePath"_s, u"/run/user/1000"_s}};
    bool fail = false;
    QString failInterface;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
        if (!fail && message.path() == u"/org/freedesktop/login1"_s && message.interface() == u"org.freedesktop.login1.Manager"_s
            && message.member() == u"GetSessionByPID"_s && message.arguments() == QList<QVariant>{quint32(2345)}) {
            bus.send(message.createReply({QVariant::fromValue(QDBusObjectPath(u"/org/freedesktop/login1/session/c42"_s))}));
        } else if (!fail && message.interface() == u"org.freedesktop.DBus.Properties"_s && message.member() == u"GetAll"_s
            && message.path() == u"/org/freedesktop/login1/session/c42"_s
            && message.arguments() == QList<QVariant>{u"org.freedesktop.login1.Session"_s}
            && failInterface != u"session"_s) {
            bus.send(message.createReply(QList<QVariant>{session}));
        } else if (!fail && message.interface() == u"org.freedesktop.DBus.Properties"_s && message.member() == u"GetAll"_s
            && message.path() == u"/org/freedesktop/login1/user/_1000"_s
            && message.arguments() == QList<QVariant>{u"org.freedesktop.login1.User"_s}
            && failInterface != u"user"_s) {
            bus.send(message.createReply(QList<QVariant>{user}));
        } else {
            bus.send(message.createErrorReply(u"org.freedesktop.DBus.Error.Failed"_s, u"fixture rejection"_s));
        }
        return true;
    }
};
}
class VirtualSessionLoginTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void exactIdentityAndNoConsole() {
        VirtualSessionLogin login{u"c42"_s, u"krdp-virtual-session"_s, u"wayland"_s, u"background"_s,
            u"online"_s, {}, {}, {}, u"/run/user/1000"_s, u"session-c42.scope"_s, 1000, 2345, 0, {}};
        const auto matches = [](const auto &value) { return value.matches(1000, 2345, u"c42"_s, u"/run/user/1000"_s); };
        QVERIFY(matches(login));
        const QList<std::function<void(VirtualSessionLogin &)>> mutations{
            [](auto &s) { s.uid = 1001; }, [](auto &s) { s.leader = 3456; },
            [](auto &s) { s.id = u"c43"_s; }, [](auto &s) { s.id += QLatin1Char('\n'); },
            [](auto &s) { s.service = u"sddm"_s; }, [](auto &s) { s.type = u"tty"_s; },
            [](auto &s) { s.sessionClass = u"user"_s; }, [](auto &s) { s.state = u"closing"_s; },
            [](auto &s) { s.seat = u"seat0"_s; }, [](auto &s) { s.tty = u"tty7"_s; },
            [](auto &s) { s.display = u":0"_s; }, [](auto &s) { s.virtualTerminal = 7; },
            [](auto &s) { s.runtime = u"/run/user/1001"_s; }, [](auto &s) { s.scope = u"session-c43.scope"_s; }};
        for (const auto &mutate : mutations) { auto changed = login; mutate(changed); QVERIFY(!matches(changed)); }
        QVERIFY(!login.matches(0, 2345, login.id, login.runtime));
        QVERIFY(!login.matches(1000, 1, login.id, login.runtime));
        login.state = u"active"_s; QVERIFY(matches(login));
        const auto launch = u"12345678-1234-1234-1234-123456789abc"_s;
        QVERIFY(!login.matchesLaunch(1000, 2345, login.id, login.runtime, launch));
        login.desktop = KRdp::virtualLoginTag(launch);
        QVERIFY(login.matchesLaunch(1000, 2345, login.id, login.runtime, launch));
        QVERIFY(!login.matchesLaunch(1000, 2345, login.id, login.runtime, u"12345678-1234-1234-1234-123456789abd"_s));
        QVERIFY(!login.matchesLaunch(1000, 2345, login.id, login.runtime, QString()));
        login.desktop = u"KDE"_s;
        QVERIFY(!login.matchesLaunch(1000, 2345, login.id, login.runtime, launch));
    }
    void readsStrictlyTypedDbusSnapshot() {
        const auto daemon = QStandardPaths::findExecutable(u"dbus-daemon"_s);
        if (daemon.isEmpty()) QSKIP("Private DBus fixture needs dbus-daemon");
        QProcess process;
        process.start(daemon, {u"--session"_s, u"--nofork"_s, u"--print-address=1"_s});
        QVERIFY(process.waitForStarted(3000)); QVERIFY(process.waitForReadyRead(3000));
        const auto address = process.readLine().trimmed(); QVERIFY(address.startsWith("unix:"));
        // Test binary has not opened a system bus before this point. Never use
        // the actual system bus for this test; redirect to a disposable daemon.
        qputenv("DBUS_SYSTEM_BUS_ADDRESS", address);
        auto bus = QDBusConnection::connectToBus(QString::fromUtf8(address), u"login-fixture"_s); QVERIFY(bus.isConnected());
        QVERIFY(bus.registerService(u"org.freedesktop.login1"_s));
        QThread thread;
        auto *fixture = new LoginFixture;
        fixture->moveToThread(&thread);
        connect(&thread, &QThread::finished, fixture, &QObject::deleteLater);
        thread.start();
        bool registered = false;
        QMetaObject::invokeMethod(fixture, [&] { registered = bus.registerVirtualObject(u"/org/freedesktop/login1"_s, fixture, QDBusConnection::SubPath); }, Qt::BlockingQueuedConnection);
        // Cleanup must also run on a failed assertion.
        const auto cleanup = qScopeGuard([&] {
            bus.unregisterObject(u"/org/freedesktop/login1"_s, QDBusConnection::UnregisterTree);
            thread.quit(); thread.wait(); process.terminate(); if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(3000); }
        });
        QVERIFY(registered);
        auto snapshot = VirtualSessionLogin::read(2345); QVERIFY(snapshot);
        QCOMPARE(snapshot->desktop, u"krdp-fixture"_s);
        QVERIFY(snapshot->matches(1000, 2345, u"c42"_s, u"/run/user/1000"_s));
        const auto original = fixture->session;
        const auto originalUser = fixture->user;
        for (const auto &key : original.keys()) {
            QMetaObject::invokeMethod(fixture, [&] { fixture->session = original; fixture->session.remove(key); }, Qt::BlockingQueuedConnection);
            QVERIFY2(!VirtualSessionLogin::read(2345), qPrintable(key));
        }
        QMetaObject::invokeMethod(fixture, [&] { fixture->session = original; fixture->session[u"VTNr"_s] = u"0"_s; }, Qt::BlockingQueuedConnection);
        QVERIFY(!VirtualSessionLogin::read(2345));
        QMetaObject::invokeMethod(fixture, [&] { fixture->session = original; fixture->user[u"UID"_s] = quint32(1001); }, Qt::BlockingQueuedConnection);
        QVERIFY(!VirtualSessionLogin::read(2345));
        for (const auto &key : originalUser.keys()) {
            QMetaObject::invokeMethod(fixture, [&] { fixture->user = originalUser; fixture->user.remove(key); }, Qt::BlockingQueuedConnection);
            QVERIFY(!VirtualSessionLogin::read(2345));
            QMetaObject::invokeMethod(fixture, [&] { fixture->user = originalUser; fixture->user[key] = true; }, Qt::BlockingQueuedConnection);
            QVERIFY(!VirtualSessionLogin::read(2345));
        }
        for (const auto &key : {u"Seat"_s, u"User"_s}) {
            QMetaObject::invokeMethod(fixture, [&] {
                fixture->user = originalUser; fixture->session = original;
                fixture->session[key] = key == u"Seat"_s ? ownerTuple() : seatTuple();
            }, Qt::BlockingQueuedConnection);
            QVERIFY(!VirtualSessionLogin::read(2345));
        }
        QMetaObject::invokeMethod(fixture, [&] {
            fixture->session = original;
            QDBusArgument invalidSeat; invalidSeat.beginStructure();
            invalidSeat << QString() << QDBusObjectPath(u"/org/freedesktop/login1/seat/seat0"_s);
            invalidSeat.endStructure(); fixture->session[u"Seat"_s] = QVariant::fromValue(invalidSeat);
        }, Qt::BlockingQueuedConnection);
        QVERIFY(!VirtualSessionLogin::read(2345));
        for (const auto &stage : {u"session"_s, u"user"_s}) {
            QMetaObject::invokeMethod(fixture, [&] { fixture->session = original; fixture->failInterface = stage; }, Qt::BlockingQueuedConnection);
            QVERIFY(!VirtualSessionLogin::read(2345));
        }
        QMetaObject::invokeMethod(fixture, [&] { fixture->fail = true; }, Qt::BlockingQueuedConnection);
        QVERIFY(!VirtualSessionLogin::read(2345));
        QVERIFY(!VirtualSessionLogin::read(0));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionLoginTest)
#include "VirtualSessionLoginTest.moc"
