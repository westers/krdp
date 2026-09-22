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
#include <QElapsedTimer>
#include <functional>
using namespace Qt::StringLiterals;
using KRdp::VirtualSessionLogin;

namespace {
QVariant ownerTuple(const QString &path = u"/org/freedesktop/login1/user/_1000"_s) {
    QDBusArgument value; value.beginStructure(); value << quint32(1000) << QDBusObjectPath(path); value.endStructure();
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
    QString sessionPath = u"/org/freedesktop/login1/session/c42"_s;
    std::function<void()> beforeReply;
    int delayMs = 0;
    bool extraPathArgument = false;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
        if (beforeReply) beforeReply();
        if (delayMs) QThread::msleep(delayMs);
        if (!fail && message.path() == u"/org/freedesktop/login1"_s && message.interface() == u"org.freedesktop.login1.Manager"_s
            && message.member() == u"GetSessionByPID"_s && message.arguments() == QList<QVariant>{quint32(2345)}) {
            QList<QVariant> args{QVariant::fromValue(QDBusObjectPath(sessionPath))};
            if (extraPathArgument) args.append(true);
            bus.send(message.createReply(args));
        } else if (!fail && message.interface() == u"org.freedesktop.DBus.Properties"_s && message.member() == u"GetAll"_s
            && message.path() == sessionPath
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
            u"online"_s, {}, {}, {}, u"/run/user/1000"_s, u"session-c42.scope"_s, 1000, 2345, 0, {}, {}, {}};
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
        auto bus = QDBusConnection::connectToBus(QString::fromUtf8(address), u"login-fixture"_s); QVERIFY(bus.isConnected());
        const auto client = QDBusConnection::connectToBus(QString::fromUtf8(address), u"login-client"_s); QVERIFY(client.isConnected());
        auto replacement = QDBusConnection::connectToBus(QString::fromUtf8(address), u"login-replacement"_s); QVERIFY(replacement.isConnected());
        const auto read = [&] { return VirtualSessionLogin::read(client, 2345); };
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
            for (const auto &name : {u"login-fixture"_s, u"login-client"_s, u"login-replacement"_s}) QDBusConnection::disconnectFromBus(name);
        });
        QVERIFY(registered);
        auto snapshot = read(); QVERIFY(snapshot);
        QCOMPARE(snapshot->uniqueOwner, bus.baseService());
        QVERIFY(!snapshot->busId.isEmpty());
        QCOMPARE(snapshot->desktop, u"krdp-fixture"_s);
        QVERIFY(snapshot->matches(1000, 2345, u"c42"_s, u"/run/user/1000"_s));
        const auto original = fixture->session;
        const auto originalUser = fixture->user;
        for (const auto &key : original.keys()) {
            QMetaObject::invokeMethod(fixture, [&] { fixture->session = original; fixture->session.remove(key); }, Qt::BlockingQueuedConnection);
            QVERIFY2(!read(), qPrintable(key));
        }
        QMetaObject::invokeMethod(fixture, [&] { fixture->session = original; fixture->session[u"VTNr"_s] = u"0"_s; }, Qt::BlockingQueuedConnection);
        QVERIFY(!read());
        QMetaObject::invokeMethod(fixture, [&] { fixture->session = original; fixture->user[u"UID"_s] = quint32(1001); }, Qt::BlockingQueuedConnection);
        QVERIFY(!read());
        for (const auto &key : originalUser.keys()) {
            QMetaObject::invokeMethod(fixture, [&] { fixture->user = originalUser; fixture->user.remove(key); }, Qt::BlockingQueuedConnection);
            QVERIFY(!read());
            QMetaObject::invokeMethod(fixture, [&] { fixture->user = originalUser; fixture->user[key] = true; }, Qt::BlockingQueuedConnection);
            QVERIFY(!read());
        }
        for (const auto &key : {u"Seat"_s, u"User"_s}) {
            QMetaObject::invokeMethod(fixture, [&] {
                fixture->user = originalUser; fixture->session = original;
                fixture->session[key] = key == u"Seat"_s ? ownerTuple() : seatTuple();
            }, Qt::BlockingQueuedConnection);
            QVERIFY(!read());
        }
        QMetaObject::invokeMethod(fixture, [&] {
            fixture->session = original;
            QDBusArgument invalidSeat; invalidSeat.beginStructure();
            invalidSeat << QString() << QDBusObjectPath(u"/org/freedesktop/login1/seat/seat0"_s);
            invalidSeat.endStructure(); fixture->session[u"Seat"_s] = QVariant::fromValue(invalidSeat);
        }, Qt::BlockingQueuedConnection);
        QVERIFY(!read());
        for (const auto &stage : {u"session"_s, u"user"_s}) {
            QMetaObject::invokeMethod(fixture, [&] { fixture->session = original; fixture->failInterface = stage; }, Qt::BlockingQueuedConnection);
            QVERIFY(!read());
        }
        QMetaObject::invokeMethod(fixture, [&] { fixture->fail = true; }, Qt::BlockingQueuedConnection);
        QVERIFY(!read());
        QVERIFY(!VirtualSessionLogin::read(client, 0));
        const auto reset = [&] {
            QMetaObject::invokeMethod(fixture, [&] {
                fixture->session = original; fixture->user = originalUser; fixture->fail = false;
                fixture->failInterface.clear(); fixture->sessionPath = u"/org/freedesktop/login1/session/c42"_s;
                fixture->beforeReply = {}; fixture->delayMs = 0; fixture->extraPathArgument = false;
            }, Qt::BlockingQueuedConnection);
        };
        const QList<std::function<void()>> inconsistent{
            [&] { fixture->session[u"Leader"_s] = quint32(2346); },
            [&] { fixture->session[u"Leader"_s] = 2345; }, // signed is not uint32
            [&] { fixture->session[u"Id"_s] = QString(); },
            [&] { fixture->session[u"Id"_s] = u"c43"_s; },
            [&] { fixture->session[u"Scope"_s] = u"session-c43.scope"_s; },
            [&] { fixture->sessionPath += u"/nested"_s; },
            [&] { fixture->session[u"User"_s] = ownerTuple(u"/org/freedesktop/login1/user/_1001"_s); },
            [&] { fixture->user[u"RuntimePath"_s] = u"/run/user/1001"_s; },
            [&] { fixture->extraPathArgument = true; },
        };
        for (const auto &mutate : inconsistent) {
            reset(); QMetaObject::invokeMethod(fixture, mutate, Qt::BlockingQueuedConnection);
            QVERIFY(!read());
        }
        for (const auto &pair : {qMakePair(u"985"_s, u"_3985"_s), qMakePair(u"c_4-2"_s, u"c_5f4_2d2"_s)}) {
            reset();
            QMetaObject::invokeMethod(fixture, [&] {
                fixture->session[u"Id"_s] = pair.first;
                fixture->session[u"Scope"_s] = QString(u"session-"_s + pair.first + u".scope"_s);
                fixture->sessionPath = u"/org/freedesktop/login1/session/"_s + pair.second;
            }, Qt::BlockingQueuedConnection);
            QVERIFY(read());
        }
        reset();
        // Exercise the real algorithm and parsing with actual wire replies;
        // intercept only the transport boundary to inject generation changes.
        int loginCalls = 0;
        bool destinationsValid = true;
        const auto trackedCall = [&](const QDBusMessage &message, int timeout) {
            destinationsValid &= !message.autoStartService();
            if (message.service() != u"org.freedesktop.DBus"_s) {
                ++loginCalls; destinationsValid &= message.service() == bus.baseService();
            }
            return client.call(message, QDBus::Block, timeout);
        };
        QVERIFY(VirtualSessionLogin::readWithCalls(2345, 1000, trackedCall, [&] { return client.isConnected(); }));
        QCOMPARE(loginCalls, 3); QVERIFY(destinationsValid);
        // A real owner switch at each object-reply boundary must refuse even
        // though the old owner's unique connection remains alive and replies.
        for (int boundary = 1; boundary <= 3; ++boundary) {
            reset(); int calls = 0; bool switched = false;
            QMetaObject::invokeMethod(fixture, [&] {
                fixture->beforeReply = [&] {
                    if (++calls == boundary) switched = bus.unregisterService(u"org.freedesktop.login1"_s)
                        && replacement.registerService(u"org.freedesktop.login1"_s);
                };
            }, Qt::BlockingQueuedConnection);
            QVERIFY(!read());
            reset(); QVERIFY(switched); QCOMPARE(calls, boundary);
            QVERIFY(replacement.unregisterService(u"org.freedesktop.login1"_s));
            QVERIFY(bus.registerService(u"org.freedesktop.login1"_s));
        }
        // A second actual bus supplies a changed GetId while the owner response
        // remains the same unique name. No invented parsing implementation.
        QProcess otherDaemon;
        otherDaemon.start(daemon, {u"--session"_s, u"--nofork"_s, u"--print-address=1"_s});
        QVERIFY(otherDaemon.waitForStarted(3000)); QVERIFY(otherDaemon.waitForReadyRead(3000));
        const auto other = QDBusConnection::connectToBus(QString::fromUtf8(otherDaemon.readLine().trimmed()), u"login-other"_s);
        const auto otherCleanup = qScopeGuard([&] {
            QDBusConnection::disconnectFromBus(u"login-other"_s);
            otherDaemon.terminate(); if (!otherDaemon.waitForFinished(3000)) { otherDaemon.kill(); otherDaemon.waitForFinished(3000); }
        });
        QVERIFY(other.isConnected());
        for (int boundary = 0; boundary <= 3; ++boundary) {
            loginCalls = 0; int ids = 0;
            const auto changedBus = [&](const QDBusMessage &message, int timeout) {
                if (message.member() == u"GetId"_s && ++ids > 1 && loginCalls >= boundary)
                    return other.call(message, QDBus::Block, timeout);
                return trackedCall(message, timeout);
            };
            QVERIFY(!VirtualSessionLogin::readWithCalls(2345, 1000, changedBus, [&] { return client.isConnected(); }));
            QCOMPARE(loginCalls, boundary);
        }
        for (const auto &malformed : {u"GetId"_s, u"GetNameOwner"_s}) {
            const auto invalidIdentity = [&](const QDBusMessage &message, int timeout) {
                if (message.member() != malformed) return trackedCall(message, timeout);
                auto substitute = QDBusMessage::createMethodCall(u"org.freedesktop.DBus"_s, u"/org/freedesktop/DBus"_s,
                    u"org.freedesktop.DBus"_s, malformed == u"GetId"_s ? u"GetNameOwner"_s : u"GetId"_s);
                if (malformed == u"GetId"_s) substitute.setArguments({u"org.freedesktop.login1"_s});
                return client.call(substitute, QDBus::Block, timeout);
            };
            QVERIFY(!VirtualSessionLogin::readWithCalls(2345, 1000, invalidIdentity, [&] { return client.isConnected(); }));
        }
        // Exact signatures/arity, using real daemon replies: as, u and an
        // empty reply must not be accepted as either identity string.
        for (const auto &target : {u"GetId"_s, u"GetNameOwner"_s, u"GetSessionByPID"_s, u"GetAll"_s}) {
            for (const auto &method : {u"ListNames"_s, u"GetConnectionUnixUser"_s, u"RemoveMatch"_s}) {
                const auto malformedWire = [&](const QDBusMessage &message, int timeout) {
                    if (message.member() != target) return trackedCall(message, timeout);
                    auto substitute = QDBusMessage::createMethodCall(u"org.freedesktop.DBus"_s, u"/org/freedesktop/DBus"_s,
                        u"org.freedesktop.DBus"_s, method);
                    if (method == u"GetConnectionUnixUser"_s) substitute.setArguments({client.baseService()});
                    if (method == u"RemoveMatch"_s) {
                        auto add = QDBusMessage::createMethodCall(u"org.freedesktop.DBus"_s, u"/org/freedesktop/DBus"_s,
                            u"org.freedesktop.DBus"_s, u"AddMatch"_s);
                        add.setArguments({u"type='signal',interface='org.example.LoginTest'"_s});
                        client.call(add, QDBus::Block, timeout);
                        substitute.setArguments(add.arguments());
                    }
                    return client.call(substitute, QDBus::Block, timeout);
                };
                QVERIFY(!VirtualSessionLogin::readWithCalls(2345, 1000, malformedWire, [&] { return client.isConnected(); }));
            }
        }
        QVERIFY(bus.unregisterService(u"org.freedesktop.login1"_s));
        QVERIFY(!read()); // absent owner must not activate anything
        QVERIFY(bus.registerService(u"org.freedesktop.login1"_s));
        reset();
        QMetaObject::invokeMethod(fixture, [&] { fixture->delayMs = 60; }, Qt::BlockingQueuedConnection);
        loginCalls = 0; QList<int> budgets;
        QElapsedTimer timer; timer.start();
        const auto timedCall = [&](const QDBusMessage &message, int timeout) {
            budgets.append(timeout); return trackedCall(message, timeout);
        };
        QVERIFY(!VirtualSessionLogin::readWithCalls(2345, 100, timedCall, [&] { return client.isConnected(); }));
        QVERIFY(timer.elapsed() < 500); QVERIFY(loginCalls < 3);
        QVERIFY(budgets.last() < budgets.first());
        reset();
        QVERIFY(!VirtualSessionLogin::read(client, 2345, 0));
        QVERIFY(!VirtualSessionLogin::read(client, 2345, 3001));
        QVERIFY(!VirtualSessionLogin::read(QDBusConnection(u"missing-login-bus"_s), 2345));
        // Actual connection loss after a reply, not only an initial false flag.
        const auto disconnectedCall = [&](const QDBusMessage &message, int timeout) {
            const auto reply = trackedCall(message, timeout);
            if (message.member() == u"GetSessionByPID"_s) {
                process.terminate(); process.waitForFinished(3000);
                QTest::qWait(20);
            }
            return reply;
        };
        QVERIFY(!VirtualSessionLogin::readWithCalls(2345, 1000, disconnectedCall, [&] { return client.isConnected(); }));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionLoginTest)
#include "VirtualSessionLoginTest.moc"
