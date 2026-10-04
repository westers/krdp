// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerhostsettings.h"
#include "HostSnapshotFixture.h"
#include "brokerauthenticationsettings.h"
#include "brokerpreferences.h"
#include "brokerservices.h"
#include "brokersettingsapply.h"
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
// Stands in for the KCM page row of a master-detail module: the root page is the
// sidebar; push() creates the named page with the given initial properties beside
// it (or over it when columnWidth is not positive: the one-column drill-down), pop()
// destroys the top page. The real module is exercised by KcmUiTest; this keeps the
// fake models of this test.
class MainNavigation : public QObject {
    Q_OBJECT
    Q_PROPERTY(int depth READ depth NOTIFY depthChanged)
    Q_PROPERTY(int columnWidth READ columnWidth WRITE setColumnWidth NOTIFY columnWidthChanged)
public:
    QString copied; QQmlEngine *engine=nullptr; QQuickItem *container=nullptr; QString base; QQuickItem *root=nullptr;
    QList<QPointer<QQuickItem>> stack; QStringList pushed; int m_columnWidth=-1;
    int columnWidth() const { return m_columnWidth; }
    void setColumnWidth(int width) { if(width==m_columnWidth) return; m_columnWidth=width; layout(); Q_EMIT columnWidthChanged(); }
    Q_INVOKABLE void push(const QString &name,const QVariantMap &properties={}) {
        QQmlComponent component(engine,QUrl(base+name));
        if(!component.isReady()) { qWarning() << component.errorString(); return; }
        auto *item=qobject_cast<QQuickItem *>(component.createWithInitialProperties(properties));
        if(!item) { qWarning() << component.errorString(); return; }
        item->setParentItem(container); stack.append(item); pushed.append(name); layout();
        Q_EMIT depthChanged();
        // Let the page's delayed layout work (Kirigami FormLayout twins) run before it can be popped.
        QTest::qWait(30);
    }
    Q_INVOKABLE void pop() {   // Back: the top page goes away
        if(stack.isEmpty()) return;
        auto *item=stack.takeLast().data(); item->setParentItem(nullptr); delete item;
        layout(); Q_EMIT depthChanged();
    }
    Q_INVOKABLE void copyAddressToClipboard(const QString &text) { copied=text; }
    int depth() const { return 1+int(stack.size()); }   // like the shell: the sidebar counts
    QQuickItem *top() const { return stack.isEmpty() ? root : stack.last().data(); }
    void goHome() { while(!stack.isEmpty()) pop(); }
    void clear() { goHome(); }
    // Sidebar on the left and the detail page on the right; one column when columnWidth <= 0.
    void layout() {
        if(!container||!root) return;
        const QSizeF total=container->size(); const bool columns=m_columnWidth>0;
        root->setPosition({0,0}); root->setSize(columns ? QSizeF(m_columnWidth,total.height()) : total); root->setVisible(columns || stack.isEmpty());
        for(int i=0;i<stack.size();++i) if(auto *item=stack[i].data()) {
            item->setVisible(i==stack.size()-1);
            item->setPosition({columns ? qreal(m_columnWidth) : 0,0}); item->setSize(columns ? QSizeF(total.width()-m_columnWidth,total.height()) : total);
        }
    }
Q_SIGNALS:
    void depthChanged(); void columnWidthChanged();
};
// The context object real pages find as `kcm` (only what the shared banners need).
class FakeKcm : public QObject {
    Q_OBJECT
    Q_PROPERTY(BrokerSettingsApply *settingsApply READ settingsApply CONSTANT)
public:
    explicit FakeKcm(BrokerSettingsApply *apply) : m_apply(apply) {}
    BrokerSettingsApply *settingsApply() const { return m_apply; }
private:
    BrokerSettingsApply *m_apply;
};
class MainTransport : public BrokerServiceTransport {
    Q_OBJECT
public:
    BrokerServiceState state{true,u"loaded"_s,u"active"_s,u"running"_s,u"enabled"_s,42};
    BrokerServiceState virtualState{true,u"loaded"_s,u"inactive"_s,u"dead"_s,u"disabled"_s,0};
    bool split=false, defer=false;       // split: Virtual has its own state; defer: complete operations on finish()
    int mutations=0, lastRoute=-1, reads=0;
    Operation lastOperation=Start; Done pending;
    void query(int route,QueryDone done) override { ++reads; done(split&&route==1?virtualState:state,{}); }
    void operate(int route,Operation operation,Done done) override { ++mutations;lastRoute=route;lastOperation=operation; if(defer) pending=std::move(done); else done({}); }
    void finish(const QString &error={}) { auto done=std::move(pending); done(error); }
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
        MainTransport transport; MainNavigation navigation; BrokerServices services; BrokerSettingsApply apply; FakeKcm kcmObject;
        QQmlEngine engine; QScopedPointer<QObject> object; QQuickItem *page=nullptr; QQuickWindow window;
        Fixture() : protocol{qEnvironmentVariable("FARSIDE_HOST_TEST_FIXTURE",QString::fromUtf8(MAIN_PROTOCOL_FIXTURE)),dir.path()},
            console(Scope::Console,u"/usr/bin/python3"_s,protocol,3000), virtualHost(Scope::Virtual,u"/usr/bin/python3"_s,protocol,3000), session(Scope::VirtualSession,u"/usr/bin/python3"_s,protocol,3000),
            auth(u"/usr/bin/python3"_s,{qEnvironmentVariable("FARSIDE_AUTH_TEST_FIXTURE",QString::fromUtf8(MAIN_AUTH_PROTOCOL_FIXTURE)),dir.path()},nullptr),
            preferences(dir.path()), services(&transport), apply(&auth,&console,&virtualHost,&session,&preferences), kcmObject(&apply) {}
        ~Fixture() { navigation.clear(); }
        // Items of the visible page first, then the overview underneath.
        QObject *item(const QString &name) {
            for(int i=int(navigation.stack.size())-1;i>=0;--i) if(auto *value=find(navigation.stack[i],name)) return value;
            return find(page,name);
        }
        QQuickItem *goTo(int index) {   // 0 Console, 1 Virtual, 2 Who Can Connect, 3 My Preferences; -1 the sidebar alone
            navigation.goHome();
            static const char *const rows[]={"sidebar_console","sidebar_virtual","sidebar_access","sidebar_preferences"};
            if(index<0) return page;
            QObject *row=find(page,QString::fromLatin1(rows[index]));
            if(!row || !QMetaObject::invokeMethod(row,"clicked")) return nullptr;
            return navigation.top();
        }
        void resize(const QSize &size) {   // the window is the view: crossing the split width switches the layout mode
            window.resize(size); QTRY_COMPARE(window.contentItem()->width(),qreal(size.width())); QCoreApplication::processEvents(); navigation.layout();
        }
        bool init() {
            auto *localized=new KLocalizedQmlContext(&engine); localized->setTranslationDomain(u"kcm_farside"_s); engine.rootContext()->setContextObject(localized);
            engine.rootContext()->setContextProperty(u"kcm"_s,&kcmObject);
            // The sidebar opens Console while it is being created, so the page stack must be ready before.
            navigation.engine=&engine; navigation.container=window.contentItem(); navigation.base=QUrl::fromLocalFile(QFileInfo(qEnvironmentVariable("FARSIDE_MAIN_TEST_PAGE",QString::fromUtf8(MAIN_PAGE))).absolutePath()+u"/"_s).toString();
            QQmlComponent component(&engine,QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_MAIN_TEST_PAGE",QString::fromUtf8(MAIN_PAGE))));
            if(!component.isReady()) { qWarning() << component.errorString(); return false; }
            object.reset(component.createWithInitialProperties({{u"navigation"_s,QVariant::fromValue(&navigation)},{u"administration"_s,QVariant::fromValue(&services)},
                {u"consoleHost"_s,QVariant::fromValue(&console)},{u"virtualHost"_s,QVariant::fromValue(&virtualHost)},{u"sessionSettings"_s,QVariant::fromValue(&session)},
                {u"authentication"_s,QVariant::fromValue(&auth)},{u"preferences"_s,QVariant::fromValue(&preferences)}}));
            page=qobject_cast<QQuickItem *>(object.data()); if(!page) return false;
            navigation.root=page;
            window.resize(900,850); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
            navigation.layout(); QTest::qWait(100);      // the list creates its rows
            return true;
        }
    };
    static int controlsOn(QQuickItem *item) {   // visible interactive controls under an item
        if(!item->isVisible()) return 0;
        int count=(item->inherits("QQuickAbstractButton")||item->inherits("QQuickComboBox")||item->inherits("QQuickSpinBox")||item->inherits("QQuickSlider")||item->inherits("QQuickTextField")) ? 1 : 0;
        for(auto *child:item->childItems()) count+=controlsOn(child);
        return count;
    }
private Q_SLOTS:
    void sidebarHasFourRowsBesideTheOpenPage() {
        Fixture f; f.transport.split=true; f.transport.virtualState.mainPid=0; QVERIFY(f.init()); loadAll(f);
        // Wide: the sidebar opens Console beside itself so the detail pane is never blank.
        QTRY_COMPARE(f.navigation.depth(),2); QCOMPARE(f.navigation.top()->objectName(),u"consoleSettingsPage"_s);
        QCOMPARE(f.navigation.columnWidth(),int(f.page->property("sidebarWidth").toReal()));
        QVERIFY(f.page->isVisible()); QVERIFY(f.navigation.top()->isVisible()); QVERIFY(f.navigation.top()->x()>=f.page->width());
        const struct { const char *key; const char *page; } rows[]={{"console","consoleSettingsPage"},{"virtual","virtualSettingsPage"},{"access","brokerSignInPage"},{"preferences","brokerPreferencesPage"}};
        auto *list=qobject_cast<QQuickItem *>(find(f.page,u"sidebarList"_s)); QVERIFY(list); QCOMPARE(list->property("count").toInt(),4);
        int index=0;
        for(const auto &row:rows) {
            auto *item=qobject_cast<QQuickItem *>(find(f.page,u"sidebar_"_s+QString::fromLatin1(row.key))); QVERIFY2(item,row.key); QVERIFY(item->isVisible());
            QVERIFY(QMetaObject::invokeMethod(item,"clicked")); QCOMPARE(f.navigation.depth(),2); QCOMPARE(f.navigation.top()->objectName(),QString::fromLatin1(row.page));
            QCOMPARE(f.page->property("currentIndex").toInt(),index); QVERIFY(item->property("highlighted").toBool()); QVERIFY(f.page->isVisible());   // the sidebar stays
            ++index;
        }
    }
    void routeRowsShowStateAddressAndTrailingSwitch() {
        Fixture f; f.transport.split=true; f.transport.virtualState.mainPid=0; QVERIFY(f.init()); loadAll(f);
        const auto subtitle=[&](const char *key) { auto *o=find(f.page,QString::fromLatin1(key)+u"Subtitle"_s); return o?o->property("subtitle").toString():QString(u"<missing>"_s); };
        QTRY_VERIFY(subtitle("console").startsWith(u"Running"_s)); QVERIFY2(subtitle("console").contains(QRegularExpression(uR"((:|port )\d+$)"_s)),qPrintable(subtitle("console")));
        QVERIFY2(!subtitle("console").contains(u"test-host"_s),"a wildcard listener must not be given an invented host name");   // the configured address only
        QTRY_VERIFY(subtitle("virtual").startsWith(u"Stopped"_s));
        QVERIFY(find(f.page,u"consoleHostEnabled"_s)->property("checked").toBool()); QVERIFY(!find(f.page,u"virtualHostEnabled"_s)->property("checked").toBool());
        QVERIFY(find(f.page,u"consoleHostEnabled"_s)->property("visible").toBool());
        QVERIFY(!find(f.page,u"accessHostEnabled"_s)); QVERIFY(!find(f.page,u"preferencesHostEnabled"_s));   // only the two routes have a switch
        // The rows carry no extra navigation: no launcher buttons, footer or Details expander remain.
        for(const auto &old:{u"configureConsole"_s,u"configureVirtual"_s,u"configureAccess"_s,u"configurePreferences"_s,u"consoleDetailsToggle"_s,u"consoleRestart"_s,u"consoleStop"_s,u"restartRequired"_s})
            QVERIFY2(!find(f.page,old),qPrintable(old));
        f.transport.virtualState.activeState=u"active"_s; f.transport.virtualState.mainPid=7; Q_EMIT f.transport.changed();
        QTRY_VERIFY(subtitle("virtual").startsWith(u"Running"_s)); QTRY_VERIFY(find(f.page,u"virtualHostEnabled"_s)->property("checked").toBool());
        // A pending edit marks its row.
        QVERIFY(!qobject_cast<QQuickItem *>(find(f.page,u"consoleModified"_s))->isVisible());
        QVERIFY(f.console.setValue(u"Port"_s,u"3401"_s)); QTRY_VERIFY(qobject_cast<QQuickItem *>(find(f.page,u"consoleModified"_s))->isVisible());
        QVERIFY(!qobject_cast<QQuickItem *>(find(f.page,u"virtualModified"_s))->isVisible());
        QVERIFY(f.session.setValue(u"VaapiDriver"_s,u"off"_s)); QTRY_VERIFY(qobject_cast<QQuickItem *>(find(f.page,u"virtualModified"_s))->isVisible());
        f.console.discard(); f.session.discard(); QTRY_VERIFY(!qobject_cast<QQuickItem *>(find(f.page,u"consoleModified"_s))->isVisible());
    }
    void everyPageIsPopulated() {
        Fixture f; QVERIFY(f.init()); loadAll(f);
        for(int index=0;index<4;++index) {
            QQuickItem *shown=f.goTo(index); QVERIFY(shown); QTest::qWait(80);
            QVERIFY2(controlsOn(shown)>=3,qPrintable(u"page %1 has %2 controls"_s.arg(index).arg(controlsOn(shown))));
        }
        // The same pages with the optional parts open have more, never fewer.
        QQuickItem *consolePage=f.goTo(0); QVERIFY(consolePage); const int before=controlsOn(consolePage); QVERIFY(consolePage->setProperty("showAdvanced",true)); QTest::qWait(80); QVERIFY(controlsOn(consolePage)>before);
    }
    void narrowDetailPaneNeverClips() {
        // The shell decides between two columns and one; in two columns at a narrow window the
        // detail pane is only (window - sidebar) wide, and no page may need sideways scrolling there.
        Fixture f; QVERIFY(f.init()); loadAll(f);
        QTRY_COMPARE(f.navigation.depth(),2); QVERIFY(f.navigation.columnWidth()>0);
        f.resize({640,800});
        for(int index=0;index<4;++index) {
            QQuickItem *shown=f.goTo(index); QVERIFY(shown); QTest::qWait(80);
            QCOMPARE(shown->width(),qreal(640-f.navigation.columnWidth()));
            QVERIFY(f.page->isVisible());                                   // the sidebar stays beside it
            auto *flickable=shown->property("flickable").value<QQuickItem *>(); QVERIFY(flickable);
            QVERIFY2(flickable->property("contentWidth").toReal()<=flickable->width()+0.5,qPrintable(u"page %1 scrolls sideways at 640"_s.arg(index)));
        }
        f.navigation.pop(); QVERIFY(f.page->isVisible()); QCOMPARE(f.page->property("currentIndex").toInt(),-1);   // Back leaves the list alone
    }
    void inventoryIsReachableThroughTheSidebar() {
        Fixture f; QVERIFY(f.init()); loadAll(f);
        // Host entries: every definition of the Console, Virtual and new-desktop scopes has a visible, enabled control.
        QSet<QString> unavailable; int hostEntries=0;
        const struct { BrokerHostSettings *model; int row; const char *prefix; } scopes[]={{&f.console,0,"host_"},{&f.virtualHost,1,"host_"},{&f.session,1,"desktop_"}};
        for(const auto &scope:scopes) {
            QQuickItem *shown=f.goTo(scope.row); QVERIFY(shown); QVERIFY(shown->setProperty("showAdvanced",true)); QTest::qWait(80);
            auto *section=find(shown,u"certificateSection"_s); QVERIFY(section);
            for(const auto &definition:scope.model->definitions()) {
                const auto key=definition.toMap()[u"key"_s].toString(); ++hostEntries;
                if(key==u"CameraLoopbackDevice" && scope.row==1) {   // Virtual shows the camera as unavailable instead of editable
                    auto *notice=qobject_cast<QQuickItem *>(find(shown,u"cameraReadiness"_s)); QVERIFY(notice); QVERIFY(notice->isVisible()); QVERIFY(notice->property("text").toString().contains(u"not available"_s));
                    unavailable.insert(key); continue;
                }
                if(key.startsWith(u"Certificate")) {   // inside the certificate editor, in its existing-paths mode
                    if(!section->property("open").toBool()) { QVERIFY(QMetaObject::invokeMethod(section,"begin")); }
                    auto *draft=qobject_cast<BrokerHostSettings *>(scope.model->certificateDraft()); QVERIFY(draft); QVERIFY(draft->chooseTls(u"existing"_s)); QTest::qWait(60);
                    auto *control=qobject_cast<QQuickItem *>(find(shown,u"certificate_"_s+key)); QVERIFY2(control,qPrintable(key)); QVERIFY2(control->isVisible()&&control->isEnabled(),qPrintable(key));
                    continue;
                }
                const QString name=QString::fromLatin1(scope.prefix)+(key==u"Address" ? u"AddressMode"_s : key);
                auto *control=qobject_cast<QQuickItem *>(find(shown,name)); QVERIFY2(control,qPrintable(name)); QVERIFY2(control->isVisible()&&control->isEnabled(),qPrintable(name));
            }
            if(section->property("open").toBool()) QVERIFY(QMetaObject::invokeMethod(section,"cancel"));
        }
        QCOMPARE(hostEntries,25); QVERIFY(unavailable.contains(u"CameraLoopbackDevice"_s));
        // Account preferences: every one of the 17 keys has a visible, enabled control (an inherit/custom selector until overridden).
        QQuickItem *prefs=f.goTo(3); QVERIFY(prefs); QVERIFY(prefs->setProperty("showAdvanced",true));
        const auto definitions=f.preferences.definitions(); QCOMPARE(definitions.size(),17);
        const QStringList modeKeys{u"MonitorIndex"_s,u"VirtualMonitorPolicy"_s,u"VirtualMonitorLayout"_s,u"VirtualMonitorFallbackSize"_s};
        for(const auto &mode:{u"specific"_s,u"virtual"_s}) {
            QVERIFY(f.preferences.setValue(u"MonitorMode"_s,mode)); QTest::qWait(60);
            for(const auto &definition:definitions) {
                const auto key=definition.toMap()[u"key"_s].toString();
                if(modeKeys.contains(key) && !(mode==u"specific" ? key==u"MonitorIndex" : key!=u"MonitorIndex")) continue;   // shown only for its own display mode
                auto *field=qobject_cast<QQuickItem *>(find(prefs,u"preference_field_"_s+key)); QVERIFY2(field,qPrintable(key)); QVERIFY2(field->isVisible(),qPrintable(key+u" in "_s+mode));
                QObject *control=nullptr; for(const auto &candidate:{u"preference_"_s+key,u"inherit_"_s+key}) if(auto *o=find(prefs,candidate)) { auto *i=qobject_cast<QQuickItem *>(o); if(i && i->isVisible()) { control=o; break; } }
                QVERIFY2(control,qPrintable(key)); QVERIFY2(qobject_cast<QQuickItem *>(control)->isEnabled(),qPrintable(key));
            }
        }
        f.preferences.discard();
        // Access policy: the four fields (who may sign in, and the allowed accounts once selected) for both routes.
        QVERIFY(f.auth.setPam(u"console"_s,u"allow-list"_s,{u"westers"_s})); QVERIFY(f.auth.setPam(u"virtual"_s,u"allow-list"_s,{u"westers"_s}));
        QQuickItem *access=f.goTo(2); QVERIFY(access); QTest::qWait(80);
        for(const auto &name:{u"consolePamMode"_s,u"virtualPamMode"_s,u"consolePamAccounts"_s,u"virtualPamAccounts"_s}) {
            auto *control=qobject_cast<QQuickItem *>(find(access,name)); QVERIFY2(control,qPrintable(name)); QVERIFY2(control->isVisible()&&control->isEnabled(),qPrintable(name));
        }
        f.auth.discard();
    }
    void refreshStatusReachableInTroubleshooting() {
        Fixture f; QVERIFY(f.init());
        QQuickItem *consolePage=f.goTo(0); QVERIFY(consolePage); QVERIFY(f.console.reload()); QTRY_VERIFY(f.console.loaded()); QTest::qWait(60);
        QVERIFY(!findVisible(consolePage,u"consoleRefreshStatus"_s));          // collapsed under Advanced
        QVERIFY(consolePage->setProperty("showAdvanced",true)); QTest::qWait(60);
        QObject *refresh=findVisible(consolePage,u"consoleRefreshStatus"_s); QVERIFY(refresh);
        QTRY_VERIFY(refresh->property("enabled").toBool());
        const int reads=f.transport.reads;
        QVERIFY(QMetaObject::invokeMethod(refresh,"clicked")); QTRY_VERIFY(f.transport.reads>reads);
        QVERIFY(findVisible(consolePage,u"inspectHostRuntime"_s));
    }
    void leavingHostPageAnyWayCancelsCertificateDraft() {
        Fixture f; QVERIFY(f.init());
        QVERIFY(f.console.reload()); QTRY_VERIFY(!f.console.busy()); QVERIFY(f.console.loaded());
        QQuickItem *consolePage=f.goTo(0); QVERIFY(consolePage); QCOMPARE(f.navigation.depth(),2); QCOMPARE(consolePage->objectName(),u"consoleSettingsPage"_s);
        QVERIFY(f.console.setValue(u"Port"_s,u"3401"_s));
        QPointer<QObject> section=find(consolePage,u"certificateSection"_s); QVERIFY(section);
        QTRY_VERIFY(find(consolePage,u"editHostCertificate"_s)->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"editHostCertificate"_s),"clicked")); QVERIFY(section->property("open").toBool());
        // The saved certificate's own details (paths, full fingerprint) start collapsed and open on request; cancelling closes them again.
        auto *details=qobject_cast<QQuickItem *>(find(consolePage,u"certificateDetails"_s)); QVERIFY(details); QVERIFY(!details->isVisible());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateDetailsToggle"_s),"clicked")); QTRY_VERIFY(details->isVisible());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateDetailsToggle"_s),"clicked")); QTRY_VERIFY(!details->isVisible());
        QTRY_VERIFY(find(consolePage,u"certificateImport"_s)->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateImport"_s),"clicked"));
        // "Change…" next to the shared screens switches the detail pane to My Preferences, scrolled to the displays.
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"consoleDisplayPreferences"_s),"clicked"));
        QTRY_COMPARE(f.navigation.top()->objectName(),u"brokerPreferencesPage"_s); QCOMPARE(f.navigation.depth(),2);
        QVERIFY(f.navigation.top()->property("scrollToDisplays").toBool());
        QCOMPARE(f.console.tlsMode(),u"keep"_s); QCOMPARE(f.console.values()[u"Port"_s].toString(),u"3401"_s);   // unstaged certificate draft gone, other edit kept
        auto *draft=qobject_cast<BrokerHostSettings *>(f.console.certificateDraft()); QVERIFY(!draft || !draft->loaded() || draft->tlsMode()==u"keep"_s);
        consolePage=f.goTo(0); QVERIFY(consolePage); QTest::qWait(60);   // back on Console: editor closed, edit still pending
        QVERIFY(!find(consolePage,u"certificateStandard"_s)->property("visible").toBool()); QCOMPARE(f.console.values()[u"Port"_s].toString(),u"3401"_s);
    }
    void backFromHostPageCancelsCertificateDraft() {
        Fixture f; QVERIFY(f.init());
        QVERIFY(f.console.reload()); QTRY_VERIFY(!f.console.busy()); QVERIFY(f.console.loaded());
        QQuickItem *consolePage=f.goTo(0); QVERIFY(consolePage);
        QTRY_VERIFY(find(consolePage,u"editHostCertificate"_s)->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"editHostCertificate"_s),"clicked"));
        QTRY_VERIFY(find(consolePage,u"certificateStandard"_s)->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateStandard"_s),"clicked"));
        auto *draft=qobject_cast<BrokerHostSettings *>(f.console.certificateDraft()); QVERIFY(draft && draft->loaded()); QCOMPARE(draft->tlsMode(),u"standard"_s);
        f.navigation.pop();                                                  // the native Back (kcm.pop())
        QCOMPARE(f.navigation.depth(),1);
        QVERIFY(f.page->isVisible());
        draft=qobject_cast<BrokerHostSettings *>(f.console.certificateDraft());
        QVERIFY(!draft || !draft->loaded() || draft->tlsMode()==u"keep"_s);  // the unstaged draft is gone
        QCOMPARE(f.console.tlsMode(),u"keep"_s);
    }
    void sidebarSelectsOneDestinationAtATime() {
        Fixture f; QVERIFY(f.init());
        QCOMPARE(f.navigation.depth(),2);   // wide: Console is already open beside the sidebar
        const struct { int index; const char *objectName; } destinations[]={{0,"consoleSettingsPage"},{1,"virtualSettingsPage"},{2,"brokerSignInPage"},{3,"brokerPreferencesPage"}};
        for(const auto &d:destinations) {
            QQuickItem *page=f.goTo(d.index); QVERIFY(page); QCOMPARE(f.navigation.depth(),2); QCOMPARE(page->objectName(),QString::fromLatin1(d.objectName));
            QVERIFY(page->isVisible()); QVERIFY(f.page->isVisible());          // beside the sidebar, never over it
            auto *other=qobject_cast<QQuickItem *>(find(f.page,u"sidebar_"_s+QString::fromLatin1(d.index==0?"virtual":"console")));
            QVERIFY(QMetaObject::invokeMethod(other,"clicked")); QCOMPARE(f.navigation.depth(),2); QVERIFY(f.navigation.top()!=page);   // a new selection replaces the page, it does not stack
        }
        f.navigation.goHome();
        // The custom header, Back button and stacked layout are gone with the native navigation.
        QVERIFY(!find(f.page,u"settingsBack"_s)); QVERIFY(!f.page->property("currentPage").isValid());
    }
    void accessPromptsOnlyWhenOpened() {
        Fixture f; QVERIFY(f.init()); QVERIFY(!f.auth.loaded());
        QTRY_COMPARE(f.navigation.depth(),2);   // the automatically opened Console page never asks for authorization
        QTest::qWait(150); QVERIFY(!f.auth.loaded()); QVERIFY(!f.auth.busy());
        QQuickItem *access=f.goTo(2); QVERIFY(access);   // the user opened it: the one prompt
        QTRY_VERIFY(f.auth.loaded()); QTRY_VERIFY(!f.auth.busy()); QTest::qWait(60);
        QVERIFY(!findVisible(access,u"accessLocked"_s)); QVERIFY(findVisible(access,u"consolePamMode"_s));
    }
    void pendingScopesMarkTheirSidebarRows() {
        Fixture f; QVERIFY(f.init());
        QVERIFY(f.virtualHost.reload()); QTRY_VERIFY(!f.virtualHost.busy()); QVERIFY(f.session.reload()); QTRY_VERIFY(!f.session.busy());
        QQuickItem *virtualPage=f.goTo(1); QVERIFY(virtualPage); QTest::qWait(60);
        auto *mark=qobject_cast<QQuickItem *>(find(f.page,u"virtualModified"_s)); QVERIFY(mark); QVERIFY(!mark->isVisible());
        f.session.setValue(u"VaapiDriver"_s,u"off"_s);                       // new-desktop edit only
        QTRY_VERIFY(mark->isVisible()); QVERIFY(!f.virtualHost.modified());
        f.session.discard(); QTRY_VERIFY(!mark->isVisible());
        QVERIFY(f.virtualHost.setValue(u"Port"_s,u"3402"_s)); QTRY_VERIFY(mark->isVisible());
        f.virtualHost.discard(); QTRY_VERIFY(!mark->isVisible());
        // There is no per-page pending-summary line: the sidebar mark and the shell's Apply bar say it.
        QVERIFY(!find(virtualPage,u"hostPendingSummary"_s));
    }
    void resetAndDefaultsCloseCertificateSection() {
        QTemporaryDir snapshots; QVERIFY(HostSnapshotFixture::publishAll(snapshots.path()));
        qputenv("FARSIDE_PUBLIC_SETTINGS_DIR", snapshots.path().toUtf8()); // Reload reads the public snapshot
        const auto restoreEnvironment=qScopeGuard([]{qunsetenv("FARSIDE_PUBLIC_SETTINGS_DIR");});
        Fixture f; QVERIFY(f.init());
        QVERIFY(f.console.reload()); QTRY_VERIFY(!f.console.busy()); QVERIFY(f.console.loaded());
        QQuickItem *consolePage=f.goTo(0); QVERIFY(consolePage);
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
        // Reset (standard bar): drops the draft, re-reads, and closes the open editor.
        QVERIFY(f.console.setValue(u"Port"_s,u"3401"_s)); openStandard();
        f.apply.reset(); closedAndCancelled();
        QCOMPARE(f.console.tlsMode(),u"keep"_s); QVERIFY(!f.console.modified());
        // Defaults (standard bar) closes it too and never stages a certificate choice.
        openStandard();
        f.apply.useDefaults(); closedAndCancelled();
        QCOMPARE(f.console.tlsMode(),u"keep"_s); f.console.discard();
        // There is no per-page Revert/Defaults/Reload any more.
        for(const auto &old:{u"saveHostSettings"_s,u"discardHostSettings"_s,u"defaultHostSettings"_s,u"loadHostSettings"_s,u"reloadHostAccept"_s}) QVERIFY2(!find(consolePage,old),qPrintable(old));
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
        QQuickWindow window; navigation.engine=&engine; navigation.container=window.contentItem();
        navigation.base=packaged ? u"qrc:/kcm/kcm_farside/"_s : QUrl::fromLocalFile(QFileInfo(qEnvironmentVariable("FARSIDE_MAIN_TEST_PAGE",QString::fromUtf8(MAIN_PAGE))).absolutePath()+u"/"_s).toString();
        QQmlComponent component(&engine,packaged ? QUrl(u"qrc:/kcm/kcm_farside/BrokerMainPage.qml"_s) : QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_MAIN_TEST_PAGE",QString::fromUtf8(MAIN_PAGE))));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"navigation"_s,QVariant::fromValue(&navigation)},{u"administration"_s,QVariant::fromValue(&services)},
            {u"consoleHost"_s,QVariant::fromValue(&console)},{u"virtualHost"_s,QVariant::fromValue(&virtualHost)},{u"sessionSettings"_s,QVariant::fromValue(&session)},
            {u"authentication"_s,QVariant::fromValue(&auth)},{u"preferences"_s,QVariant::fromValue(&preferences)}}));
        QVERIFY2(object,qPrintable(component.errorString())); auto *page=qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
        navigation.root=page; window.resize(900,850); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show(); navigation.layout(); QTest::qWait(100);
        const auto cleanup=qScopeGuard([&]{navigation.clear();});
        const auto item=[&](const QString &name)->QObject *{
            for(int i=int(navigation.stack.size())-1;i>=0;--i) if(auto *value=find(navigation.stack[i],name)) return value;
            return find(page,name);
        };
        const auto goTo=[&](int index)->QQuickItem *{   // 0 Console, 1 Virtual, 2 Who Can Connect, 3 My Preferences
            navigation.goHome(); static const char *const rows[]={"sidebar_console","sidebar_virtual","sidebar_access","sidebar_preferences"};
            if(!QMetaObject::invokeMethod(find(page,QString::fromLatin1(rows[index])),"clicked")) return nullptr;
            return navigation.top();
        };
        QVERIFY(!console.loaded()); QVERIFY(!virtualHost.loaded()); QVERIFY(!session.loaded()); QVERIFY(!auth.loaded()); QVERIFY(!preferences.loaded());
        QTRY_COMPARE(navigation.depth(),2);   // wide: the sidebar opened Console beside itself, with nothing loaded yet
        QQuickItem *consolePage=goTo(0); QVERIFY(consolePage); QCOMPARE(navigation.depth(),2);
        QVERIFY(find(consolePage,u"hostNotPublished"_s)->property("visible").toBool());   // not loaded: a clear message, no empty form
        // Troubleshooting lives under Advanced and is hidden until opened.
        QObject *inspect = item(u"inspectHostRuntime"_s);
        QVERIFY(inspect);
        QVERIFY(!inspect->property("visible").toBool());
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
        {   // Back destroys the page and keeps the draft in the model; troubleshooting keeps its editors across service events.
            auto *portEditor=find(consolePage,u"host_Port"_s); QVERIFY(portEditor);
            const auto portValue=portEditor->property("value");
            navigation.pop(); QCOMPARE(navigation.depth(),1); QTest::qWait(60);   // Back destroys the page; the draft lives in the model
            QCOMPARE(console.values(),before); QVERIFY(console.modified());
            consolePage=goTo(0); QVERIFY(consolePage); QTest::qWait(60);           // reopening shows the same unsaved edit
            QVERIFY(consolePage->setProperty("showAdvanced",true)); QTest::qWait(150);
            inspect=item(u"inspectHostRuntime"_s); QVERIFY(inspect); QVERIFY(inspect->property("visible").toBool());
            auto *inspectItem=qobject_cast<QQuickItem *>(inspect); QVERIFY(inspectItem);
            const auto inspectGeometry=inspectItem->mapRectToItem(page,QRectF(0,0,inspectItem->width(),inspectItem->height()));
            for(int update=0;update<4;++update) {
                transport.state.activeState=update%2?u"active"_s:u"inactive"_s; Q_EMIT transport.changed(); QTest::qWait(30);
                QCOMPARE(item(u"inspectHostRuntime"_s),inspect); QVERIFY(inspect->property("visible").toBool());
                QCOMPARE(inspectItem->mapRectToItem(page,QRectF(0,0,inspectItem->width(),inspectItem->height())),inspectGeometry);
            }
            transport.state.activeState=u"active"_s; Q_EMIT transport.changed();
            QVERIFY(consolePage->setProperty("showAdvanced",false)); QVERIFY(!inspect->property("visible").toBool());
            QVERIFY(consolePage->setProperty("showAdvanced",true)); QVERIFY(inspect->property("visible").toBool());
            QVERIFY(consolePage->setProperty("showAdvanced",false));
            QCOMPARE(console.values(),before); QVERIFY(console.modified());
            portEditor=find(consolePage,u"host_Port"_s); QVERIFY(portEditor); QCOMPARE(portEditor->property("value"),portValue);
        }
        // Inline certificate section: opens in place, cancel preserves other host edits.
        auto *certificateButton=find(consolePage,u"editHostCertificate"_s); QVERIFY(certificateButton);
        QVERIFY(!find(consolePage,u"certificateStandard"_s)->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(certificateButton,"clicked")); QCOMPARE(navigation.depth(),2);   // stays on the host page
        auto *draft=qobject_cast<BrokerHostSettings *>(console.certificateDraft()); QVERIFY(draft && draft->loaded());
        QTRY_VERIFY(find(consolePage,u"certificateStandard"_s)->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateStandard"_s),"clicked")); QCOMPARE(draft->tlsMode(),u"standard"_s); QCOMPARE(console.tlsMode(),u"keep"_s);
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"cancelCertificateEdit"_s),"clicked")); QCOMPARE(navigation.depth(),2);
        QCOMPARE(console.values(),before); QCOMPARE(console.tlsMode(),u"keep"_s);                  // Port edit kept, TLS untouched
        QVERIFY(!find(consolePage,u"certificateStandard"_s)->property("visible").toBool());
        // Re-opening starts from the saved choice again (Review focus 2).
        QVERIFY(QMetaObject::invokeMethod(certificateButton,"clicked"));
        QVERIFY(find(consolePage,u"certificateKeep"_s)->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateStandard"_s),"clicked")); QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"stageCertificateEdit"_s),"clicked"));
        QCOMPARE(navigation.depth(),2); QCOMPARE(console.tlsMode(),u"standard"_s); QCOMPARE(console.values()[u"Port"_s].toString(),u"3401"_s);
        // Advanced open/close never changes the draft state.
        { const auto modified=console.modified(); QVERIFY(consolePage->setProperty("showAdvanced",true)); QTest::qWait(50); QVERIFY(consolePage->setProperty("showAdvanced",false)); QCOMPARE(console.modified(),modified); QCOMPARE(console.tlsMode(),u"standard"_s); }
        // Leaving the page cancels an open, unstaged certificate draft only.
        QVERIFY(QMetaObject::invokeMethod(certificateButton,"clicked"));
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"certificateKeep"_s),"clicked"));
        navigation.pop();                                                                   // native Back
        QCOMPARE(navigation.depth(),1);
        QCOMPARE(console.tlsMode(),u"standard"_s);                                          // earlier staged choice kept
        QCOMPARE(console.values()[u"Port"_s].toString(),u"3401"_s);
        consolePage=goTo(0); QVERIFY(consolePage); QTest::qWait(60);
        QVERIFY(!find(consolePage,u"certificateStandard"_s)->property("visible").toBool());   // the draft editor is closed again
        QVERIFY(find(consolePage,u"editHostCertificate"_s)->property("visible").toBool());
        console.defaults(); QCOMPARE(console.tlsMode(),u"standard"_s); console.discard();   // Defaults keep the staged certificate choice
        QVERIFY(virtualHost.reload()); QTRY_VERIFY(!virtualHost.busy()); QVERIFY(session.reload()); QTRY_VERIFY(!session.busy()); QVERIFY(auth.reload()); QTRY_VERIFY(!auth.busy()); QVERIFY(preferences.reload());
        {   // The Virtual page edits two independent drafts (host, new-desktop hardware); the standard Apply saves both.
            QQuickItem *virtualPage=goTo(1); QVERIFY(virtualPage); QTest::qWait(60);
            for(const auto &old:{u"saveHostSettings"_s,u"saveDesktopHardware"_s,u"discardHostSettings"_s,u"discardDesktopHardware"_s,u"defaultHostSettings"_s,u"defaultDesktopHardware"_s,u"loadHostSettings"_s,u"loadDesktopHardware"_s})
                QVERIFY2(!find(virtualPage,old),qPrintable(old));
            QVERIFY(!session.modified()); QVERIFY(!virtualHost.modified());
            session.setValue(u"VaapiDriver"_s, u"off"_s);                 // hardware edit
            QVERIFY(session.modified()); QVERIFY(!virtualHost.modified());
            QVERIFY(virtualHost.setValue(u"Port"_s, u"3402"_s));          // host edit
            QVERIFY(session.modified()); QVERIFY(virtualHost.modified());
            session.discard(); QVERIFY(!session.modified()); QVERIFY(virtualHost.modified());
            virtualHost.discard(); QVERIFY(!virtualHost.modified());
        }
        QQuickItem *prefsPage=goTo(3); QVERIFY(prefsPage); QCOMPARE(prefsPage->objectName(),u"brokerPreferencesPage"_s);
        auto *mode=find(prefsPage,u"inherit_Quality"_s); QVERIFY(mode);
        QVERIFY(mode->setProperty("currentIndex",1)); QVERIFY(QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,1)));
        QVERIFY(preferences.values().contains(u"Quality"_s)); QCOMPARE(preferences.values()[u"Quality"_s].toString(),QString()); QVERIFY(!preferences.canSave());
        auto *prefQuality=find(prefsPage,u"preference_Quality"_s); QVERIFY(prefQuality); QVERIFY(prefQuality->setProperty("value",91)); QVERIFY(QMetaObject::invokeMethod(prefQuality,"valueModified")); QCOMPARE(preferences.values()[u"Quality"_s].toString(),u"91"_s);
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"specific"_s)); QVERIFY(preferences.setValue(u"MonitorIndex"_s,u"2"_s));
        auto *monitorField=qobject_cast<QQuickItem *>(find(prefsPage,u"preference_field_MonitorIndex"_s)); QVERIFY(monitorField); QTRY_VERIFY(monitorField->isVisible());
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"virtual"_s)); QTRY_VERIFY(!monitorField->isVisible()); QCOMPARE(preferences.values()[u"MonitorIndex"_s].toString(),u"2"_s);
        auto *policyField=qobject_cast<QQuickItem *>(find(prefsPage,u"preference_field_VirtualMonitorPolicy"_s)); QVERIFY(policyField); QTRY_VERIFY(policyField->isVisible());
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"workspace"_s)); QTRY_VERIFY(!policyField->isVisible()); preferences.discard(); QVERIFY(!preferences.modified());
        QVERIFY(goTo(2)); QVERIFY(QMetaObject::invokeMethod(item(u"consoleAddAlias"_s),"clicked"));
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
        for(int index=0;index<4;++index) {
            QQuickItem *shown=goTo(index); QVERIFY(shown); screenshot(u"page-"_s+QString::number(index));
            if (index != 2) {
                auto *current = shown;
                QVERIFY(current->setProperty("showAdvanced",true));
                BrokerHostSettings *captureHost = index == 0 ? &console : index == 1 ? &virtualHost : nullptr; QObject *captureSection = nullptr;
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
        QVERIFY(goTo(2)); aliasDialog=item(u"brokerAliasDialog"_s); QVERIFY(QMetaObject::invokeMethod(item(u"consoleAddAlias"_s),"clicked")); screenshot(u"alias-dialog"_s); QVERIFY(QMetaObject::invokeMethod(aliasDialog,"close"));
        QVERIFY(goTo(0));
        // Turning a route off asks first and does nothing until confirmed.
        auto *enabled=find(page,u"consoleHostEnabled"_s); QVERIFY(enabled); QVERIFY(enabled->setProperty("checked",false)); QVERIFY(QMetaObject::invokeMethod(enabled,"clicked"));
        auto *confirm=find(page,u"consoleConfirmServiceOperation"_s); QVERIFY(confirm); QTRY_VERIFY(confirm->property("visible").toBool()); QCOMPARE(transport.mutations,0); screenshot(u"stop-dialog"_s); QVERIFY(QMetaObject::invokeMethod(confirm,"close")); QCOMPARE(transport.mutations,0);
        const auto resizeTo=[&](const QSize &size){ window.resize(size); QTRY_COMPARE(window.contentItem()->width(),qreal(size.width())); QCoreApplication::processEvents(); navigation.layout(); };
        resizeTo({640,360});
        for(int index=0;index<4;++index) {
            QQuickItem *shown=goTo(index); QVERIFY(shown); QTest::qWait(60);
            QCOMPARE(shown->width(),qreal(640-navigation.columnWidth()));   // beside the sidebar
            for(const auto &old:{u"saveHostSettings"_s,u"saveBrokerPreferences"_s,u"saveBrokerAuthentication"_s,u"saveDesktopHardware"_s}) QVERIFY2(!find(shown,old),qPrintable(old));
        }
        resizeTo({640,800});
        for(int index=0;index<4;++index) { QVERIFY(goTo(index)); screenshot(u"narrow-"_s+QString::number(index)); }
        navigation.goHome(); screenshot(u"narrow-list"_s);
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u'\n')));
    }

    void loadAll(Fixture &f) {
        QVERIFY(f.console.reload()); QTRY_VERIFY(!f.console.busy()); QVERIFY(f.virtualHost.reload()); QTRY_VERIFY(!f.virtualHost.busy());
        QVERIFY(f.session.reload()); QTRY_VERIFY(!f.session.busy()); QVERIFY(f.auth.reload()); QTRY_VERIFY(!f.auth.busy()); QVERIFY(f.preferences.reload());
    }
    static void writeMode(const QTemporaryDir &dir,const QString &name,const QByteArray &value) {
        QFile file(dir.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate)); file.write(value);
    }
    void applyNamesFailedScopesOnEveryPage() {
        Fixture f; QVERIFY(f.init()); loadAll(f);
        QVERIFY(f.console.setValue(u"Port"_s,u"3401"_s)); QVERIFY(f.virtualHost.setValue(u"Port"_s,u"3405"_s)); QVERIFY(f.preferences.setValue(u"Quality"_s,u"70"_s));
        writeMode(f.dir,u"mode-virtual"_s,"stale");
        QVERIFY(!findVisible(f.page,u"applyFailures"_s));
        f.apply.apply(); QTRY_VERIFY(!f.apply.applying());
        QVERIFY(!f.console.modified()); QVERIFY(f.virtualHost.modified()); QVERIFY(!f.preferences.modified());
        for(int index=0;index<4;++index) {           // Console, Virtual, Who Can Connect, My Preferences
            QQuickItem *shown=f.goTo(index); QVERIFY(shown); QTest::qWait(30);
            auto *banner=findVisible(shown,u"applyFailures"_s); QVERIFY2(banner,qPrintable(QString::number(index)));
            const auto text=banner->property("text").toString();
            QVERIFY2(text.contains(u"Virtual:"_s),qPrintable(text)); QVERIFY(!text.contains(u"Console:"_s)); QVERIFY(!text.contains(u"My preferences"_s));
        }
        // A later successful Apply clears it.
        writeMode(f.dir,u"mode-virtual"_s,"success"); QVERIFY(f.virtualHost.reload()); QTRY_VERIFY(!f.virtualHost.busy()); QVERIFY(f.virtualHost.setValue(u"Port"_s,u"3406"_s));
        f.apply.apply(); QTRY_VERIFY(!f.apply.applying()); QVERIFY(f.apply.failures().isEmpty()); QVERIFY(!findVisible(f.goTo(0),u"applyFailures"_s));
    }
    void savedSettingsOfferAnExplicitConfirmedRestart() {
        Fixture f; f.transport.split=true; f.transport.state.activeState=u"active"_s; f.transport.virtualState.activeState=u"active"_s; f.transport.virtualState.unitFileState=u"enabled"_s;
        QVERIFY(f.init()); loadAll(f);
        QQuickItem *consolePage=f.goTo(0); QVERIFY(consolePage);
        auto *notice=qobject_cast<QQuickItem *>(find(consolePage,u"hostSavedNotice"_s)); QVERIFY(notice); QVERIFY(!notice->isVisible());
        QVERIFY(f.console.setValue(u"Port"_s,u"3401"_s)); QVERIFY(f.virtualHost.setValue(u"Port"_s,u"3405"_s));
        f.apply.apply(); QTRY_VERIFY(!f.apply.applying()); QVERIFY(f.apply.failures().isEmpty());
        QTRY_VERIFY(notice->isVisible());
        QCOMPARE(f.transport.mutations,0);                                   // saving never restarts anything
        QObject *restartAction=find(consolePage,u"hostSavedRestart"_s); QVERIFY(restartAction); QTRY_VERIFY(restartAction->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(restartAction,"trigger"));
        auto *dialog=find(f.page,u"consoleConfirmServiceOperation"_s); QVERIFY(dialog); QTRY_VERIFY(dialog->property("visible").toBool());
        QCOMPARE(f.transport.mutations,0);                                   // asks first
        QVERIFY(QMetaObject::invokeMethod(dialog,"reject")); QCOMPARE(f.transport.mutations,0);
        QVERIFY(QMetaObject::invokeMethod(restartAction,"trigger")); QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(f.page,u"consoleConfirmServiceAction"_s),"trigger"));
        QCOMPARE(f.transport.mutations,1); QCOMPARE(f.transport.lastRoute,0); QCOMPARE(f.transport.lastOperation,BrokerServiceTransport::Restart);
        // The Virtual page offers the same for its own route.
        QQuickItem *virtualPage=f.goTo(1); QVERIFY(virtualPage);
        auto *virtualNotice=qobject_cast<QQuickItem *>(find(virtualPage,u"hostSavedNotice"_s)); QVERIFY(virtualNotice); QTRY_VERIFY(virtualNotice->isVisible());
    }
    void serviceControlsConfirmAndFollowActualState() {
        // Switch in the sidebar row, Restart and Start-at-boot on the route page.
        Fixture f; f.transport.split=true; f.transport.defer=true; f.transport.virtualState.mainPid=0;
        QVERIFY(f.init()); loadAll(f);
        const auto text=[&](QObject *scope,const QString &name) { auto *o=find(qobject_cast<QQuickItem *>(scope),name); return o?o->property("text").toString():QString(u"<missing>"_s); };
        QQuickItem *consolePage=f.goTo(0); QVERIFY(consolePage); QQuickItem *virtualPage=nullptr;
        QTRY_COMPARE(text(consolePage,u"consoleHostStatus"_s),u"Running"_s);
        QVERIFY(find(consolePage,u"consoleServiceAutostart"_s)->property("checked").toBool());
        QCOMPARE(f.transport.mutations,0);
        // Turning Console off asks first.
        auto *enabled=find(f.page,u"consoleHostEnabled"_s); QVERIFY(enabled->property("checked").toBool());
        QVERIFY(enabled->setProperty("checked",false)); QVERIFY(QMetaObject::invokeMethod(enabled,"clicked"));
        auto *dialog=find(f.page,u"consoleConfirmServiceOperation"_s); QVERIFY(dialog); QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY(find(f.page,u"consoleHostEnabled"_s)->property("checked").toBool());   // follows the actual state, not the click
        QCOMPARE(f.transport.mutations,0); QVERIFY(QMetaObject::invokeMethod(dialog,"reject")); QCOMPARE(f.transport.mutations,0);
        // Restart from the page asks too, then runs.
        QVERIFY(QMetaObject::invokeMethod(find(consolePage,u"consoleServiceRestart"_s),"clicked")); QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(find(f.page,u"consoleConfirmServiceAction"_s),"trigger"));
        QCOMPARE(f.transport.lastRoute,0); QCOMPARE(f.transport.lastOperation,BrokerServiceTransport::Restart); QVERIFY(f.services.busy());
        QVERIFY(!find(f.page,u"virtualHostEnabled"_s)->property("enabled").toBool());      // no second operation while one runs
        f.transport.finish(u"Administrator authorization cancelled"_s);
        QCOMPARE(find(f.page,u"consoleSubtitle"_s)->property("subtitle").toString().left(7),u"Running"_s);
        QVERIFY(f.services.services()[0].toMap()[u"error"_s].toString().contains(u"cancelled"_s));
        QVERIFY(qobject_cast<QQuickItem *>(find(consolePage,u"consoleHostStatus"_s))->isVisible());
        // Starting Virtual from its sidebar row needs no confirmation.
        QVERIFY(find(f.page,u"virtualHostEnabled"_s)->setProperty("checked",true)); QVERIFY(QMetaObject::invokeMethod(find(f.page,u"virtualHostEnabled"_s),"clicked")); QCOMPARE(f.transport.lastRoute,1); QCOMPARE(f.transport.lastOperation,BrokerServiceTransport::Start);
        f.transport.virtualState.activeState=u"active"_s; f.transport.virtualState.mainPid=123; f.transport.finish();
        virtualPage=f.goTo(1); QVERIFY(virtualPage);
        QTRY_COMPARE(text(virtualPage,u"virtualHostStatus"_s),u"Running"_s);
        // Start at boot: a refused change leaves the switch at the real state.
        QVERIFY(QMetaObject::invokeMethod(find(virtualPage,u"virtualServiceAutostart"_s),"clicked")); QCOMPARE(f.transport.lastOperation,BrokerServiceTransport::Enable);
        f.transport.finish(u"Administrator authorization cancelled"_s);
        QVERIFY(!find(virtualPage,u"virtualServiceAutostart"_s)->property("checked").toBool());
    }
};
QTEST_MAIN(BrokerMainPageTest)
#include "BrokerMainPageTest.moc"
