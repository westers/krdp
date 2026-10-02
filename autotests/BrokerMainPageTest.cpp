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
    void navigationDraftsAndNativePresentation() {
        QTemporaryDir dir;
        const QStringList protocol{qEnvironmentVariable("FARSIDE_HOST_TEST_FIXTURE",QString::fromUtf8(MAIN_PROTOCOL_FIXTURE)),dir.path()};
        BrokerHostSettings console(Scope::Console,u"/usr/bin/python3"_s,protocol,3000), virtualHost(Scope::Virtual,u"/usr/bin/python3"_s,protocol,3000), session(Scope::VirtualSession,u"/usr/bin/python3"_s,protocol,3000);
        BrokerAuthenticationSettings auth(u"/usr/bin/python3"_s,{qEnvironmentVariable("FARSIDE_AUTH_TEST_FIXTURE",QString::fromUtf8(MAIN_AUTH_PROTOCOL_FIXTURE)),dir.path()},nullptr);
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
        QCOMPARE(page->property("currentPage").toInt(),0);
        QVERIFY(QMetaObject::invokeMethod(item(u"configureConsole"_s),"clicked")); QCOMPARE(page->property("currentPage").toInt(),1);
        QVERIFY(console.reload()); QTRY_VERIFY(!console.busy()); QVERIFY(console.loaded());
        auto *consolePage=qobject_cast<QQuickItem *>(item(u"consoleSettingsPage"_s)); QVERIFY(consolePage);
        QVERIFY(console.setValue(u"Quality"_s,u"80"_s));
        auto *quality=qobject_cast<QQuickItem *>(find(consolePage,u"host_Quality"_s)); QVERIFY(quality && quality->isVisible());
        QTest::qWait(150); quality->forceActiveFocus(); QTest::keyClick(&window,Qt::Key_A,Qt::ControlModifier); QTest::keyClick(&window,Qt::Key_7);
        auto *focus=window.activeFocusItem(); QVERIFY(focus); const auto pendingText=focus->property("text").toString(); QVERIFY(!pendingText.isEmpty());
        const auto geometry=quality->mapRectToItem(page,QRectF(0,0,quality->width(),quality->height())); const auto saved=console.values();
        for(int update=0;update<6;++update) {
            transport.state.mainPid=100+update; transport.state.activeState=update%2?u"inactive"_s:u"active"_s;
            Q_EMIT transport.changed(); QTest::qWait(30);
            QCOMPARE(find(consolePage,u"host_Quality"_s),quality); QCOMPARE(window.activeFocusItem(),focus);
            QCOMPARE(focus->property("text").toString(),pendingText); QCOMPARE(console.values(),saved);
            QCOMPARE(quality->mapRectToItem(page,QRectF(0,0,quality->width(),quality->height())),geometry);
        }
        const auto reads=transport.reads; QTest::qWait(5200); QCOMPARE(transport.reads,reads);
        QTest::keyClick(&window,Qt::Key_Return); QTRY_COMPARE(console.values()[u"Quality"_s].toString(),u"7"_s);
        QVERIFY(console.setValue(u"Port"_s,u"3401"_s)); const auto before=console.values();
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"editHostCertificate"_s),"clicked")); QCOMPARE(page->property("currentPage").toInt(),5);
        auto *draft=qobject_cast<BrokerHostSettings *>(console.certificateDraft()); QVERIFY(draft && draft->loaded());
        QVERIFY(QMetaObject::invokeMethod(item(u"certificateStandard"_s),"clicked")); QCOMPARE(draft->tlsMode(),u"standard"_s); QCOMPARE(console.tlsMode(),u"keep"_s);
        QVERIFY(QMetaObject::invokeMethod(item(u"cancelCertificateEdit"_s),"clicked")); QCOMPARE(page->property("currentPage").toInt(),1); QCOMPARE(console.values(),before); QCOMPARE(console.tlsMode(),u"keep"_s);
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"editHostCertificate"_s),"clicked"));
        QVERIFY(QMetaObject::invokeMethod(item(u"certificateStandard"_s),"clicked")); QVERIFY(QMetaObject::invokeMethod(item(u"stageCertificateEdit"_s),"clicked"));
        QCOMPARE(page->property("currentPage").toInt(),1); QCOMPARE(console.tlsMode(),u"standard"_s); QCOMPARE(console.values()[u"Port"_s].toString(),u"3401"_s);
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"defaultHostSettings"_s),"clicked")); QCOMPARE(console.tlsMode(),u"standard"_s); console.discard();
        QVERIFY(virtualHost.reload()); QTRY_VERIFY(!virtualHost.busy()); QVERIFY(session.reload()); QTRY_VERIFY(!session.busy()); QVERIFY(auth.reload()); QTRY_VERIFY(!auth.busy()); QVERIFY(preferences.reload());
        QVERIFY(page->setProperty("currentPage",4));
        auto *prefsPage=qobject_cast<QQuickItem *>(item(u"brokerPreferencesPage"_s)); QVERIFY(prefsPage);
        auto *mode=find(prefsPage,u"inherit_Quality"_s); QVERIFY(mode);
        QVERIFY(mode->setProperty("currentIndex",1)); QVERIFY(QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,1)));
        QVERIFY(preferences.values().contains(u"Quality"_s)); QCOMPARE(preferences.values()[u"Quality"_s].toString(),QString()); QVERIFY(!preferences.canSave());
        auto *prefQuality=find(prefsPage,u"preference_Quality"_s); QVERIFY(prefQuality); QVERIFY(prefQuality->setProperty("value",91)); QVERIFY(QMetaObject::invokeMethod(prefQuality,"valueModified")); QCOMPARE(preferences.values()[u"Quality"_s].toString(),u"91"_s);
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"specific"_s)); QVERIFY(preferences.setValue(u"MonitorIndex"_s,u"2"_s));
        auto *monitorField=qobject_cast<QQuickItem *>(find(prefsPage,u"preference_field_MonitorIndex"_s)); QVERIFY(monitorField); QTRY_VERIFY(monitorField->isVisible());
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"virtual"_s)); QTRY_VERIFY(!monitorField->isVisible()); QCOMPARE(preferences.values()[u"MonitorIndex"_s].toString(),u"2"_s);
        auto *policyField=qobject_cast<QQuickItem *>(find(prefsPage,u"preference_field_VirtualMonitorPolicy"_s)); QVERIFY(policyField); QTRY_VERIFY(policyField->isVisible());
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"workspace"_s)); QTRY_VERIFY(!policyField->isVisible()); preferences.discard(); QVERIFY(!preferences.modified());
        QVERIFY(page->setProperty("currentPage",3)); QVERIFY(QMetaObject::invokeMethod(item(u"consoleAddAlias"_s),"clicked"));
        auto *aliasDialog=item(u"brokerAliasDialog"_s); QVERIFY(aliasDialog); QTRY_VERIFY(aliasDialog->property("visible").toBool());
        auto *stageAlias=item(u"stageBrokerAlias"_s); QVERIFY(stageAlias);
        QVERIFY(item(u"brokerAliasName"_s)->setProperty("text",u"fixture-remote"_s)); QVERIFY(item(u"brokerAliasOwner"_s)->setProperty("text",u"westers"_s)); QVERIFY(!stageAlias->property("enabled").toBool());
        QVERIFY(item(u"brokerAliasPassword"_s)->setProperty("text",u"fixture-only-password"_s)); QTRY_VERIFY(stageAlias->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(stageAlias,"triggered")); QTRY_VERIFY(!aliasDialog->property("visible").toBool()); QCOMPARE(item(u"brokerAliasPassword"_s)->property("text").toString(),QString());
        QVERIFY(auth.modified()); QVERIFY(auth.removeAlias(u"console"_s,u"fixture-remote"_s)); QVERIFY(auth.undoRemoveAlias()); QCOMPARE(auth.policy()[u"console"_s].toMap()[u"credentials"_s].toList().size(),1); auth.discard();
        transport.state.activeState = u"active"_s; transport.state.mainPid = 42; Q_EMIT transport.changed();
        const auto screenshots=qEnvironmentVariable("FARSIDE_MAIN_SCREENSHOTS");
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"virtual"_s));
        const auto screenshot=[&](const QString &name){QTest::qWait(180); if(!screenshots.isEmpty()) QVERIFY(window.grabWindow().save(screenshots+u"/"_s+name+u".png"_s));};
        for(int index=0;index<10;++index) {
            if(index==5) QVERIFY(console.beginCertificateEdit());
            if(index==6) QVERIFY(virtualHost.beginCertificateEdit());
            QVERIFY(page->setProperty("currentPage",index)); screenshot(u"page-"_s+QString::number(index));
        }
        QVERIFY(page->setProperty("currentPage",3)); QVERIFY(QMetaObject::invokeMethod(item(u"consoleAddAlias"_s),"clicked")); screenshot(u"alias-dialog"_s); QVERIFY(QMetaObject::invokeMethod(aliasDialog,"close"));
        QVERIFY(page->setProperty("currentPage",0));
        auto *details=qobject_cast<QQuickItem *>(item(u"settingsOverview"_s)); QVERIFY(details);
        auto *enabled=find(details,u"consoleHostEnabled"_s); QVERIFY(enabled); QVERIFY(enabled->setProperty("checked",false)); QVERIFY(QMetaObject::invokeMethod(enabled,"clicked"));
        auto *confirm=find(details,u"consoleConfirmServiceOperation"_s); QVERIFY(confirm); QTRY_VERIFY(confirm->property("visible").toBool()); QCOMPARE(transport.mutations,0); screenshot(u"stop-dialog"_s); QVERIFY(QMetaObject::invokeMethod(confirm,"close")); QCOMPARE(transport.mutations,0);
        window.resize(640,360); page->setSize(window.size());
        for(int index=1;index<8;++index) {
            QVERIFY(page->setProperty("currentPage",index)); QTest::qWait(60);
            const auto visibleSave=[&](auto &&self,QQuickItem *parent)->QQuickItem * {
                if(parent->isVisible() && parent->objectName().startsWith(u"save")) return parent;
                for (auto *child : parent->childItems()) { if (auto *result = self(self, child)) return result; }
                return nullptr;
            };
            if(auto *save=visibleSave(visibleSave,page)) {
                const auto bounds=save->mapRectToItem(page,QRectF(0,0,save->width(),save->height()));
                QVERIFY2(bounds.left()>=0 && bounds.right()<=page->width()+1 && bounds.bottom()<=page->height()+1,qPrintable(QStringLiteral("Footer %1 exceeds %2x%3: %4,%5").arg(save->objectName()).arg(page->width()).arg(page->height()).arg(bounds.right()).arg(bounds.bottom())));
            }
        }
        screenshot(u"narrow-hardware"_s);
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u'\n')));
    }

};
QTEST_MAIN(BrokerMainPageTest)
#include "BrokerMainPageTest.moc"
