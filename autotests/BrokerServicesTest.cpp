// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerservices.h"
#include <QTest>
using namespace Qt::StringLiterals;
class FakeBrokerServices : public BrokerServiceTransport {
public:
    using BrokerServiceTransport::BrokerServiceTransport;
    std::array<BrokerServiceState, 2> states{{{true, u"loaded"_s, u"active"_s, u"running"_s, u"enabled"_s, 42},
        {true, u"loaded"_s, u"inactive"_s, u"dead"_s, u"disabled"_s, 0}}};
    std::array<QString, 2> errors;
    QList<int> queried;
    QList<QPair<int, Operation>> operated;
    Done pending;
    QList<QPair<int, QueryDone>> queries;
    bool defer = false;
    void query(int route, QueryDone done) override {
        queried.append(route);
        if (defer) queries.append({route, std::move(done)});
        else done(states[route], errors[route]);
    }
    void operate(int route, Operation op, Done done) override { operated.append({route, op}); pending = std::move(done); }
    void complete(const QString &error = {}) { auto done = std::move(pending); done(error); }
    void completeQuery() { auto [route, done] = queries.takeFirst(); done(states[route], errors[route]); }
};
class BrokerServicesTest : public QObject {
    Q_OBJECT
private:
    static QVariantMap row(const BrokerServices &model, int route) { return model.services()[route].toMap(); }
private Q_SLOTS:
    void openingDoesNotMutateOrQuery() {
        FakeBrokerServices transport; BrokerServices model(&transport);
        QCOMPARE(model.services().size(), 2); QVERIFY(!model.busy()); QVERIFY(transport.queried.isEmpty()); QVERIFY(transport.operated.isEmpty());
        QVERIFY(!row(model, 0)[u"known"_s].toBool()); QVERIFY(!model.perform(u"console"_s, u"start"_s));
        QCOMPARE(BrokerServiceTransport::unit(0), u"farside-console-host.service"_s);
        QCOMPARE(BrokerServiceTransport::unit(1), u"farside-virtual-host.service"_s); QVERIFY(BrokerServiceTransport::unit(2).isEmpty());
    }
    void independentStateAndExactActions() {
        FakeBrokerServices transport; BrokerServices model(&transport); model.refresh();
        QVERIFY(row(model, 0)[u"canStop"_s].toBool()); QVERIFY(!row(model, 0)[u"canStart"_s].toBool());
        QVERIFY(row(model, 1)[u"canStart"_s].toBool()); QVERIFY(!row(model, 1)[u"autostart"_s].toBool());
        for (const auto &route : {u"user"_s, u"stock"_s, u"farside-console-host.service"_s, u"console; reboot"_s}) QVERIFY(!model.perform(route, u"start"_s));
        QVERIFY(!model.perform(u"virtual"_s, u"mask"_s)); QVERIFY(!model.perform(u"virtual"_s, u"disable"_s)); QVERIFY(transport.operated.isEmpty());
        QVERIFY(model.perform(u"virtual"_s, u"start"_s)); QCOMPARE(transport.operated.last().first, 1); QCOMPARE(transport.operated.last().second, BrokerServiceTransport::Start);
        QVERIFY(model.busy()); QVERIFY(!model.perform(u"console"_s, u"stop"_s)); QVERIFY(!row(model, 0)[u"canStop"_s].toBool());
        QCOMPARE(row(model, 1)[u"activeState"_s].toString(), u"inactive"_s);
        transport.states[1].activeState=u"active"_s; transport.states[1].mainPid=99; transport.complete();
        QVERIFY(!model.busy()); QCOMPARE(row(model, 1)[u"mainPid"_s].toUInt(), 99u); QCOMPARE(row(model, 0)[u"mainPid"_s].toUInt(), 42u);
    }
    void deniedOrFailedJobsReadActualStateAndKeepError() {
        FakeBrokerServices transport; BrokerServices model(&transport); model.refresh();
        QVERIFY(model.perform(u"console"_s, u"restart"_s)); transport.complete(u"Administrator authorization was cancelled"_s);
        QVERIFY(!model.busy()); QCOMPARE(row(model,0)[u"activeState"_s].toString(), u"active"_s);
        QVERIFY(row(model,0)[u"error"_s].toString().contains(u"cancelled"_s)); model.refresh(false);
        QVERIFY(row(model,0)[u"error"_s].toString().contains(u"cancelled"_s)); model.refresh(); QVERIFY(row(model,0)[u"error"_s].toString().isEmpty());
        QVERIFY(model.perform(u"console"_s, u"restart"_s)); transport.states[0].activeState=u"failed"_s; transport.complete(u"Job failed"_s);
        QCOMPARE(row(model,0)[u"activeState"_s].toString(), u"failed"_s); QVERIFY(row(model,0)[u"canStart"_s].toBool());
    }
    void bootStateReadbackAfterSuccessAndPartialFailure() {
        FakeBrokerServices transport; BrokerServices model(&transport); model.refresh();
        QVERIFY(model.perform(u"virtual"_s, u"enable"_s)); QVERIFY(!row(model,1)[u"autostart"_s].toBool());
        transport.states[1].unitFileState=u"enabled"_s;
        Q_EMIT transport.changed(); transport.complete(u"Startup setting changed, but reload failed"_s);
        QVERIFY(row(model,1)[u"autostart"_s].toBool()); QVERIFY(row(model,1)[u"error"_s].toString().contains(u"reload failed"_s));
        QVERIFY(model.perform(u"virtual"_s, u"disable"_s)); transport.states[1].unitFileState=u"disabled"_s; transport.complete();
        QVERIFY(!row(model,1)[u"autostart"_s].toBool()); QCOMPARE(transport.operated.last().second, BrokerServiceTransport::Disable);
        transport.states[1].unitFileState=u"enabled-runtime"_s; model.refresh();
        QVERIFY(!row(model,1)[u"autostart"_s].toBool()); QVERIFY(row(model,1)[u"canAutostart"_s].toBool());
        QVERIFY(!model.perform(u"virtual"_s, u"disable"_s)); QVERIFY(model.perform(u"virtual"_s, u"enable"_s)); transport.complete();
    }
    void unsafeAndMissingStates_data() {
        QTest::addColumn<QString>("load"); QTest::addColumn<QString>("file"); QTest::addColumn<bool>("known");
        QTest::newRow("missing") << u"not-found"_s << QString() << true;
        QTest::newRow("masked") << u"masked"_s << u"masked"_s << true;
        QTest::newRow("runtime-mask") << u"loaded"_s << u"masked-runtime"_s << true;
        QTest::newRow("bad-unit") << u"bad-setting"_s << u"disabled"_s << true;
        QTest::newRow("unknown") << QString() << QString() << false;
        QTest::newRow("unknown-file-state") << u"loaded"_s << u"new-unrecognized-state"_s << true;
    }
    void unsafeAndMissingStates() {
        QFETCH(QString,load); QFETCH(QString,file); QFETCH(bool,known);
        FakeBrokerServices transport; transport.states[1]={known,load,u"inactive"_s,u"dead"_s,file,0};
        BrokerServices model(&transport); model.refresh();
        for (const auto &op : {u"start"_s,u"stop"_s,u"restart"_s,u"enable"_s,u"disable"_s}) QVERIFY(!model.perform(u"virtual"_s,op));
        QVERIFY(transport.operated.isEmpty());
    }
    void queryingDefersOperationsAndCoalescesRefresh() {
        FakeBrokerServices transport; transport.defer=true; BrokerServices model(&transport); model.refresh();
        QCOMPARE(transport.queries.size(),2); QVERIFY(model.busy()); model.refresh(); model.refresh(); Q_EMIT transport.changed();
        QVERIFY(!model.perform(u"console"_s,u"stop"_s)); QCOMPARE(transport.queries.size(),2);
        transport.completeQuery(); transport.completeQuery(); QCOMPARE(transport.queries.size(),2);
        transport.completeQuery(); transport.completeQuery(); QVERIFY(!model.busy());
        QVERIFY(model.perform(u"console"_s,u"stop"_s)); transport.complete(); QVERIFY(model.busy());
        transport.states[0]={}; transport.errors[0]=u"System manager unavailable"_s; transport.completeQuery();
        QVERIFY(!model.busy()); QVERIFY(!row(model,0)[u"known"_s].toBool()); QVERIFY(!row(model,0)[u"canStop"_s].toBool());
    }
    void staticUnitCanStartWithoutOfferingBootChanges() {
        FakeBrokerServices transport; transport.states[1].unitFileState=u"static"_s; BrokerServices model(&transport); model.refresh();
        QVERIFY(row(model,1)[u"canStart"_s].toBool()); QVERIFY(!row(model,1)[u"canAutostart"_s].toBool());
        QVERIFY(!model.perform(u"virtual"_s,u"enable"_s)); QVERIFY(model.perform(u"virtual"_s,u"start"_s)); transport.complete();
    }
    void pendingCallbacksDoNotUseDeletedModel() {
        FakeBrokerServices transport; auto *model=new BrokerServices(&transport); model->refresh(); QVERIFY(model->perform(u"virtual"_s,u"start"_s));
        delete model; transport.complete();
        transport.defer=true; model=new BrokerServices(&transport); model->refresh(); delete model; transport.completeQuery(); transport.completeQuery();
    }
};
QTEST_GUILESS_MAIN(BrokerServicesTest)
#include "BrokerServicesTest.moc"
