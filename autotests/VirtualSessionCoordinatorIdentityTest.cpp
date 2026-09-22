// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionCoordinatorIdentity.h"
#include <QTest>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QThread>
#include <QUuid>
#include <functional>
#include <sys/wait.h>
#include <unistd.h>

using namespace Qt::StringLiterals;
using Identity = KRdp::VirtualSessionCoordinatorIdentity;
namespace {
const QString name = u"org.freedesktop.systemd1"_s;
const QString path = u"/org/freedesktop/systemd1/unit/krdp_2dmaintenance_2dvalidate_2eservice"_s;
class Fixture : public QDBusVirtualObject {
public:
    QVariantMap unit{{u"Id"_s, u"krdp-maintenance-validate.service"_s}, {u"ActiveState"_s, u"activating"_s},
        {u"SubState"_s, u"start"_s}, {u"InvocationID"_s, QUuid::createUuid().toRfc4122()}};
    QVariantMap service{{u"MainPID"_s, quint32(getpid())}, {u"Type"_s, u"oneshot"_s}, {u"Delegate"_s, false}};
    QString resultPath = path;
    bool badPathType = false, badPropertiesType = false, extra = false, error = false;
    int delay = 0, calls = 0;
    std::function<void(const QDBusMessage &, const QDBusConnection &)> callback;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &m, const QDBusConnection &bus) override {
        ++calls;
        if (callback) callback(m, bus);
        if (delay) QThread::msleep(delay);
        QList<QVariant> args;
        if (error) { bus.send(m.createErrorReply(u"org.freedesktop.DBus.Error.Failed"_s, u"fixture"_s)); return true; }
        if (m.path() == u"/org/freedesktop/systemd1"_s && m.interface() == name + u".Manager"_s
            && m.member() == u"GetUnitByPID"_s && m.arguments() == QList<QVariant>{quint32(getpid())}) {
            args = {badPathType ? QVariant(resultPath) : QVariant::fromValue(QDBusObjectPath(resultPath))};
        } else if (m.path() == path && m.interface() == u"org.freedesktop.DBus.Properties"_s && m.member() == u"GetAll"_s
                   && m.arguments().size() == 1) {
            const auto iface = m.arguments().first().toString();
            if (iface != name + u".Unit"_s && iface != name + u".Service"_s) return false;
            args = {badPropertiesType ? QVariant(u"wrong"_s) : QVariant(iface.endsWith(u".Unit"_s) ? unit : service)};
        } else return false;
        if (extra) args.append(true);
        bus.send(m.createReply(args)); return true;
    }
};
// Every connection is to this disposable private daemon; no systemBus() calls.
struct Harness {
    QProcess daemon;
    QString serverName = QUuid::createUuid().toString(), clientName = QUuid::createUuid().toString();
    QDBusConnection server{serverName}, client{clientName};
    QThread thread;
    Fixture *fixture = nullptr;
    bool ready = false;
    Harness() {
        daemon.start(u"dbus-daemon"_s, {u"--session"_s, u"--nofork"_s, u"--print-address=1"_s});
        if (!daemon.waitForStarted(3000) || !daemon.waitForReadyRead(3000)) return;
        const auto address = QString::fromUtf8(daemon.readLine().trimmed());
        if (!address.startsWith(u"unix:"_s)) return;
        server = QDBusConnection::connectToBus(address, serverName);
        client = QDBusConnection::connectToBus(address, clientName);
        if (!server.isConnected() || !client.isConnected() || !server.registerService(name)) return;
        fixture = new Fixture;
        fixture->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, fixture, &QObject::deleteLater);
        thread.start();
        change([&](auto &) { ready = server.registerVirtualObject(u"/org/freedesktop/systemd1"_s, fixture, QDBusConnection::SubPath); });
    }
    void change(const std::function<void(Fixture &)> &fn) {
        QMetaObject::invokeMethod(fixture, [&] { fn(*fixture); }, Qt::BlockingQueuedConnection);
    }
    ~Harness() {
        if (thread.isRunning()) {
            change([&](auto &) { server.unregisterObject(u"/org/freedesktop/systemd1"_s, QDBusConnection::UnregisterTree); });
            thread.quit(); thread.wait();
        }
        QDBusConnection::disconnectFromBus(clientName); QDBusConnection::disconnectFromBus(serverName);
        daemon.terminate(); if (!daemon.waitForFinished(3000)) { daemon.kill(); daemon.waitForFinished(3000); }
    }
};
}
class VirtualSessionCoordinatorIdentityTest : public QObject {
    Q_OBJECT
    static auto pin(Harness &h, int timeout = 3000) {
        return Identity::pinOnBus(h.client, QFile::symLinkTarget(u"/proc/self/exe"_s), getuid(), getpid(), true, timeout);
    }
private Q_SLOTS:
    void asciiEndpointGrammar() {
        QVERIFY(Identity::validBusId(u"0123456789abcdef0123456789abcdef"_s));
        QVERIFY(Identity::validBusId(QString(32, QLatin1Char('0'))));
        for (const auto &value : {QString(), QString(31, QLatin1Char('a')), QString(33, QLatin1Char('a')),
                                 QString(32, QLatin1Char('A')), QString(32, QLatin1Char('g')),
                                 QString(QString(31, QLatin1Char('a')) + QChar(0)), QString(QString(31, QLatin1Char('a')) + QChar(0xff10))})
            QVERIFY(!Identity::validBusId(value));
        for (const auto &value : {u":1.0"_s, u":A_-9.b.C_0-"_s, u":_.-"_s}) QVERIFY(Identity::validUniqueOwner(value));
        for (const auto &value : {QString(), u":"_s, u":a"_s, u"a.b"_s, u":.a"_s, u":a."_s, u":a..b"_s,
                                 u":a.b\n"_s, u":a.b c"_s, u":a.b/c"_s, u":a.é"_s, QString(u":a.b"_s + QChar(0))})
            QVERIFY(!Identity::validUniqueOwner(value));
    }
    void exactIdentityMoveAndRevalidate() {
        Harness h; QVERIFY(h.ready);
        auto id = pin(h); QVERIFY(id);
        QCOMPARE(id->uniqueOwner(), h.server.baseService()); QCOMPARE(id->busId().size(), 32);
        QVERIFY(!id->bootId().isEmpty()); QVERIFY(id->revalidate());
        h.change([&](auto &f) { QCOMPARE(id->invocation(), QUuid::fromRfc4122(f.unit[u"InvocationID"_s].toByteArray()).toString(QUuid::WithoutBraces)); });
        Identity moved(std::move(*id)); QVERIFY(!id->revalidate()); QVERIFY(moved.revalidate());
        auto other = pin(h); QVERIFY(other); moved = std::move(*other); QVERIFY(!other->revalidate()); QVERIFY(moved.revalidate());
        const pid_t child = fork(); QVERIFY(child >= 0);
        if (!child) _exit(moved.revalidate() ? 1 : 0);
        int status; QCOMPARE(waitpid(child, &status, 0), child); QVERIFY(WIFEXITED(status)); QCOMPARE(WEXITSTATUS(status), 0);
    }
    void malformed_data() {
        QTest::addColumn<QString>("kind");
        for (const auto *s : {"path", "path-type", "extra", "properties-type", "error", "unit", "pid", "pid-type", "type", "active", "substate",
                              "delegate", "delegate-type", "inv-short", "inv-null", "inv-type", "missing", "changing", "owner-loss"}) QTest::newRow(s) << QString::fromLatin1(s);
    }
    void malformed() {
        QFETCH(QString, kind); Harness h; QVERIFY(h.ready);
        h.change([&](auto &f) {
            if (kind == u"path"_s) f.resultPath += u"_extra"_s;
            if (kind == u"path-type"_s) f.badPathType = true;
            if (kind == u"extra"_s) f.extra = true;
            if (kind == u"properties-type"_s) f.badPropertiesType = true;
            if (kind == u"error"_s) f.error = true;
            if (kind == u"unit"_s) f.unit[u"Id"_s] = u"other.service"_s;
            if (kind == u"pid"_s) f.service[u"MainPID"_s] = quint32(getpid() + 1);
            if (kind == u"pid-type"_s) f.service[u"MainPID"_s] = QString::number(getpid());
            if (kind == u"type"_s) f.service[u"Type"_s] = u"exec"_s;
            if (kind == u"active"_s) f.unit[u"ActiveState"_s] = u"active"_s;
            if (kind == u"substate"_s) f.unit[u"SubState"_s] = u"start-post"_s;
            if (kind == u"delegate"_s) f.service[u"Delegate"_s] = true;
            if (kind == u"delegate-type"_s) f.service[u"Delegate"_s] = 0;
            if (kind == u"inv-short"_s) f.unit[u"InvocationID"_s] = QByteArray(15, 'a');
            if (kind == u"inv-null"_s) f.unit[u"InvocationID"_s] = QByteArray(16, 0);
            if (kind == u"inv-type"_s) f.unit[u"InvocationID"_s] = QUuid::createUuid().toString();
            if (kind == u"missing"_s) f.service.remove(u"Delegate"_s);
            if (kind == u"changing"_s) f.callback = [&f](const auto &m, const auto &) {
                if (m.arguments() == QList<QVariant>{QString(name + u".Service"_s)}) f.unit[u"InvocationID"_s] = QUuid::createUuid().toRfc4122();
            };
            if (kind == u"owner-loss"_s) f.callback = [](const auto &, const auto &bus) { auto connection = bus; connection.unregisterService(name); };
        });
        QVERIFY(!pin(h));
    }
    void credentialsExecutableAndDeadline() {
        Harness h; QVERIFY(h.ready);
        const auto executable = QFile::symLinkTarget(u"/proc/self/exe"_s);
        QVERIFY(!Identity::pinOnBus(h.client, executable, getuid() + 1, getpid(), true, 3000));
        QVERIFY(!Identity::pinOnBus(h.client, executable, getuid(), getpid() + 1, true, 3000));
        QVERIFY(!Identity::pinOnBus(h.client, u"/usr/bin/sleep"_s, getuid(), getpid(), true, 3000));
        QVERIFY(!pin(h, 0)); QVERIFY(!pin(h, 3001));
        h.change([](auto &f) { f.delay = 25; });
        QElapsedTimer elapsed; elapsed.start(); QVERIFY(!pin(h, 60)); QVERIFY(elapsed.elapsed() < 500);
    }
    void revalidationRejectsChangedInvocationAndDisconnect() {
        Harness h; QVERIFY(h.ready); auto id = pin(h); QVERIFY(id);
        h.change([](auto &f) { f.unit[u"InvocationID"_s] = QUuid::createUuid().toRfc4122(); });
        QVERIFY(!id->revalidate());
        auto current = pin(h); QVERIFY(current);
        h.change([&](auto &) { QVERIFY(h.server.unregisterService(name)); });
        QVERIFY(!current->revalidate());
        h.change([&](auto &) { QVERIFY(h.server.registerService(name)); });
        auto connected = pin(h); QVERIFY(connected);
        h.daemon.terminate(); QVERIFY(h.daemon.waitForFinished(3000));
        QVERIFY(!connected->revalidate()); QVERIFY(!pin(h));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionCoordinatorIdentityTest)
#include "VirtualSessionCoordinatorIdentityTest.moc"
