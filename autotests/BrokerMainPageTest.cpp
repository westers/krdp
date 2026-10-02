// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerhostsettings.h"
#include "brokerauthenticationsettings.h"
#include "brokerpreferences.h"
#include "brokerservices.h"
#include <KLocalizedQmlContext>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
using namespace Qt::StringLiterals;
class MainNavigation : public QObject {
    Q_OBJECT
public:
    QString page, copied;QVariantMap initial;
    Q_INVOKABLE void push(const QString &name,const QVariantMap &properties={}) { page=name;initial=properties; }
    Q_INVOKABLE void copyAddressToClipboard(const QString &text) { copied=text; }
};
class MainTransport : public BrokerServiceTransport {
    Q_OBJECT
public:
    BrokerServiceState state{true,u"loaded"_s,u"active"_s,u"running"_s,u"enabled"_s,42};
    int mutations=0, lastRoute=-1;
    Operation lastOperation=Start;
    void query(int,QueryDone done) override { done(state,{}); }
    void operate(int route,Operation operation,Done done) override { ++mutations;lastRoute=route;lastOperation=operation;done({}); }
};
class BrokerMainPageTest : public QObject {
    Q_OBJECT
    using Scope=BrokerHostSettings::Scope;
    static QObject *find(QQuickItem *parent,const QString &name) {
        if(parent->objectName()==name)return parent;
        if(auto *value=parent->findChild<QObject *>(name))return value;
        for(auto *child:parent->childItems())if(auto *value=find(child,name))return value;
        return nullptr;
    }
private Q_SLOTS:
    void populatedStatusSavedAndStagedData_data() {
        QTest::addColumn<QString>("state");QTest::addColumn<QString>("label");
        QTest::newRow("active")<<u"active"_s<<u"Running"_s;
        QTest::newRow("inactive")<<u"inactive"_s<<u"Stopped"_s;
        QTest::newRow("failed")<<u"failed"_s<<u"Failed"_s;
        QTest::newRow("activating")<<u"activating"_s<<u"Starting…"_s;
        QTest::newRow("deactivating")<<u"deactivating"_s<<u"Stopping…"_s;
        QTest::newRow("reloading")<<u"reloading"_s<<u"Reloading…"_s;
        QTest::newRow("missing")<<u"missing"_s<<u"Not installed"_s;
        QTest::newRow("masked")<<u"masked"_s<<u"Blocked by the administrator"_s;
        QTest::newRow("unknown")<<u"unknown"_s<<u"Status unavailable"_s;
    }
    void populatedStatusSavedAndStagedData() {
        QFETCH(QString,state);QFETCH(QString,label);QTemporaryDir directory;
        const QStringList protocol{qEnvironmentVariable("FARSIDE_HOST_TEST_FIXTURE",QString::fromUtf8(MAIN_PROTOCOL_FIXTURE)),directory.path()};
        BrokerHostSettings console(Scope::Console,u"/usr/bin/python3"_s,protocol,3000),virtualHost(Scope::Virtual,u"/usr/bin/python3"_s,protocol,3000),
            session(Scope::VirtualSession,u"/usr/bin/python3"_s,protocol,3000);
        BrokerAuthenticationSettings auth(u"/nonexistent/farside-test-helper"_s,{},nullptr);
        BrokerPreferences preferences(directory.path());MainTransport transport;MainNavigation navigation;
        if(state==u"missing")transport.state.loadState=u"not-found"_s;
        else if(state==u"masked")transport.state.unitFileState=u"masked"_s;
        else if(state==u"unknown")transport.state.known=false;
        else transport.state.activeState=state;
        BrokerServices services(&transport);
        QQmlEngine engine;auto *localized=new KLocalizedQmlContext(&engine);localized->setTranslationDomain(u"kcm_farside"_s);engine.rootContext()->setContextObject(localized);
        QStringList warnings;connect(&engine,&QQmlEngine::warnings,this,[&](const auto &errors){for(const auto &error:errors)warnings.append(error.toString());});
        QQmlComponent component(&engine,QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_MAIN_TEST_PAGE",QString::fromUtf8(MAIN_PAGE))));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"navigation"_s,QVariant::fromValue(&navigation)},
            {u"administration"_s,QVariant::fromValue(&services)},{u"consoleHost"_s,QVariant::fromValue(&console)},
            {u"virtualHost"_s,QVariant::fromValue(&virtualHost)},{u"sessionSettings"_s,QVariant::fromValue(&session)},
            {u"authentication"_s,QVariant::fromValue(&auth)},{u"preferences"_s,QVariant::fromValue(&preferences)},{u"hostName"_s,u"test-host"_s}}));
        QVERIFY2(object,qPrintable(component.errorString()));auto *page=qobject_cast<QQuickItem *>(object.data());QVERIFY(page);
        QQuickWindow window;window.resize(640,800);page->setParentItem(window.contentItem());page->setSize(window.size());window.show();
        const auto item=[&](const QString &name){return find(page,name);};
        const auto click=[&](const QString &name){auto *button=item(name);return button&&QMetaObject::invokeMethod(button,"clicked");};
        QVERIFY(!console.loaded());QVERIFY(!virtualHost.loaded());QVERIFY(!auth.loaded());QVERIFY(!preferences.loaded());
        QCOMPARE(item(u"consoleHostStatus"_s)->property("text").toString(),label);QCOMPARE(item(u"virtualHostStatus"_s)->property("text").toString(),label);
        QVERIFY(!item(u"consoleStoredEndpointRow"_s)->property("visible").toBool());
        const auto screenshots=qEnvironmentVariable("FARSIDE_MAIN_SCREENSHOTS");
        if(!screenshots.isEmpty()&&state==u"active") {
            QTest::qWait(150);QVERIFY(window.grabWindow().save(screenshots+u"/main-fresh.png"_s));
        }
        QVERIFY(click(u"consoleHostSettingsLink"_s));QCOMPARE(navigation.page,u"BrokerHostsPage.qml"_s);QCOMPARE(navigation.initial[u"initialScope"_s].toInt(),0);
        QVERIFY(click(u"virtualHostSettingsLink"_s));QCOMPARE(navigation.initial[u"initialScope"_s].toInt(),1);
        for(const auto &host:{&console,&virtualHost}) { QVERIFY(host->reload());QTRY_VERIFY(!host->busy());QVERIFY(host->loaded()); }
        QTRY_VERIFY(item(u"consoleStoredEndpointRow"_s)->property("visible").toBool());
        QVERIFY(item(u"consoleStoredEndpoint"_s)->property("text").toString().contains(u"test-host:3391"));
        QVERIFY(item(u"virtualStoredEndpoint"_s)->property("text").toString().contains(u"test-host:3395"));
        QVERIFY(console.setValue(u"Port"_s,u"3401"_s));QVERIFY(console.setValue(u"Address"_s,u"2001:db8::1"_s));
        // Staged edits never become a connection address or certificate claim.
        QVERIFY(item(u"consoleStoredEndpoint"_s)->property("text").toString().contains(u"test-host:3391"));
        QVERIFY(item(u"consoleHostPending"_s)->property("visible").toBool());QVERIFY(click(u"consoleCopyStoredEndpoint"_s));QCOMPARE(navigation.copied,u"test-host:3391"_s);
        QVERIFY(console.save());QTRY_VERIFY(!console.busy());QVERIFY(console.applicationRequired());
        QTRY_VERIFY(item(u"consoleStoredEndpoint"_s)->property("text").toString().contains(u"[2001:db8::1]:3401"));
        QVERIFY(item(u"consoleHostApply"_s)->property("visible").toBool());QVERIFY(!item(u"consoleHostPending"_s)->property("visible").toBool());
        QVERIFY(!item(u"consoleInspectedEndpoint"_s)->property("visible").toBool());
        QVERIFY(console.inspectRuntime());QTRY_VERIFY(!console.busy());QVERIFY(!console.runtimeStale());
        QTRY_VERIFY(item(u"consoleInspectedEndpoint"_s)->property("visible").toBool());
        QVERIFY(item(u"consoleInspectedEndpoint"_s)->property("text").toString().contains(u"[2001:db8::1]:3401"));
        QVERIFY(console.setValue(u"Port"_s,u"3402"_s));QVERIFY(console.save());QTRY_VERIFY(!console.busy());QVERIFY(console.runtimeStale());
        QTRY_VERIFY(!item(u"consoleInspectedEndpoint"_s)->property("visible").toBool());
        for(const auto &pair:{qMakePair(u"brokerServicesLink"_s,u"BrokerServicesPage.qml"_s),qMakePair(u"brokerSignInButton"_s,u"BrokerSignInPage.qml"_s),
            qMakePair(u"brokerPreferencesLink"_s,u"BrokerPreferencesPage.qml"_s),qMakePair(u"brokerHostsLink"_s,u"BrokerHostsPage.qml"_s)}) {
            QVERIFY(click(pair.first));QCOMPARE(navigation.page,pair.second);
        }
        QCOMPARE(navigation.initial[u"initialScope"_s].toInt(),2);
        QVERIFY(click(u"refreshBrokerStatus"_s));QCOMPARE(transport.mutations,0);
        if(state==u"active") {
            auto *toggle=item(u"consoleHostEnabled"_s);QVERIFY(toggle);
            QVERIFY(toggle->setProperty("checked",false));QVERIFY(QMetaObject::invokeMethod(toggle,"clicked"));
            auto *confirm=item(u"confirmMainServiceStop"_s);QVERIFY(confirm);QTRY_VERIFY(confirm->property("visible").toBool());
            QCOMPARE(transport.mutations,0);QVERIFY(QMetaObject::invokeMethod(confirm,"reject"));
            QCOMPARE(transport.mutations,0);QVERIFY(toggle->property("checked").toBool());
            QVERIFY(toggle->setProperty("checked",false));QVERIFY(QMetaObject::invokeMethod(toggle,"clicked"));
            QVERIFY(QMetaObject::invokeMethod(confirm,"accept"));QTRY_COMPARE(transport.mutations,1);
            QCOMPARE(transport.lastRoute,0);QCOMPARE(transport.lastOperation,BrokerServiceTransport::Stop);
            QTest::qWait(200);
        }
        QTest::qWait(100);auto *flickable=page->property("flickable").value<QQuickItem *>();QVERIFY(flickable);
        auto *content=flickable->property("contentItem").value<QQuickItem *>();QVERIFY(content);
        for(const auto &name:{u"consoleStoredEndpoint"_s,u"consoleStoredFingerprint"_s,u"consoleHostApply"_s,u"pageButtons"_s,u"stockScopeNotice"_s}) {
            auto *control=qobject_cast<QQuickItem *>(item(name));QVERIFY(control);const auto rect=control->mapRectToItem(content,QRectF(0,0,control->width(),control->height()));
            QVERIFY2(rect.left()>=-0.5&&rect.right()<=flickable->width()+0.5,qPrintable(name));
        }
        if(!screenshots.isEmpty())QVERIFY(window.grabWindow().save(screenshots+u"/main-"_s+state+u".png"_s));
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u'\n')));page->setParentItem(nullptr);
    }
};
QTEST_MAIN(BrokerMainPageTest)
#include "BrokerMainPageTest.moc"
