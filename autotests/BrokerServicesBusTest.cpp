// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Run only on Sol under dbus-run-session, never on Hal or the user's real bus.
#include "brokerservices.h"
#include <QDBusVirtualObject>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QTest>
#include <memory>
using namespace Qt::StringLiterals;
struct UnitFileChange { QString kind, path, source; };
Q_DECLARE_METATYPE(UnitFileChange)
Q_DECLARE_METATYPE(QList<UnitFileChange>)
QDBusArgument &operator<<(QDBusArgument &a,const UnitFileChange &v) { a.beginStructure(); a<<v.kind<<v.path<<v.source; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a,UnitFileChange &v) { a.beginStructure(); a>>v.kind>>v.path>>v.source; a.endStructure(); return a; }
class PrivateSystemManager : public QDBusVirtualObject {
public:
    explicit PrivateSystemManager(const QDBusConnection &connection) : bus(connection) {}
    QDBusConnection bus;
    std::array<BrokerServiceState,2> states{{{true,u"loaded"_s,u"active"_s,u"running"_s,u"enabled"_s,42},
        {true,u"loaded"_s,u"inactive"_s,u"dead"_s,u"disabled"_s,0}}};
    QStringList methods;
    QVariantList lastArguments;
    bool lastAuthorized=false, early=false, hold=false, denied=false, reloadFails=false, malformed=false, alreadySubscribed=false;
    QString result=u"done"_s;
    uint counter=0;
    QDBusMessage held;
    QString heldUnit, heldPath;
    QString introspect(const QString &) const override { return {}; }
    void signalJob(const QString &path,const QString &unit,const QString &result) {
        auto signal=QDBusMessage::createSignal(u"/org/freedesktop/systemd1"_s,u"org.freedesktop.systemd1.Manager"_s,u"JobRemoved"_s);
        signal.setArguments({counter,QVariant::fromValue(QDBusObjectPath(path)),unit,result}); QVERIFY(bus.send(signal));
    }
    void completeHeld(const QString &result=u"done"_s) {
        signalJob(heldPath,heldUnit,result); QVERIFY(bus.send(held.createReply(QVariant::fromValue(QDBusObjectPath(heldPath)))));
        held=QDBusMessage();
    }
    bool handleMessage(const QDBusMessage &message,const QDBusConnection &connection) override {
        const auto args=message.arguments(); const auto method=message.member(); methods.append(method);
        const auto reply=[&](const QVariantList &values) { return connection.send(message.createReply(values)); };
        if (message.interface()==u"org.freedesktop.DBus.Properties"_s) {
            const int i=message.path().endsWith(u"/console"_s)?0:1; const auto &s=states[i];
            if(method==u"GetAll"_s) return reply({QVariantMap{{u"LoadState"_s,s.loadState},{u"ActiveState"_s,s.activeState},{u"SubState"_s,s.subState}}});
            if(method==u"Get"_s) return reply({QVariant::fromValue(QDBusVariant(malformed?QVariant(u"not-a-pid"_s):QVariant(s.mainPid)))});
        }
        if(method==u"LoadUnit"_s || method==u"GetUnitFileState"_s) {
            const int i=args.value(0).toString()==BrokerServiceTransport::unit(0)?0:args.value(0).toString()==BrokerServiceTransport::unit(1)?1:-1;
            if(i<0) return connection.send(message.createErrorReply(u"org.freedesktop.systemd1.NoSuchUnit"_s,u"Unknown unit"_s));
            if(method==u"LoadUnit"_s) return reply({QVariant::fromValue(QDBusObjectPath(i==0?u"/org/freedesktop/systemd1/unit/console"_s:u"/org/freedesktop/systemd1/unit/virtual"_s))});
            return reply({states[i].unitFileState});
        }
        if(method==u"Subscribe"_s) {
            if(alreadySubscribed) return connection.send(message.createErrorReply(u"org.freedesktop.systemd1.AlreadySubscribed"_s,u"Client is already subscribed"_s));
            return reply({});
        }
        lastArguments=args; lastAuthorized=message.isInteractiveAuthorizationAllowed();
        if(denied) return connection.send(message.createErrorReply(u"org.freedesktop.DBus.Error.AccessDenied"_s,u"Administrator authorization cancelled"_s));
        if(method==u"Reload"_s) {
            if(reloadFails) return connection.send(message.createErrorReply(u"org.freedesktop.DBus.Error.AccessDenied"_s,u"Reload denied"_s));
            return reply({});
        }
        if(method==u"EnableUnitFiles"_s || method==u"DisableUnitFiles"_s) {
            const auto units=qdbus_cast<QStringList>(args[0]);
            if(units.size()!=1 || (units[0]!=BrokerServiceTransport::unit(0) && units[0]!=BrokerServiceTransport::unit(1))) return false;
            const int i=units[0]==BrokerServiceTransport::unit(0)?0:1;
            if(args[1].toBool() || (method==u"EnableUnitFiles"_s && args[2].toBool())) return false;
            states[i].unitFileState=method==u"EnableUnitFiles"_s?u"enabled"_s:u"disabled"_s;
            const auto changes=QVariant::fromValue(QList<UnitFileChange>{});
            return reply(method==u"EnableUnitFiles"_s?QVariantList{true,changes}:QVariantList{changes});
        }
        if(method!=u"StartUnit"_s && method!=u"StopUnit"_s && method!=u"RestartUnit"_s) return false;
        const auto name=args.value(0).toString(); const int i=name==BrokerServiceTransport::unit(0)?0:name==BrokerServiceTransport::unit(1)?1:-1;
        if(i<0 || args.value(1).toString()!=u"replace"_s) return false;
        const QString path=u"/org/freedesktop/systemd1/job/"_s+QString::number(++counter);
        if(result==u"done"_s) {
            states[i].activeState=method==u"StopUnit"_s?u"inactive"_s:u"active"_s;
            states[i].subState=method==u"StopUnit"_s?u"dead"_s:u"running"_s;
            states[i].mainPid=method==u"StopUnit"_s?0:100+counter;
        }
        if(hold) { message.setDelayedReply(true); held=message; heldUnit=name; heldPath=path; return true; }
        if(early) {
            message.setDelayedReply(true); signalJob(path,name,result);
            QTimer::singleShot(30,this,[this,message,path] { bus.send(message.createReply(QVariant::fromValue(QDBusObjectPath(path)))); }); return true;
        }
        const bool sent=reply({QVariant::fromValue(QDBusObjectPath(path))});
        QTimer::singleShot(40,this,[this,path,name] { signalJob(path,name,result); }); return sent;
    }
};
class BrokerServicesBusTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QDBusConnection> server,client;
    std::unique_ptr<PrivateSystemManager> manager;
    static QVariantMap row(const BrokerServices &model,int index) { return model.services()[index].toMap(); }
