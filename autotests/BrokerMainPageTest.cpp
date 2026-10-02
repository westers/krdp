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
    int mutations=0, lastRoute=-1, reads=0;
    Operation lastOperation=Start;
    void query(int,QueryDone done) override { ++reads; done(state,{}); }
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
    void tabsKeepEditorsFocusAndGeometryAcrossServiceUpdates() {
        QTemporaryDir dir;
        const QStringList protocol{qEnvironmentVariable("FARSIDE_HOST_TEST_FIXTURE",QString::fromUtf8(MAIN_PROTOCOL_FIXTURE)),dir.path()};
        BrokerHostSettings console(Scope::Console,u"/usr/bin/python3"_s,protocol,3000), virtualHost(Scope::Virtual,u"/usr/bin/python3"_s,protocol,3000), session(Scope::VirtualSession,u"/usr/bin/python3"_s,protocol,3000);
        BrokerAuthenticationSettings auth(u"/nonexistent/farside-test-helper"_s,{},nullptr);
        BrokerPreferences preferences(dir.path()); MainTransport transport; MainNavigation navigation; BrokerServices services(&transport);
        QQmlEngine engine; auto *localized=new KLocalizedQmlContext(&engine); localized->setTranslationDomain(u"kcm_farside"_s); engine.rootContext()->setContextObject(localized);
        QStringList warnings; connect(&engine,&QQmlEngine::warnings,this,[&](const auto &errors){for(const auto &e:errors)warnings.append(e.toString());});
        QQmlComponent component(&engine,QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_MAIN_TEST_PAGE",QString::fromUtf8(MAIN_PAGE))));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"navigation"_s,QVariant::fromValue(&navigation)},{u"administration"_s,QVariant::fromValue(&services)},
            {u"consoleHost"_s,QVariant::fromValue(&console)},{u"virtualHost"_s,QVariant::fromValue(&virtualHost)},{u"sessionSettings"_s,QVariant::fromValue(&session)},
            {u"authentication"_s,QVariant::fromValue(&auth)},{u"preferences"_s,QVariant::fromValue(&preferences)},{u"hostName"_s,u"test-host"_s}}));
        QVERIFY2(object,qPrintable(component.errorString())); auto *page=qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
        QQuickWindow window; window.resize(900,850); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        const auto item=[&](const QString &name){return find(page,name);};
        QVERIFY(!console.loaded()); QVERIFY(!virtualHost.loaded()); QVERIFY(!session.loaded()); QVERIFY(!auth.loaded()); QVERIFY(!preferences.loaded());
        auto *consolePage=qobject_cast<QQuickItem *>(item(u"consoleSettingsPage"_s)); auto *virtualPage=item(u"virtualSettingsPage"_s);
        QVERIFY(consolePage); QVERIFY(virtualPage); QVERIFY(console.reload()); QTRY_VERIFY(!console.busy()); QVERIFY(console.loaded());
        QVERIFY(console.setValue(u"Quality"_s,u"80"_s));
        auto *quality=qobject_cast<QQuickItem *>(find(consolePage,u"host_Quality"_s)); QVERIFY(quality); QVERIFY(quality->isVisible());
        QTest::qWait(150); quality->forceActiveFocus(); QTest::keyClick(&window,Qt::Key_A,Qt::ControlModifier); QTest::keyClick(&window,Qt::Key_7);
        auto *focus=window.activeFocusItem(); QVERIFY(focus); const auto pendingText=focus->property("text").toString(); QVERIFY(!pendingText.isEmpty());
        const auto geometry=quality->mapRectToItem(page,QRectF(0,0,quality->width(),quality->height()));
        const auto saved=console.values(); auto *toggle=item(u"consoleHostEnabled"_s); QVERIFY(toggle);
        for(int update=0;update<8;++update) {
            transport.state.mainPid=100+update; transport.state.activeState=update%2?u"inactive"_s:u"active"_s;
            Q_EMIT transport.changed(); QTest::qWait(30);
            QCOMPARE(find(consolePage,u"host_Quality"_s),quality); QCOMPARE(item(u"consoleHostEnabled"_s),toggle);
            QCOMPARE(window.activeFocusItem(),focus); QCOMPARE(focus->property("text").toString(),pendingText); QCOMPARE(console.values(),saved);
            QCOMPARE(quality->mapRectToItem(page,QRectF(0,0,quality->width(),quality->height())),geometry);
        }
        const auto reads=transport.reads; QTest::qWait(5200); QCOMPARE(transport.reads,reads); // former 5s poll must be gone
        QTest::keyClick(&window,Qt::Key_Return); QTRY_COMPARE(console.values()[u"Quality"_s].toString(),u"7"_s);
        const auto screenshots=qEnvironmentVariable("FARSIDE_MAIN_SCREENSHOTS");
        console.discard(); QTRY_COMPARE(quality->property("value").toInt(),80); QVERIFY(virtualHost.reload()); QTRY_VERIFY(!virtualHost.busy()); QVERIFY(session.reload()); QTRY_VERIFY(!session.busy()); QVERIFY(preferences.reload());
        for(int tab=0;tab<4;++tab) {
            QVERIFY(page->setProperty("currentTab",tab)); QTest::qWait(150);
            if(!screenshots.isEmpty())QVERIFY(window.grabWindow().save(screenshots+u"/tabs-"_s+QString::number(tab)+u".png"_s));
        }
        QVERIFY(page->setProperty("currentTab",1));
        auto *virtualTabs=find(qobject_cast<QQuickItem *>(virtualPage),u"virtualSettingsTabs"_s); QVERIFY(virtualTabs);
        // Choosing defaults retains both host drafts and the same page objects.
        auto *scope=find(qobject_cast<QQuickItem *>(virtualPage),u"hostScope"_s); QVERIFY(scope); QVERIFY(scope->setProperty("currentIndex",2));
        QCOMPARE(virtualPage->property("host").value<QObject *>(),&session); QVERIFY(session.setValue(u"RenderPci"_s,u"0000:01:00.0"_s));
        QVERIFY(scope->setProperty("currentIndex",1)); QCOMPARE(virtualPage->property("host").value<QObject *>(),&virtualHost);
        QCOMPARE(session.values()[u"RenderPci"_s].toString(),u"0000:01:00.0"_s);
        QVERIFY(page->setProperty("currentTab",0)); QCOMPARE(find(consolePage,u"host_Quality"_s),quality);
        // Saved address is independent of the current draft, and copy uses that saved address.
        QVERIFY(console.setValue(u"Port"_s,u"3401"_s));
        QVERIFY(page->setProperty("currentTab",2));
        QVERIFY(QMetaObject::invokeMethod(item(u"consoleCertificateDetails"_s),"clicked"));
        auto *certificateDialog=item(u"certificateDetailsDialog"_s); QVERIFY(certificateDialog);
        QTRY_VERIFY(certificateDialog->property("visible").toBool());
        auto *loader=certificateDialog->property("contentItem").value<QQuickItem *>(); QVERIFY(loader);
        auto *certificatePage=loader->property("item").value<QQuickItem *>(); QVERIFY(certificatePage);
        QVERIFY(certificatePage->property("certificateOnly").toBool());
        QCOMPARE(certificatePage->property("host").value<QObject *>(),&console);
        QVERIFY(QMetaObject::invokeMethod(find(certificatePage,u"defaultHostSettings"_s),"clicked"));
        QCOMPARE(console.tlsMode(),u"standard"_s); QCOMPARE(console.values()[u"Port"_s].toString(),u"3401"_s);
        QVERIFY(QMetaObject::invokeMethod(find(certificatePage,u"discardHostSettings"_s),"clicked"));
        QCOMPARE(console.tlsMode(),u"keep"_s); QCOMPARE(console.values()[u"Port"_s].toString(),u"3401"_s);
        QVERIFY(QMetaObject::invokeMethod(certificateDialog,"reject"));
        QVERIFY(page->setProperty("currentTab",0));
        QVERIFY(QMetaObject::invokeMethod(item(u"consoleDisplayPreferences"_s),"clicked"));
        QCOMPARE(page->property("currentTab").toInt(),3); QVERIFY(page->setProperty("currentTab",0));
        QVERIFY(item(u"consoleStoredEndpoint"_s)->property("text").toString().contains(u"test-host:3391"));
        QVERIFY(QMetaObject::invokeMethod(item(u"consoleCopyStoredEndpoint"_s),"clicked")); QCOMPARE(navigation.copied,u"test-host:3391"_s);
        QVERIFY(console.save()); QTRY_VERIFY(!console.busy()); QVERIFY(console.applicationRequired());
        QTRY_VERIFY(item(u"consoleStoredEndpoint"_s)->property("text").toString().contains(u"test-host:3401"));
        QVERIFY(console.inspectRuntime()); QTRY_VERIFY(!console.busy()); QTRY_VERIFY(item(u"consoleInspectedEndpoint"_s)->property("visible").toBool());
        QVERIFY(console.setValue(u"Port"_s,u"3402"_s)); QVERIFY(console.save()); QTRY_VERIFY(!console.busy());
        QTRY_VERIFY(!item(u"consoleInspectedEndpoint"_s)->property("visible").toBool());
        transport.state.activeState=u"active"_s; Q_EMIT transport.changed();
        QVERIFY(toggle->setProperty("checked",false)); QVERIFY(QMetaObject::invokeMethod(toggle,"clicked"));
        auto *confirm=item(u"consoleConfirmServiceOperation"_s); QVERIFY(confirm); QTRY_VERIFY(confirm->property("visible").toBool());
        QCOMPARE(transport.mutations,0); QVERIFY(QMetaObject::invokeMethod(confirm,"reject")); QCOMPARE(transport.mutations,0); QVERIFY(toggle->property("checked").toBool());
        QVERIFY(toggle->setProperty("checked",false)); QVERIFY(QMetaObject::invokeMethod(toggle,"clicked")); QVERIFY(QMetaObject::invokeMethod(confirm,"accept"));
        QCOMPARE(transport.mutations,1); QCOMPARE(transport.lastRoute,0); QCOMPARE(transport.lastOperation,BrokerServiceTransport::Stop);
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u'\n'))); page->setParentItem(nullptr);
    }
};
QTEST_MAIN(BrokerMainPageTest)
#include "BrokerMainPageTest.moc"
