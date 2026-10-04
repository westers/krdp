// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerhostsettings.h"
#include "HostSnapshotFixture.h"
#include "brokerauthenticationsettings.h"
#include "brokerpreferences.h"
#include "brokerservices.h"
#include "ServerCertificate.h"
#include <KLocalizedQmlContext>
#include <QFile>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPluginLoader>
#include <QScopeGuard>
#include <QJSValue>
#include <QTemporaryDir>
#include <QTest>
using namespace Qt::StringLiterals;
// Stands in for the KCM page stack: push() creates the named page with the given
// initial properties above the overview, pop() destroys the top page. The real
// module is exercised by KcmUiTest; this keeps the fake models of this test.
class MainNavigation : public QObject {
    Q_OBJECT
public:
    QString copied; QQmlEngine *engine=nullptr; QQuickItem *container=nullptr; QString base; QQuickItem *root=nullptr;
    QList<QPointer<QQuickItem>> stack; QStringList pushed;
    Q_INVOKABLE void push(const QString &name,const QVariantMap &properties={}) {
        QQmlComponent component(engine,QUrl(base+name));
        if(!component.isReady()) { qWarning() << component.errorString(); return; }
        auto *item=qobject_cast<QQuickItem *>(component.createWithInitialProperties(properties));
        if(!item) { qWarning() << component.errorString(); return; }
        top()->setVisible(false);
        item->setParentItem(container); item->setSize(root->size()); stack.append(item); pushed.append(name);
        // Let the page's delayed layout work (Kirigami FormLayout twins) run before it can be popped.
        QTest::qWait(30);
    }
    Q_INVOKABLE void pop() {   // Back: the page goes away; the one below is shown again
        if(stack.isEmpty()) return;
        auto *item=stack.takeLast().data(); item->setParentItem(nullptr); delete item;
        top()->setVisible(true);
    }
    Q_INVOKABLE void copyAddressToClipboard(const QString &text) { copied=text; }
    int depth() const { return int(stack.size()); }
    QQuickItem *top() const { return stack.isEmpty() ? root : stack.last().data(); }
    void goHome() { while(!stack.isEmpty()) pop(); }
    void resizeAll(const QSizeF &size) { root->setSize(size); for(const auto &item:stack) if(item) item->setSize(size); }
    void clear() { goHome(); }
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
    static QObject *findVisible(QQuickItem *parent,const QString &name) {
        if(parent->objectName()==name && parent->isVisible())return parent;
        for(auto *child:parent->childItems())if(auto *value=findVisible(child,name))return value;
        return nullptr;
    }
    struct Fixture {
        QTemporaryDir dir; QStringList protocol;
        BrokerHostSettings console, virtualHost, session; BrokerAuthenticationSettings auth; BrokerPreferences preferences;
        MainTransport transport; MainNavigation navigation; BrokerServices services;
        QQmlEngine engine; QScopedPointer<QObject> object; QQuickItem *page=nullptr; QQuickWindow window;
        Fixture() : protocol{qEnvironmentVariable("FARSIDE_HOST_TEST_FIXTURE",QString::fromUtf8(MAIN_PROTOCOL_FIXTURE)),dir.path()},
            console(Scope::Console,u"/usr/bin/python3"_s,protocol,3000), virtualHost(Scope::Virtual,u"/usr/bin/python3"_s,protocol,3000), session(Scope::VirtualSession,u"/usr/bin/python3"_s,protocol,3000),
            auth(u"/usr/bin/python3"_s,{qEnvironmentVariable("FARSIDE_AUTH_TEST_FIXTURE",QString::fromUtf8(MAIN_AUTH_PROTOCOL_FIXTURE)),dir.path()},nullptr),
            preferences(dir.path()), services(&transport) {}
        ~Fixture() { navigation.clear(); }
        // Items of the visible page first, then the overview underneath.
        QObject *item(const QString &name) {
            for(int i=navigation.depth()-1;i>=0;--i) if(auto *value=find(navigation.stack[i],name)) return value;
            return find(page,name);
        }
        QQuickItem *goTo(int index) {   // 0 overview, 1 Console, 2 Virtual, 3 Who Can Connect, 4 My Preferences
            navigation.goHome();
            static const char *const buttons[]={"","configureConsole","configureVirtual","configureAccess","configurePreferences"};
            if(index>0 && !QMetaObject::invokeMethod(find(page,QString::fromLatin1(buttons[index])),"clicked")) return nullptr;
            return navigation.top();
        }
        bool init() {
            auto *localized=new KLocalizedQmlContext(&engine); localized->setTranslationDomain(u"kcm_farside"_s); engine.rootContext()->setContextObject(localized);
            QQmlComponent component(&engine,QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_MAIN_TEST_PAGE",QString::fromUtf8(MAIN_PAGE))));
            if(!component.isReady()) { qWarning() << component.errorString(); return false; }
            object.reset(component.createWithInitialProperties({{u"navigation"_s,QVariant::fromValue(&navigation)},{u"administration"_s,QVariant::fromValue(&services)},
                {u"consoleHost"_s,QVariant::fromValue(&console)},{u"virtualHost"_s,QVariant::fromValue(&virtualHost)},{u"sessionSettings"_s,QVariant::fromValue(&session)},
                {u"authentication"_s,QVariant::fromValue(&auth)},{u"preferences"_s,QVariant::fromValue(&preferences)},{u"hostName"_s,u"test-host"_s}}));
            page=qobject_cast<QQuickItem *>(object.data()); if(!page) return false;
            window.resize(900,850); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
            navigation.engine=&engine; navigation.container=window.contentItem(); navigation.root=page; navigation.base=QUrl::fromLocalFile(QFileInfo(qEnvironmentVariable("FARSIDE_MAIN_TEST_PAGE",QString::fromUtf8(MAIN_PAGE))).absolutePath()+u"/"_s).toString();
            return true;
        }
    };
private Q_SLOTS:
    void refreshStatusReachableInDetails() {
        Fixture f; QVERIFY(f.init());
        QObject *toggle=find(f.page,u"consoleDetailsToggle"_s); QVERIFY(toggle);
        QVERIFY(!findVisible(f.page,u"consoleRefreshStatus"_s));          // not in the always-visible row
        QVERIFY(QMetaObject::invokeMethod(toggle,"clicked")); QTest::qWait(60);
        QObject *refresh=findVisible(f.page,u"consoleRefreshStatus"_s); QVERIFY(refresh);
        QTRY_VERIFY(refresh->property("enabled").toBool());
        const int reads=f.transport.reads;
        QVERIFY(QMetaObject::invokeMethod(refresh,"clicked")); QTRY_VERIFY(f.transport.reads>reads);
    }
    void leavingHostPageAnyWayCancelsCertificateDraft() {
        Fixture f; QVERIFY(f.init());
        QVERIFY(f.console.reload()); QTRY_VERIFY(!f.console.busy()); QVERIFY(f.console.loaded());
        QQuickItem *consolePage=f.goTo(1); QVERIFY(consolePage); QCOMPARE(f.navigation.depth(),1); QCOMPARE(consolePage->objectName(),u"consoleSettingsPage"_s);
        QVERIFY(f.console.setValue(u"Port"_s,u"3401"_s));
        QPointer<QObject> section=find(consolePage,u"certificateSection"_s); QVERIFY(section);
        QTRY_VERIFY(find(consolePage,u"editHostCertificate"_s)->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"editHostCertificate"_s),"clicked")); QVERIFY(section->property("open").toBool());
        QTRY_VERIFY(find(consolePage,u"certificateImport"_s)->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateImport"_s),"clicked"));
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"consoleDisplayPreferences"_s),"clicked"));
        QCOMPARE(f.navigation.depth(),2); QCOMPARE(f.navigation.top()->objectName(),u"brokerPreferencesPage"_s);   // pushed on top of the host page
        QVERIFY(!section->property("open").toBool());
        QCOMPARE(f.console.tlsMode(),u"keep"_s); QCOMPARE(f.console.values()[u"Port"_s].toString(),u"3401"_s);
        f.navigation.pop(); QCOMPARE(f.navigation.depth(),1); QCOMPARE(f.navigation.top(),consolePage); QVERIFY(consolePage->isVisible());   // Back returns to the host page
        f.navigation.pop(); QCOMPARE(f.navigation.depth(),0);
        QVERIFY(!section || !section->property("open").toBool()); QCOMPARE(f.console.tlsMode(),u"keep"_s); QCOMPARE(f.console.values()[u"Port"_s].toString(),u"3401"_s);
    }
    void backFromHostPageCancelsCertificateDraft() {
        Fixture f; QVERIFY(f.init());
        QVERIFY(f.console.reload()); QTRY_VERIFY(!f.console.busy()); QVERIFY(f.console.loaded());
        QQuickItem *consolePage=f.goTo(1); QVERIFY(consolePage);
        QTRY_VERIFY(find(consolePage,u"editHostCertificate"_s)->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"editHostCertificate"_s),"clicked"));
        QTRY_VERIFY(find(consolePage,u"certificateStandard"_s)->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateStandard"_s),"clicked"));
        auto *draft=qobject_cast<BrokerHostSettings *>(f.console.certificateDraft()); QVERIFY(draft && draft->loaded()); QCOMPARE(draft->tlsMode(),u"standard"_s);
        f.navigation.pop();                                                  // the native Back (kcm.pop())
        QCOMPARE(f.navigation.depth(),0);
        QVERIFY(f.page->isVisible());
        draft=qobject_cast<BrokerHostSettings *>(f.console.certificateDraft());
        QVERIFY(!draft || !draft->loaded() || draft->tlsMode()==u"keep"_s);  // the unstaged draft is gone
        QCOMPARE(f.console.tlsMode(),u"keep"_s);
    }
    void rootPushesAllFourDestinations() {
        Fixture f; QVERIFY(f.init());
        QCOMPARE(f.navigation.depth(),0);
        const struct { int index; const char *objectName; } destinations[]={{1,"consoleSettingsPage"},{2,"virtualSettingsPage"},{3,"brokerSignInPage"},{4,"brokerPreferencesPage"}};
        for(const auto &d:destinations) {
            QQuickItem *page=f.goTo(d.index); QVERIFY(page); QCOMPARE(f.navigation.depth(),1); QCOMPARE(page->objectName(),QString::fromLatin1(d.objectName));
            QVERIFY(!f.page->isVisible()); QVERIFY(page->isVisible());
            f.navigation.pop(); QCOMPARE(f.navigation.depth(),0); QVERIFY(f.page->isVisible());   // Back reaches the root
        }
        // The custom header, Back button and stacked layout are gone with the native navigation.
        QVERIFY(!find(f.page,u"settingsBack"_s)); QVERIFY(!f.page->property("currentPage").isValid());
    }
    void virtualFooterNamesPendingScope() {
        Fixture f; QVERIFY(f.init());
        QVERIFY(f.virtualHost.reload()); QTRY_VERIFY(!f.virtualHost.busy()); QVERIFY(f.session.reload()); QTRY_VERIFY(!f.session.busy());
        QQuickItem *virtualPage=f.goTo(2); QVERIFY(virtualPage); QTest::qWait(60);
        auto *summary=qobject_cast<QQuickItem *>(find(virtualPage,u"hostPendingSummary"_s)); QVERIFY(summary);
        QObject *saveHost=find(virtualPage,u"saveHostSettings"_s); QVERIFY(saveHost);
        QVERIFY(!summary->isVisible());
        f.session.setValue(u"VaapiDriver"_s,u"off"_s);                       // hardware (desktop defaults) edit only
        QTRY_VERIFY(summary->isVisible()); QVERIFY(!saveHost->property("enabled").toBool());
        QVERIFY(summary->property("text").toString().contains(u"New desktop defaults"_s));
        QVERIFY(summary->property("text").toString().contains(u"Save Desktop Defaults"_s));
        QVERIFY(f.virtualHost.setValue(u"Port"_s,u"3402"_s));                // both dirty
        const auto both=summary->property("text").toString();
        QVERIFY(both.contains(u"Virtual only"_s)); QVERIFY(both.contains(u"New desktop defaults"_s));
        f.session.discard(); QTRY_VERIFY(summary->isVisible());              // host only
        const auto hostOnly=summary->property("text").toString();
        QVERIFY(hostOnly.contains(u"Virtual only"_s)); QVERIFY(!hostOnly.contains(u"desktop defaults"_s,Qt::CaseInsensitive));
    }
    void hostDiscardReloadDefaultsCloseCertificateSection() {
        QTemporaryDir snapshots; QVERIFY(HostSnapshotFixture::publishAll(snapshots.path()));
        qputenv("FARSIDE_PUBLIC_SETTINGS_DIR", snapshots.path().toUtf8()); // Reload reads the public snapshot
        const auto restoreEnvironment=qScopeGuard([]{qunsetenv("FARSIDE_PUBLIC_SETTINGS_DIR");});
        Fixture f; QVERIFY(f.init());
        QVERIFY(f.console.reload()); QTRY_VERIFY(!f.console.busy()); QVERIFY(f.console.loaded());
        QQuickItem *consolePage=f.goTo(1); QVERIFY(consolePage);
        QObject *section=find(consolePage,u"certificateSection"_s); QVERIFY(section);
        const auto openStandard=[&]{
            QTRY_VERIFY(find(consolePage,u"editHostCertificate"_s)->property("enabled").toBool());
            QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"editHostCertificate"_s),"clicked")); QVERIFY(section->property("open").toBool());
            QTRY_VERIFY(find(consolePage,u"certificateStandard"_s)->property("visible").toBool());
            QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateStandard"_s),"clicked"));
            QObject *stage=find(consolePage,u"stageCertificateEdit"_s); QVERIFY(stage); QVERIFY(stage->property("enabled").toBool());
        };
        const auto closedAndCancelled=[&]{
            QVERIFY(!section->property("open").toBool());
            auto *draft=qobject_cast<BrokerHostSettings *>(f.console.certificateDraft());
            QVERIFY(!draft || !draft->loaded() || draft->tlsMode()==u"keep"_s);
        };
        // Revert Changes: needs a pending edit to be enabled.
        QVERIFY(f.console.setValue(u"Port"_s,u"3401"_s)); openStandard();
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"discardHostSettings"_s),"clicked")); closedAndCancelled();
        QCOMPARE(f.console.tlsMode(),u"keep"_s); QVERIFY(!f.console.modified());
        // Restore Defaults.
        openStandard();
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"defaultHostSettings"_s),"clicked")); closedAndCancelled();
        QCOMPARE(f.console.tlsMode(),u"keep"_s); f.console.discard();
        // Reload Saved Settings: confirm only when modified, so make an edit and accept the dialog.
        QVERIFY(f.console.setValue(u"Port"_s,u"3401"_s)); openStandard();
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"loadHostSettings"_s),"clicked"));
        QObject *accept=find(consolePage,u"reloadHostAccept"_s); QVERIFY(accept);
        QVERIFY(QMetaObject::invokeMethod(accept,"triggered")); QTRY_VERIFY(!f.console.busy()); closedAndCancelled();
        QCOMPARE(f.console.tlsMode(),u"keep"_s);
        // Without a pending edit, Reload applies directly and must also close an open section.
        openStandard();
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"loadHostSettings"_s),"clicked")); QTRY_VERIFY(!f.console.busy()); closedAndCancelled();
    }
    void navigationDraftsAndNativePresentation() {
        QTemporaryDir dir;
        const QStringList protocol{qEnvironmentVariable("FARSIDE_HOST_TEST_FIXTURE",QString::fromUtf8(MAIN_PROTOCOL_FIXTURE)),dir.path()};
        BrokerHostSettings console(Scope::Console,u"/usr/bin/python3"_s,protocol,3000), virtualHost(Scope::Virtual,u"/usr/bin/python3"_s,protocol,3000), session(Scope::VirtualSession,u"/usr/bin/python3"_s,protocol,3000);
        BrokerAuthenticationSettings auth(u"/usr/bin/python3"_s,{qEnvironmentVariable("FARSIDE_AUTH_TEST_FIXTURE",QString::fromUtf8(MAIN_AUTH_PROTOCOL_FIXTURE)),dir.path()},nullptr);
        BrokerPreferences preferences(dir.path()); MainTransport transport; MainNavigation navigation; BrokerServices services(&transport);
        QPluginLoader packagedPlugin(qEnvironmentVariable("FARSIDE_MAIN_PACKAGED_PLUGIN"));
        const bool packaged = !packagedPlugin.fileName().isEmpty();
        if (packaged) QVERIFY2(packagedPlugin.load(), qPrintable(packagedPlugin.errorString()));
        QQmlEngine engine; auto *localized=new KLocalizedQmlContext(&engine); localized->setTranslationDomain(u"kcm_farside"_s); engine.rootContext()->setContextObject(localized);
        QStringList warnings; connect(&engine,&QQmlEngine::warnings,this,[&](const auto &errors){for(const auto &e:errors)warnings.append(e.toString());});
        QQmlComponent component(&engine,packaged ? QUrl(u"qrc:/kcm/kcm_farside/BrokerMainPage.qml"_s) : QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_MAIN_TEST_PAGE",QString::fromUtf8(MAIN_PAGE))));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"navigation"_s,QVariant::fromValue(&navigation)},{u"administration"_s,QVariant::fromValue(&services)},
            {u"consoleHost"_s,QVariant::fromValue(&console)},{u"virtualHost"_s,QVariant::fromValue(&virtualHost)},{u"sessionSettings"_s,QVariant::fromValue(&session)},
            {u"authentication"_s,QVariant::fromValue(&auth)},{u"preferences"_s,QVariant::fromValue(&preferences)},{u"hostName"_s,u"test-host"_s}}));
        QVERIFY2(object,qPrintable(component.errorString())); auto *page=qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
        QQuickWindow window; window.resize(900,850); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        navigation.engine=&engine; navigation.container=window.contentItem(); navigation.root=page;
        navigation.base=packaged ? u"qrc:/kcm/kcm_farside/"_s : QUrl::fromLocalFile(QFileInfo(qEnvironmentVariable("FARSIDE_MAIN_TEST_PAGE",QString::fromUtf8(MAIN_PAGE))).absolutePath()+u"/"_s).toString();
        const auto cleanup=qScopeGuard([&]{navigation.clear();});
        const auto item=[&](const QString &name)->QObject *{
            for(int i=navigation.depth()-1;i>=0;--i) if(auto *value=find(navigation.stack[i],name)) return value;
            return find(page,name);
        };
        const auto goTo=[&](int index)->QQuickItem *{
            navigation.goHome(); static const char *const buttons[]={"","configureConsole","configureVirtual","configureAccess","configurePreferences"};
            if(index>0 && !QMetaObject::invokeMethod(find(page,QString::fromLatin1(buttons[index])),"clicked")) return nullptr;
            return navigation.top();
        };
        QVERIFY(!console.loaded()); QVERIFY(!virtualHost.loaded()); QVERIFY(!session.loaded()); QVERIFY(!auth.loaded()); QVERIFY(!preferences.loaded());
        QCOMPARE(navigation.depth(),0);
        // Details live in the overview card and are hidden until expanded.
        QObject *detailsButton = item(u"consoleDetailsToggle"_s);
        QVERIFY(detailsButton);
        QObject *inspect = item(u"inspectHostRuntime"_s);   // first match = console card
        QVERIFY(inspect);
        QVERIFY(!inspect->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(detailsButton, "clicked"));
        QVERIFY(inspect->property("visible").toBool());
        QCOMPARE(navigation.depth(),0);   // no page change
        QVERIFY(QMetaObject::invokeMethod(detailsButton, "clicked"));
        QVERIFY(!inspect->property("visible").toBool());
        QQuickItem *consolePage=goTo(1); QVERIFY(consolePage); QCOMPARE(navigation.depth(),1);
        QVERIFY(console.reload()); QTRY_VERIFY(!console.busy()); QVERIFY(console.loaded());
        QVERIFY(console.setValue(u"Quality"_s,u"80"_s));
        auto *quality=qobject_cast<QQuickItem *>(find(consolePage,u"host_Quality"_s)); QVERIFY(quality && quality->isVisible());
        QTest::qWait(150); quality->forceActiveFocus(); QTest::keyClick(&window,Qt::Key_A,Qt::ControlModifier); QTest::keyClick(&window,Qt::Key_7);
        auto *focus=window.activeFocusItem(); QVERIFY(focus); const auto pendingText=focus->property("text").toString(); QVERIFY(!pendingText.isEmpty());
        // A freshly created page lets its form layout settle (sub-pixel animation) before geometry is compared.
        const auto quality_geometry=[&]{return quality->mapRectToItem(page,QRectF(0,0,quality->width(),quality->height()));};
        for(auto previous=quality_geometry(),current=previous;;previous=current) { QTest::qWait(100); current=quality_geometry(); if(current==previous) break; }
        const auto geometry=quality_geometry(); const auto saved=console.values();
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
        {   // Review focus 2/4: Details expanders keep drafts, and service events keep their editors and geometry.
            auto *portEditor=find(consolePage,u"host_Port"_s); QVERIFY(portEditor);
            const auto portValue=portEditor->property("value");
            navigation.pop(); QCOMPARE(navigation.depth(),0); QTest::qWait(60);   // Back destroys the page; the draft lives in the model
            QVERIFY(QMetaObject::invokeMethod(detailsButton,"clicked")); QVERIFY(inspect->property("visible").toBool()); QTest::qWait(150);
            auto *overviewPage=page; QVERIFY(overviewPage);
            auto *overviewFlick=overviewPage->property("flickable").value<QQuickItem *>(); QVERIFY(overviewFlick);
            const qreal scrollY=overviewFlick->property("contentY").toReal();
            auto *inspectItem=qobject_cast<QQuickItem *>(inspect); QVERIFY(inspectItem);
            const auto inspectGeometry=inspectItem->mapRectToItem(page,QRectF(0,0,inspectItem->width(),inspectItem->height()));
            for(int update=0;update<4;++update) {
                transport.state.activeState=update%2?u"active"_s:u"inactive"_s; Q_EMIT transport.changed(); QTest::qWait(30);
                QCOMPARE(item(u"inspectHostRuntime"_s),inspect); QVERIFY(inspect->property("visible").toBool());
                QCOMPARE(overviewFlick->property("contentY").toReal(),scrollY);
                QCOMPARE(inspectItem->mapRectToItem(page,QRectF(0,0,inspectItem->width(),inspectItem->height())),inspectGeometry);
            }
            transport.state.activeState=u"active"_s; Q_EMIT transport.changed();
            QVERIFY(QMetaObject::invokeMethod(detailsButton,"clicked")); QVERIFY(!inspect->property("visible").toBool());
            QVERIFY(QMetaObject::invokeMethod(detailsButton,"clicked")); QVERIFY(inspect->property("visible").toBool());
            QVERIFY(QMetaObject::invokeMethod(detailsButton,"clicked"));
            QCOMPARE(console.values(),before); QVERIFY(console.modified());
            consolePage=goTo(1); QVERIFY(consolePage); QTest::qWait(60);   // reopening shows the same unsaved edit
            portEditor=find(consolePage,u"host_Port"_s); QVERIFY(portEditor); QCOMPARE(portEditor->property("value"),portValue);
        }
        // Inline certificate section: opens in place, cancel preserves other host edits.
        auto *certificateButton=find(consolePage,u"editHostCertificate"_s); QVERIFY(certificateButton);
        QVERIFY(!find(consolePage,u"certificateStandard"_s)->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(certificateButton,"clicked")); QCOMPARE(navigation.depth(),1);   // stays on the host page
        auto *draft=qobject_cast<BrokerHostSettings *>(console.certificateDraft()); QVERIFY(draft && draft->loaded());
        QTRY_VERIFY(find(consolePage,u"certificateStandard"_s)->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateStandard"_s),"clicked")); QCOMPARE(draft->tlsMode(),u"standard"_s); QCOMPARE(console.tlsMode(),u"keep"_s);
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"cancelCertificateEdit"_s),"clicked")); QCOMPARE(navigation.depth(),1);
        QCOMPARE(console.values(),before); QCOMPARE(console.tlsMode(),u"keep"_s);                  // Port edit kept, TLS untouched
        QVERIFY(!find(consolePage,u"certificateStandard"_s)->property("visible").toBool());
        // Re-opening starts from the saved choice again (Review focus 2).
        QVERIFY(QMetaObject::invokeMethod(certificateButton,"clicked"));
        QVERIFY(find(consolePage,u"certificateKeep"_s)->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateStandard"_s),"clicked")); QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"stageCertificateEdit"_s),"clicked"));
        QCOMPARE(navigation.depth(),1); QCOMPARE(console.tlsMode(),u"standard"_s); QCOMPARE(console.values()[u"Port"_s].toString(),u"3401"_s);
        // Advanced open/close never changes the draft state.
        { const auto modified=console.modified(); QVERIFY(consolePage->setProperty("showAdvanced",true)); QTest::qWait(50); QVERIFY(consolePage->setProperty("showAdvanced",false)); QCOMPARE(console.modified(),modified); QCOMPARE(console.tlsMode(),u"standard"_s); }
        // Leaving the page cancels an open, unstaged certificate draft only.
        QVERIFY(QMetaObject::invokeMethod(certificateButton,"clicked"));
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateKeep"_s),"clicked"));
        navigation.pop();                                                                   // native Back
        QCOMPARE(navigation.depth(),0);
        QCOMPARE(console.tlsMode(),u"standard"_s);                                          // earlier staged choice kept
        QCOMPARE(console.values()[u"Port"_s].toString(),u"3401"_s);
        consolePage=goTo(1); QVERIFY(consolePage); QTest::qWait(60);
        QVERIFY(!find(consolePage,u"certificateStandard"_s)->property("visible").toBool());   // the draft editor is closed again
        QVERIFY(find(consolePage,u"editHostCertificate"_s)->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"defaultHostSettings"_s),"clicked")); QCOMPARE(console.tlsMode(),u"standard"_s); console.discard();
        QVERIFY(virtualHost.reload()); QTRY_VERIFY(!virtualHost.busy()); QVERIFY(session.reload()); QTRY_VERIFY(!session.busy()); QVERIFY(auth.reload()); QTRY_VERIFY(!auth.busy()); QVERIFY(preferences.reload());
        {   // Review focus 3: the Virtual page has two independent save scopes.
            QQuickItem *virtualPage=goTo(2); QVERIFY(virtualPage); QTest::qWait(60);
            QObject *saveHost = find(virtualPage, u"saveHostSettings"_s);
            QObject *saveHw = find(virtualPage, u"saveDesktopHardware"_s);
            QVERIFY(saveHost && saveHw);
            QVERIFY(!saveHost->property("enabled").toBool());
            QVERIFY(!saveHw->property("enabled").toBool());
            session.setValue(u"VaapiDriver"_s, u"off"_s);                 // hardware edit
            QVERIFY(saveHw->property("enabled").toBool());
            QVERIFY(!saveHost->property("enabled").toBool());
            QVERIFY(QMetaObject::invokeMethod(find(virtualPage, u"discardDesktopHardware"_s), "clicked"));
            QVERIFY(!session.modified());
            QVERIFY(virtualHost.setValue(u"Port"_s, u"3402"_s));          // host edit
            QVERIFY(saveHost->property("enabled").toBool());
            QVERIFY(!saveHw->property("enabled").toBool());
            session.setValue(u"VaapiDriver"_s, u"off"_s);
            QVERIFY(QMetaObject::invokeMethod(find(virtualPage, u"defaultHostSettings"_s), "clicked"));   // host Restore Defaults is host scope only
            QVERIFY(session.modified());
            QVERIFY(QMetaObject::invokeMethod(find(virtualPage, u"discardHostSettings"_s), "clicked")); // host Revert is host scope only
            QVERIFY(!virtualHost.modified()); QVERIFY(session.modified());
            QVERIFY(QMetaObject::invokeMethod(find(virtualPage, u"defaultDesktopHardware"_s), "clicked"));
            QVERIFY(QMetaObject::invokeMethod(find(virtualPage, u"discardDesktopHardware"_s), "clicked"));
            QVERIFY(!session.modified()); QVERIFY(!virtualHost.modified());
        }
        QQuickItem *prefsPage=goTo(4); QVERIFY(prefsPage); QCOMPARE(prefsPage->objectName(),u"brokerPreferencesPage"_s);
        auto *mode=find(prefsPage,u"inherit_Quality"_s); QVERIFY(mode);
        QVERIFY(mode->setProperty("currentIndex",1)); QVERIFY(QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,1)));
        QVERIFY(preferences.values().contains(u"Quality"_s)); QCOMPARE(preferences.values()[u"Quality"_s].toString(),QString()); QVERIFY(!preferences.canSave());
        auto *prefQuality=find(prefsPage,u"preference_Quality"_s); QVERIFY(prefQuality); QVERIFY(prefQuality->setProperty("value",91)); QVERIFY(QMetaObject::invokeMethod(prefQuality,"valueModified")); QCOMPARE(preferences.values()[u"Quality"_s].toString(),u"91"_s);
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"specific"_s)); QVERIFY(preferences.setValue(u"MonitorIndex"_s,u"2"_s));
        auto *monitorField=qobject_cast<QQuickItem *>(find(prefsPage,u"preference_field_MonitorIndex"_s)); QVERIFY(monitorField); QTRY_VERIFY(monitorField->isVisible());
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"virtual"_s)); QTRY_VERIFY(!monitorField->isVisible()); QCOMPARE(preferences.values()[u"MonitorIndex"_s].toString(),u"2"_s);
        auto *policyField=qobject_cast<QQuickItem *>(find(prefsPage,u"preference_field_VirtualMonitorPolicy"_s)); QVERIFY(policyField); QTRY_VERIFY(policyField->isVisible());
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"workspace"_s)); QTRY_VERIFY(!policyField->isVisible()); preferences.discard(); QVERIFY(!preferences.modified());
        QVERIFY(goTo(3)); QVERIFY(QMetaObject::invokeMethod(item(u"consoleAddAlias"_s),"clicked"));
        auto *aliasDialog=item(u"brokerAliasDialog"_s); QVERIFY(aliasDialog); QTRY_VERIFY(aliasDialog->property("visible").toBool());
        auto *stageAlias=item(u"stageBrokerAlias"_s); QVERIFY(stageAlias);
        QVERIFY(item(u"brokerAliasName"_s)->setProperty("text",u"fixture-remote"_s)); QVERIFY(item(u"brokerAliasOwner"_s)->setProperty("text",u"westers"_s)); QVERIFY(!stageAlias->property("enabled").toBool());
        QVERIFY(item(u"brokerAliasPassword"_s)->setProperty("text",u"fixture-only-password"_s)); QTRY_VERIFY(stageAlias->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(stageAlias,"triggered")); QTRY_VERIFY(!aliasDialog->property("visible").toBool()); QCOMPARE(item(u"brokerAliasPassword"_s)->property("text").toString(),QString());
        QVERIFY(auth.modified()); QVERIFY(auth.removeAlias(u"console"_s,u"fixture-remote"_s)); QVERIFY(auth.undoRemoveAlias()); QCOMPARE(auth.policy()[u"console"_s].toMap()[u"credentials"_s].toList().size(),1); auth.discard();
        transport.state.activeState = u"active"_s; transport.state.mainPid = 42; Q_EMIT transport.changed();
        const auto screenshots=qEnvironmentVariable("FARSIDE_MAIN_SCREENSHOTS");
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"virtual"_s));
        QVERIFY(auth.setAlias(u"console"_s,u"fixture-remote"_s,u"westers"_s,u"fixture-only-password"_s));
        QVERIFY(console.inspectRuntime()); QTRY_VERIFY(!console.busy());
        QVERIFY(virtualHost.inspectRuntime()); QTRY_VERIFY(!virtualHost.busy());
        KRdp::ServerCertificate::Paths capturePair{dir.filePath(u"capture-certificate.crt"_s),dir.filePath(u"capture-private.key"_s)}; QString captureError;
        QVERIFY(KRdp::ServerCertificate::generate(capturePair,u"capture-fixture"_s,QDateTime::currentDateTimeUtc(),10,&captureError));
        const auto screenshot=[&](const QString &name){QTest::qWait(350); if(!screenshots.isEmpty()) QVERIFY(window.grabWindow().save(screenshots+u"/"_s+name+u".png"_s));};
        for(int index=0;index<5;++index) {
            QQuickItem *shown=goTo(index); QVERIFY(shown); screenshot(u"page-"_s+QString::number(index));
            if (index == 1 || index == 2 || index == 4) {
                auto *current = shown;
                QVERIFY(current->setProperty("showAdvanced",true));
                BrokerHostSettings *captureHost = index == 1 ? &console : index == 2 ? &virtualHost : nullptr; QObject *captureSection = nullptr;
                if (captureHost) {   // expand the inline certificate section in its import state for the capture
                    captureSection = find(current,u"certificateSection"_s); QVERIFY(captureSection); QVERIFY(QMetaObject::invokeMethod(captureSection,"begin"));
                    auto *draft=qobject_cast<BrokerHostSettings *>(captureHost->certificateDraft()); QVERIFY(draft);
                    QVERIFY(draft->chooseTls(u"import"_s));
                    captureSection->setProperty("certificateFile",QUrl::fromLocalFile(capturePair.certificate));
                    captureSection->setProperty("privateKeyFile",QUrl::fromLocalFile(capturePair.key));
                    QVERIFY(draft->importTls(QUrl::fromLocalFile(capturePair.certificate),QUrl::fromLocalFile(capturePair.key)));
                }
                auto *flickable = current->property("flickable").value<QQuickItem *>(); QVERIFY(flickable);
                QTest::qWait(100);
                const qreal end = qMax<qreal>(0,flickable->property("contentHeight").toReal()-flickable->height());
                int part=0;
                for(qreal offset=0;;offset=qMin(end,offset+flickable->height()*0.8)) {
                    flickable->setProperty("contentY",offset); screenshot(u"advanced-"_s+QString::number(index)+u"-"_s+QString::number(part++));
                    if(offset>=end)break;
                }
                QVERIFY(current->setProperty("showAdvanced",false)); flickable->setProperty("contentY",0); if (captureSection) QVERIFY(QMetaObject::invokeMethod(captureSection,"cancel"));
            }
        }
        QVERIFY(goTo(3)); aliasDialog=item(u"brokerAliasDialog"_s); QVERIFY(QMetaObject::invokeMethod(item(u"consoleAddAlias"_s),"clicked")); screenshot(u"alias-dialog"_s); QVERIFY(QMetaObject::invokeMethod(aliasDialog,"close"));
        QVERIFY(goTo(0));
        auto *details=page; QVERIFY(details);
        auto *enabled=find(details,u"consoleHostEnabled"_s); QVERIFY(enabled); QVERIFY(enabled->setProperty("checked",false)); QVERIFY(QMetaObject::invokeMethod(enabled,"clicked"));
        auto *confirm=find(details,u"consoleConfirmServiceOperation"_s); QVERIFY(confirm); QTRY_VERIFY(confirm->property("visible").toBool()); QCOMPARE(transport.mutations,0); screenshot(u"stop-dialog"_s); QVERIFY(QMetaObject::invokeMethod(confirm,"close")); QCOMPARE(transport.mutations,0);
        {   // Overview with the Console Details box expanded (collapsed state is page-0).
            QVERIFY(goTo(0)); QObject *toggle=item(u"consoleDetailsToggle"_s); QVERIFY(toggle);
            QVERIFY(QMetaObject::invokeMethod(toggle,"clicked")); screenshot(u"overview-details"_s);
            auto *overviewFlick=page->property("flickable").value<QQuickItem *>(); QVERIFY(overviewFlick);
            overviewFlick->setProperty("contentY",qMax<qreal>(0,overviewFlick->property("contentHeight").toReal()-overviewFlick->height())); screenshot(u"overview-details-bottom"_s);
            overviewFlick->setProperty("contentY",0); QVERIFY(QMetaObject::invokeMethod(toggle,"clicked"));
        }
        window.resize(640,360); navigation.resizeAll(window.size());
        for(int index=1;index<5;++index) {
            QQuickItem *shown=goTo(index); QVERIFY(shown); QTest::qWait(60);
            const auto visibleSave=[&](auto &&self,QQuickItem *parent)->QQuickItem * {
                if(parent->isVisible() && parent->objectName().startsWith(u"save") && parent->objectName()!=u"saveDesktopHardware"_s) return parent;   // the hardware save is inline content, not a fixed footer
                for (auto *child : parent->childItems()) { if (auto *result = self(self, child)) return result; }
                return nullptr;
            };
            if(auto *save=visibleSave(visibleSave,shown)) {
                const auto bounds=save->mapRectToItem(page,QRectF(0,0,save->width(),save->height()));
                QVERIFY2(bounds.left()>=0 && bounds.right()<=page->width()+1 && bounds.bottom()<=page->height()+1,qPrintable(QStringLiteral("Footer %1 exceeds %2x%3: %4,%5").arg(save->objectName()).arg(page->width()).arg(page->height()).arg(bounds.right()).arg(bounds.bottom())));
            }
        }
        window.resize(640,800); navigation.resizeAll(window.size());
        for(int index=0;index<3;++index) { QVERIFY(goTo(index)); screenshot(u"narrow-"_s+QString::number(index)); }
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u'\n')));
    }

};
QTEST_MAIN(BrokerMainPageTest)
#include "BrokerMainPageTest.moc"
