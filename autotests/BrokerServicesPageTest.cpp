// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerservices.h"
#include <KLocalizedQmlContext>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
using namespace Qt::StringLiterals;
class PageServiceTransport : public BrokerServiceTransport {
public:
    std::array<BrokerServiceState,2> states{{{true,u"loaded"_s,u"active"_s,u"running"_s,u"enabled"_s,42},
        {true,u"loaded"_s,u"inactive"_s,u"dead"_s,u"disabled"_s,0}}};
    QList<QPair<int,Operation>> actions;
    Done pending;
    void query(int route,QueryDone done) override { done(states[route],{}); }
    void operate(int route,Operation action,Done done) override { actions.append({route,action}); pending=std::move(done); }
    void finish(const QString &error={}) { auto done=std::move(pending); done(error); }
};
class BrokerServicesPageTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void serviceControlsConfirmAndFollowActualState() {
        PageServiceTransport transport; BrokerServices administration(&transport);
        QQmlEngine engine; QStringList warnings;
        connect(&engine,&QQmlEngine::warnings,this,[&](const QList<QQmlError> &errors) { for(const auto &e:errors) warnings.append(e.toString()); });
        auto *localized=new KLocalizedQmlContext(&engine); localized->setTranslationDomain(u"kcm_farside"_s); engine.rootContext()->setContextObject(localized);
        QQmlComponent component(&engine,QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_SERVICE_TEST_PAGE",QString::fromUtf8(SERVICE_PAGE))));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"administration"_s,QVariant::fromValue(&administration)}}));
        QVERIFY2(object,qPrintable(component.errorString())); auto *page=qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
        QQuickWindow window; window.resize(1000,900); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        const auto find=[](auto &&self,QQuickItem *parent,const QString &name)->QObject* {
            if(parent->objectName()==name) return parent;
            if(auto *object=parent->findChild<QObject *>(name)) return object;
            for(auto *child:parent->childItems()) if(auto *object=self(self,child,name)) return object;
            return nullptr;
        };
        const auto item=[&](const QString &name) { return find(find,page,name); };
        QTRY_VERIFY(item(u"consoleServiceStatus"_s)); QTRY_VERIFY(item(u"virtualServiceStatus"_s));
        QCOMPARE(item(u"consoleServiceStatus"_s)->property("text").toString(),u"Running"_s);
        QCOMPARE(item(u"virtualServiceStatus"_s)->property("text").toString(),u"Stopped"_s);
        QVERIFY(!item(u"consoleServiceStart"_s)->property("enabled").toBool()); QVERIFY(item(u"virtualServiceStart"_s)->property("enabled").toBool());
        QVERIFY(item(u"consoleServiceAutostart"_s)->property("checked").toBool()); QVERIFY(transport.actions.isEmpty());
        QTest::qWait(400); const auto screenshot=qEnvironmentVariable("FARSIDE_SERVICE_SCREENSHOT"); if(!screenshot.isEmpty()) QVERIFY(window.grabWindow().save(screenshot));
        QVERIFY(QMetaObject::invokeMethod(item(u"consoleServiceStop"_s),"clicked"));
        auto *dialog=item(u"confirmBrokerServiceOperation"_s); QVERIFY(dialog); QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(transport.actions.isEmpty()); QVERIFY(QMetaObject::invokeMethod(dialog,"reject")); QVERIFY(transport.actions.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(item(u"consoleServiceRestart"_s),"clicked")); QVERIFY(QMetaObject::invokeMethod(dialog,"accept"));
        QCOMPARE(transport.actions.last().first,0); QCOMPARE(transport.actions.last().second,BrokerServiceTransport::Restart); QVERIFY(administration.busy());
        QVERIFY(!item(u"virtualServiceStart"_s)->property("enabled").toBool()); transport.finish(u"Administrator authorization cancelled"_s);
        QCOMPARE(item(u"consoleServiceStatus"_s)->property("text").toString(),u"Running"_s);
        QVERIFY(administration.services()[0].toMap()[u"error"_s].toString().contains(u"cancelled"_s));
        QVERIFY(QMetaObject::invokeMethod(item(u"virtualServiceStart"_s),"clicked")); QCOMPARE(transport.actions.last().first,1);
        transport.states[1].activeState=u"active"_s; transport.states[1].mainPid=123; transport.finish();
        QCOMPARE(item(u"virtualServiceStatus"_s)->property("text").toString(),u"Running"_s);
        QVERIFY(QMetaObject::invokeMethod(item(u"virtualServiceAutostart"_s),"clicked")); QCOMPARE(transport.actions.last().second,BrokerServiceTransport::Enable);
        QVERIFY(!item(u"virtualServiceAutostart"_s)->property("checked").toBool()); transport.finish(u"Administrator authorization cancelled"_s);
        QVERIFY(!item(u"virtualServiceAutostart"_s)->property("checked").toBool());
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u"\n"_s))); page->setParentItem(nullptr);
    }
};
QTEST_MAIN(BrokerServicesPageTest)
#include "BrokerServicesPageTest.moc"