private Q_SLOTS:
    void initTestCase() {
        QVERIFY2(qEnvironmentVariable("FARSIDE_SERVICE_PRIVATE_BUS")==u"1"_s,"Sol private dbus-run-session only");
        qDBusRegisterMetaType<UnitFileChange>(); qDBusRegisterMetaType<QList<UnitFileChange>>();
        server=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(QDBusConnection::SessionBus,u"farside-service-test-server"_s));
        client=std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(QDBusConnection::SessionBus,u"farside-service-test-client"_s));
        QVERIFY(server->isConnected()); QVERIFY(client->isConnected()); QVERIFY(server->registerService(u"org.freedesktop.systemd1"_s));
        manager=std::make_unique<PrivateSystemManager>(*server);
        QVERIFY(server->registerVirtualObject(u"/org/freedesktop/systemd1"_s,manager.get(),QDBusConnection::SubPath));
    }
    void init() { manager->methods.clear(); manager->heldPath.clear(); manager->heldUnit.clear(); manager->early=manager->hold=manager->denied=manager->reloadFails=manager->malformed=manager->alreadySubscribed=false; manager->result=u"done"_s;
        manager->states={{{true,u"loaded"_s,u"active"_s,u"running"_s,u"enabled"_s,42},{true,u"loaded"_s,u"inactive"_s,u"dead"_s,u"disabled"_s,0}}}; }
    void readFixedUnitsAndRejectIncompleteReply() {
        SystemBrokerServiceTransport transport(*client); BrokerServices model(&transport); model.refresh(); QTRY_VERIFY(!model.busy());
        QVERIFY2(row(model,0)[u"known"_s].toBool(),qPrintable(row(model,0)[u"error"_s].toString())); QCOMPARE(row(model,0)[u"mainPid"_s].toUInt(),42u);
        QVERIFY(row(model,1)[u"canStart"_s].toBool()); QVERIFY(!manager->methods.contains(u"StartUnit"_s));
        manager->malformed=true; model.refresh(); QTRY_VERIFY(!model.busy()); QVERIFY(!row(model,0)[u"known"_s].toBool()); QVERIFY(!row(model,1)[u"canStart"_s].toBool());
    }
    void startJob_data() { QTest::addColumn<bool>("early"); QTest::newRow("signal-before-reply")<<true; QTest::newRow("signal-after-reply")<<false; }
    void startJob() {
        QFETCH(bool,early); manager->early=early; SystemBrokerServiceTransport transport(*client); BrokerServices model(&transport); model.refresh(); QTRY_VERIFY(!model.busy());
        QVERIFY(model.perform(u"virtual"_s,u"start"_s)); QVERIFY(model.busy()); QTRY_VERIFY(!model.busy()); QVERIFY2(row(model,1)[u"error"_s].toString().isEmpty(),qPrintable(row(model,1)[u"error"_s].toString()));
        QCOMPARE(row(model,1)[u"activeState"_s].toString(),u"active"_s); QVERIFY(manager->lastAuthorized); QCOMPARE(manager->lastArguments,QVariantList({BrokerServiceTransport::unit(1),u"replace"_s}));
        QVERIFY(manager->methods.indexOf(u"Subscribe"_s)<manager->methods.indexOf(u"StartUnit"_s));
        QVERIFY(model.perform(u"virtual"_s,u"restart"_s)); QTRY_VERIFY(!model.busy()); QCOMPARE(manager->methods.count(u"Subscribe"_s),1);
    }
    void unrelatedAndFailedJobs() {
        manager->hold=true; SystemBrokerServiceTransport transport(*client); BrokerServices model(&transport); model.refresh(); QTRY_VERIFY(!model.busy());
        QVERIFY(model.perform(u"console"_s,u"stop"_s)); QTRY_VERIFY(!manager->heldPath.isEmpty());
        manager->signalJob(u"/org/freedesktop/systemd1/job/9999"_s,manager->heldUnit,u"done"_s);
        manager->signalJob(manager->heldPath,BrokerServiceTransport::unit(1),u"done"_s); QTest::qWait(50); QVERIFY(model.busy());
        manager->completeHeld(u"failed"_s); QTRY_VERIFY(!model.busy()); QVERIFY(row(model,0)[u"error"_s].toString().contains(u"failed"_s));
    }
    void deniedAuthorizationHasNoOptimisticState() {
        manager->denied=true; SystemBrokerServiceTransport transport(*client); BrokerServices model(&transport); model.refresh(); QTRY_VERIFY(!model.busy());
        QVERIFY(model.perform(u"console"_s,u"stop"_s)); QTRY_VERIFY(!model.busy()); QCOMPARE(row(model,0)[u"activeState"_s].toString(),u"active"_s);
        QVERIFY(row(model,0)[u"error"_s].toString().contains(u"cancelled"_s)); QVERIFY(manager->lastAuthorized);
        QVERIFY(model.perform(u"virtual"_s,u"enable"_s)); QTRY_VERIFY(!model.busy()); QVERIFY(!row(model,1)[u"autostart"_s].toBool()); QVERIFY(!manager->methods.contains(u"Reload"_s));
    }
    void persistentStartupAndPartialReloadFailure() {
        SystemBrokerServiceTransport transport(*client); BrokerServices model(&transport); model.refresh(); QTRY_VERIFY(!model.busy());
        QVERIFY(model.perform(u"virtual"_s,u"enable"_s)); QTRY_VERIFY(!model.busy()); QVERIFY(row(model,1)[u"autostart"_s].toBool()); QVERIFY(manager->methods.contains(u"Reload"_s));
        manager->reloadFails=true; QVERIFY(model.perform(u"virtual"_s,u"disable"_s)); QTRY_VERIFY(!model.busy()); QVERIFY(!row(model,1)[u"autostart"_s].toBool()); QVERIFY(row(model,1)[u"error"_s].toString().contains(u"reload failed"_s));
    }
    void sharedBusSubscriptionIsAccepted() {
        manager->alreadySubscribed=true; SystemBrokerServiceTransport transport(*client); BrokerServices model(&transport); model.refresh(); QTRY_VERIFY(!model.busy());
        QVERIFY(model.perform(u"virtual"_s,u"start"_s)); QTRY_VERIFY(!model.busy()); QVERIFY(row(model,1)[u"error"_s].toString().isEmpty());
        QCOMPARE(row(model,1)[u"activeState"_s].toString(),u"active"_s);
    }
    void managerReplacementRejectsLatePriorJobReply() {
        manager->hold=true; SystemBrokerServiceTransport transport(*client); BrokerServices model(&transport); model.refresh(); QTRY_VERIFY(!model.busy());
        QVERIFY(model.perform(u"virtual"_s,u"start"_s)); QTRY_VERIFY(!manager->heldPath.isEmpty());
        const auto oldMessage=manager->held; const auto oldPath=manager->heldPath;
        QVERIFY(server->unregisterService(u"org.freedesktop.systemd1"_s)); QVERIFY(server->registerService(u"org.freedesktop.systemd1"_s));
        QTRY_VERIFY(!model.busy()); QVERIFY(row(model,1)[u"error"_s].toString().contains(u"changed"_s));
        manager->heldPath.clear(); QVERIFY(model.perform(u"virtual"_s,u"restart"_s)); QTRY_VERIFY(!manager->heldPath.isEmpty());
        manager->signalJob(oldPath,BrokerServiceTransport::unit(1),u"failed"_s);
        QVERIFY(server->send(oldMessage.createReply(QVariant::fromValue(QDBusObjectPath(oldPath)))));
        QTest::qWait(50); QVERIFY(model.busy()); manager->completeHeld(); QTRY_VERIFY(!model.busy()); QVERIFY(row(model,1)[u"error"_s].toString().isEmpty());
    }
    void invalidUnitsAndOperationsNeverReachBus() {
        SystemBrokerServiceTransport transport(*client); int done=0; transport.query(2,[&](auto,const auto &error) { QVERIFY(!error.isEmpty()); ++done; });
        transport.operate(-1,BrokerServiceTransport::Stop,[&](const auto &error) { QVERIFY(!error.isEmpty()); ++done; });
        transport.operate(0,BrokerServiceTransport::Operation(999),[&](const auto &error) { QVERIFY(!error.isEmpty()); ++done; });
        QCOMPARE(done,3); QVERIFY(manager->methods.isEmpty());
    }
    void readOnlyActualSystemBrokers() {
        SystemBrokerServiceTransport transport(QDBusConnection::systemBus()); BrokerServices model(&transport); model.refresh(); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(),60000);
        for(int i=0;i<2;++i) { const auto state=row(model,i); QVERIFY2(state[u"known"_s].toBool(),qPrintable(state[u"error"_s].toString())); QCOMPARE(state[u"activeState"_s].toString(),u"active"_s); QVERIFY(state[u"mainPid"_s].toUInt()>0); }
        QVERIFY(manager->methods.isEmpty()); // No system service mutation method called.
    }
    void cleanupTestCase() {
        server->unregisterObject(u"/org/freedesktop/systemd1"_s,QDBusConnection::UnregisterTree); server->unregisterService(u"org.freedesktop.systemd1"_s); manager.reset();
        QDBusConnection::disconnectFromBus(u"farside-service-test-server"_s); QDBusConnection::disconnectFromBus(u"farside-service-test-client"_s);
    }
};
QTEST_GUILESS_MAIN(BrokerServicesBusTest)
#include "BrokerServicesBusTest.moc"
