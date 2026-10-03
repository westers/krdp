// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// FARSIDE-KCM: loads the built settings page (the real plugin and QML) the way
// System Settings does, then checks that
// - the main page and each of its four sub-pages load without a warning;
// - on a 1280×800 screen (and a smaller window) every key item of the main
//   page can be scrolled into view, needs no sideways scrolling, and overlaps
//   no other key item. The page used to paint its settings over the address
//   list and squeeze the user list to nothing (evidence/2026-09-28-kcm-before).
//
// Runs offscreen with a throwaway config and no session bus (see CMakeLists).

#include <KPluginMetaData>
#include <KQuickConfigModule>
#include <KQuickConfigModuleLoader>

#include <QApplication>
#include <QCryptographicHash>
#include <QQmlContext>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

namespace
{
QMutex messagesMutex;
QStringList messages;
QtMessageHandler previousHandler = nullptr;

// Messages from the environment, not from the page.
bool benign(const QString &message)
{
    static const QStringList known{
        u"org.freedesktop.portal"_s,
        u"libva"_s,
        u"software backend"_s,
        u"createPlatformOpenGLContext"_s,
        u"Could not connect to the session bus"_s,
        u"D-Bus"_s,
        u"DBus"_s,
        u"org.freedesktop.systemd1"_s,
        u"Cannot read the state of"_s,
        u"Cannot read the unit file state"_s,
        // KPipeWire probing the GPU for the H.264 check.
        u"kpipewire"_s,
    };
    return std::any_of(known.cbegin(), known.cend(), [&](const QString &k) {
        return message.contains(k, Qt::CaseInsensitive);
    });
}

void collect(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type != QtDebugMsg && type != QtInfoMsg) {
        const QString full = QString::fromLatin1(context.category ? context.category : "default") + u": "_s + message;
        if (!benign(full)) {
            QMutexLocker lock(&messagesMutex);
            messages << full;
        }
    }
    previousHandler(type, context, message);
}

QStringList takeMessages()
{
    QMutexLocker lock(&messagesMutex);
    return std::exchange(messages, {});
}

QQuickItem *flickableOf(QQuickItem *page)
{
    return page->property("flickable").value<QQuickItem *>();
}
QQuickItem *findItem(QQuickItem *parent, const QString &name)
{
    if (parent->objectName() == name) return parent;
    if (auto *item = parent->findChild<QQuickItem *>(name)) return item;
    for (auto *child : parent->childItems()) if (auto *item = findItem(child, name)) return item;
    return nullptr;
}
}

class KcmUiTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;
    std::shared_ptr<QQmlEngine> m_engine;
    KQuickConfigModule *m_module = nullptr;
    QQuickWindow *m_window = nullptr;
    QMap<QString, QByteArray> m_preserved;
    QByteArray readFixture(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }

    // What kcmshell/System Settings leave for the page: the window minus the
    // title bar above it and the Help/Defaults/Apply bar below it.
    QSizeF pageSize(const QSize &window) const
    {
        const qreal chrome = 3 * 2 * 18.0 + 12; // generous: ~2 grid units each, plus separators
        return QSizeF(window.width(), window.height() - chrome);
    }

    QQuickItem *showPage(QQuickItem *page, const QSize &window)
    {
        m_window->resize(window);
        page->setParentItem(m_window->contentItem());
        page->setPosition({0, 0});
        page->setSize(pageSize(window));
        page->setVisible(true);
        // Let layouts and bindings settle.
        for (int i = 0; i < 10; ++i) {
            QCoreApplication::processEvents();
            QTest::qWait(20);
        }
        return page;
    }

    void checkReachable(QQuickItem *page, const QStringList &names)
    {
        auto *flickable = flickableOf(page);
        QVERIFY2(flickable, "the page is not a scrollable page");
        auto *content = flickable->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        const qreal contentHeight = flickable->property("contentHeight").toReal();
        const qreal viewHeight = flickable->height();
        QVERIFY(viewHeight > 0);

        // The fixed header (messages) sits above the scrolling area, not over it.
        const QRectF viewInPage = flickable->mapRectToItem(page, QRectF(0, 0, flickable->width(), viewHeight));
        QVERIFY2(viewInPage.bottom() <= page->height() + 0.5, "the scrolling area runs past the page");

        QList<std::pair<QString, QRectF>> rects;
        for (const auto &name : names) {
            auto *item = findItem(page, name);
            QVERIFY2(item, qPrintable(u"missing item "_s + name));
            QVERIFY2(item->isVisible(), qPrintable(name + u" is hidden"_s));
            QVERIFY2(item->width() > 0 && item->height() > 0, qPrintable(name + u" has no size"_s));
            if (!content->isAncestorOf(item)) {
                const auto bounds=item->mapRectToItem(page,QRectF(0,0,item->width(),item->height()));
                QVERIFY2(bounds.left()>=-0.5 && bounds.right()<=page->width()+0.5 && bounds.bottom()<=page->height()+0.5,qPrintable(name));
                continue;
            }

            const QRectF r = item->mapRectToItem(content, QRectF(0, 0, item->width(), item->height()));
            QVERIFY2(r.top() >= -0.5 && r.bottom() <= contentHeight + 0.5,
                     qPrintable(u"%1 lies outside the scrollable content (%2..%3 of %4)"_s.arg(name).arg(r.top()).arg(r.bottom()).arg(contentHeight)));
            QVERIFY2(r.left() >= -0.5 && r.right() <= flickable->width() + 0.5,
                     qPrintable(u"%1 needs sideways scrolling (%2..%3 of %4)"_s.arg(name).arg(r.left()).arg(r.right()).arg(flickable->width())));

            // Scroll it into view, as a user would, and look again.
            const qreal maxY = std::max<qreal>(0, contentHeight - viewHeight);
            flickable->setProperty("contentY", std::clamp<qreal>(r.top(), 0, maxY));
            QCoreApplication::processEvents();
            const QRectF inView = item->mapRectToItem(flickable, QRectF(0, 0, item->width(), std::min<qreal>(item->height(), viewHeight)));
            QVERIFY2(inView.top() >= -0.5 && inView.bottom() <= viewHeight + 0.5,
                     qPrintable(u"%1 cannot be scrolled into view (%2..%3 of %4)"_s.arg(name).arg(inView.top()).arg(inView.bottom()).arg(viewHeight)));
            rects.append({name, r});
        }
        flickable->setProperty("contentY", 0);

        for (qsizetype i = 0; i < rects.size(); ++i) {
            for (qsizetype j = i + 1; j < rects.size(); ++j) {
                const QRectF overlap = rects.at(i).second.intersected(rects.at(j).second);
                QVERIFY2(overlap.width() < 1 || overlap.height() < 1,
                         qPrintable(u"%1 overlaps %2"_s.arg(rects.at(i).first, rects.at(j).first)));
            }
        }
    }

private Q_SLOTS:
    void initTestCase()
    {
        previousHandler = qInstallMessageHandler(collect);

        // Legacy settings/TLS/secret fixtures must remain byte-identical even
        // when the new module opens or its inherited global actions are called.
        const QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
        QVERIFY2(config.startsWith(QDir::tempPath()) || config.contains(u"/build"_s), "run through ctest: it sets a throwaway XDG_CONFIG_HOME");
        QDir().mkpath(config);
        for (const auto &name : {u"farsideserverrc"_s, u"krdpserverrc"_s, u"legacy.crt"_s, u"legacy.key"_s, u"legacy-wallet.fixture"_s}) {
            const auto path=config+u"/"_s+name;QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
            const QByteArray bytes="[General]\nUsers=legacy-fixture\nSystemUserEnabled=true\nListenPort=1\nUnrelatedFixture=preserve\n";
            QCOMPARE(file.write(bytes),bytes.size());file.close();m_preserved[path]=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
        }

        m_engine = std::make_shared<QQmlEngine>();
        const KPluginMetaData metaData(qEnvironmentVariable("FARSIDE_KCM_TEST_PLUGIN_PATH", QStringLiteral(KCM_PLUGIN_PATH)), KPluginMetaData::AllowEmptyMetaData);
        QVERIFY2(metaData.isValid(), KCM_PLUGIN_PATH);
        const auto result = KQuickConfigModuleLoader::loadModule(metaData, this, {}, m_engine);
        QVERIFY2(result.plugin, qPrintable(result.errorText));
        m_module = result.plugin;
        m_module->load();
        QVERIFY2(m_module->mainUi(), "main.qml did not load; stop before inspecting its controls");

        m_window = new QQuickWindow;
        m_window->resize(1280, 800);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanupTestCase()
    {
        for(auto it=m_preserved.begin();it!=m_preserved.end();++it)
            QCOMPARE(QCryptographicHash::hash(readFixture(it.key()),QCryptographicHash::Sha256),it.value());
        delete m_window;
        qInstallMessageHandler(previousHandler);
    }

    void mainPageLoadsWithoutWarnings()
    {
        auto *page = m_module->mainUi();
        QVERIFY2(page, "main.qml did not load");
        QCOMPARE(page->objectName(), u"mainPage"_s);
        QCOMPARE(page->property("title").toString(), u"Farside Remote Desktop"_s);
        showPage(page, {1280, 800});
        const auto warnings = takeMessages();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
    }

    void mainPageItemsReachable_data()
    {
        QTest::addColumn<QSize>("window");
        QTest::newRow("laptop 1280x800") << QSize(1280, 800);
        QTest::newRow("full HD 1920x1080") << QSize(1920, 1080);
        QTest::newRow("small window 640x360") << QSize(640, 360);
    }

    void mainPageItemsReachable()
    {
        QFETCH(QSize, window);
        auto *page = showPage(m_module->mainUi(), window);
        page->setProperty("currentPage", 0);
        checkReachable(page, {u"consoleHostEnabled"_s, u"consoleHostStatus"_s, u"configureConsole"_s, u"configureVirtual"_s, u"configureAccess"_s, u"configurePreferences"_s});
        for (int tab = 1; tab <= 4; ++tab) {
            QVERIFY(page->setProperty("currentPage", tab));
            QTest::qWait(100); // Qt Quick polishes persistent page layouts after a resize.
            auto *back = findItem(page,u"settingsBack"_s); QVERIFY(back && back->isVisible());
            const auto buttonName = tab <= 2 ? u"saveHostSettings"_s : tab == 3 ? u"saveBrokerAuthentication"_s : u"saveBrokerPreferences"_s;
            // Find the visible footer; another tab's editor remains alive.
            const auto visibleFind = [&](auto &&self, QQuickItem *parent) -> QQuickItem * {
                if (parent->objectName() == buttonName && parent->isVisible()) return parent;
                for (auto *child : parent->childItems()) if (auto *item = self(self, child)) return item;
                return nullptr;
            };
            auto *save = visibleFind(visibleFind,page); QVERIFY(save && save->width()>0 && save->height()>0);
            const auto bounds=save->mapRectToItem(page,QRectF(0,0,save->width(),save->height()));
            QVERIFY2(bounds.left()>=-0.5 && bounds.right()<=page->width()+0.5 && bounds.bottom()<=page->height()+0.5,qPrintable(u"tab %1 footer %2 bounds (%3,%4)..(%5,%6) page %7x%8"_s.arg(tab).arg(buttonName).arg(bounds.left()).arg(bounds.top()).arg(bounds.right()).arg(bounds.bottom()).arg(page->width()).arg(page->height())));
        }
        page->setProperty("currentPage", 0);
        const auto warnings = takeMessages();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
    }

    void subPagesLoadWithoutWarnings_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("objectName");
        QTest::addColumn<QString>("title");
        QTest::addColumn<QStringList>("keyItems");
        QTest::newRow("broker sign-in") << u"BrokerSignInPage.qml"_s << u"brokerSignInPage"_s << u"Who Can Connect"_s
                                       << QStringList{u"loadBrokerAuthentication"_s, u"saveBrokerAuthentication"_s};
        QTest::newRow("broker services") << u"BrokerServicesPage.qml"_s << u"brokerServicesPage"_s << u"Console and Virtual Services"_s
                                       << QStringList{u"refreshBrokerServices"_s};
        QTest::newRow("broker preferences") << u"BrokerPreferencesPage.qml"_s << u"brokerPreferencesPage"_s << u"My Preferences"_s
                                          << QStringList{u"loadBrokerPreferences"_s,u"saveBrokerPreferences"_s,u"defaultBrokerPreferences"_s};
        QTest::newRow("broker hosts") << u"BrokerHostsPage.qml"_s << u"brokerHostsPage"_s << u"Console Settings"_s
                                    << QStringList{u"unlockHostSettings"_s, u"loadHostSettings"_s, u"saveHostSettings"_s, u"defaultHostSettings"_s};

    }

    void subPagesLoadWithoutWarnings()
    {
        QFETCH(QString, file);
        QFETCH(QString, objectName);
        QFETCH(QString, title);
        QFETCH(QStringList, keyItems);

        takeMessages();
        const int depth = m_module->depth();
        m_module->push(file);
        QCOMPARE(m_module->depth(), depth + 1);
        auto *page = m_module->subPage(m_module->depth() - 2);
        QVERIFY2(page, qPrintable(file + u" did not load"_s));
        QCOMPARE(page->objectName(), objectName);
        QCOMPARE(page->property("title").toString(), title);
        showPage(page, {1280, 800});
        checkReachable(page, keyItems);
        const auto warnings = takeMessages();
        m_module->pop();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
    }

    void moduleOpeningAndGlobalActionsPreserveLegacyAndKeepModelsLazy() {
        for(const auto &name:{"settings()","toggleServer(bool)","restartServer()","applyListenPort(int)","addUser(QString,QString)","readPasswordFromWallet(QString)"})
            QVERIFY(m_module->metaObject()->indexOfMethod(name)<0);
        QVERIFY(m_module->metaObject()->indexOfProperty("coexistence")<0);
        QCOMPARE(int(m_module->buttons()),int(KAbstractConfigModule::Help));
        for(const auto &name:{"consoleHostSettings","virtualHostSettings","virtualSessionSettings","brokerAuthentication","brokerPreferences"}) {
            auto *model=m_module->property(name).value<QObject *>();QVERIFY(model);QVERIFY(!model->property("loaded").toBool());
        }
        m_module->defaults();m_module->save();m_module->load();
        QVERIFY(m_module->findChildren<QProcess *>().isEmpty());
        for(const auto &name:{"consoleHostSettings","virtualHostSettings","virtualSessionSettings","brokerAuthentication","brokerPreferences"}) {
            auto *model=m_module->property(name).value<QObject *>();QVERIFY(model);QVERIFY(!model->property("loaded").toBool());
        }
        for(auto it=m_preserved.begin();it!=m_preserved.end();++it)
            QCOMPARE(QCryptographicHash::hash(readFixture(it.key()),QCryptographicHash::Sha256),it.value());
        for(const auto &file:{u"UsersPage.qml"_s,u"ScreensPage.qml"_s,u"VideoAudioPage.qml"_s,u"AdvancedPage.qml"_s,u"EditUserModal.qml"_s})
            QVERIFY(!QFile::exists(u":/kcm/kcm_farside/"_s+file));
    }
    void hostNavigationSelectsScope_data() {
        QTest::addColumn<QString>("route");QTest::addColumn<int>("index");
        QTest::newRow("Console")<<u"console"_s<<0;QTest::newRow("Virtual")<<u"virtual"_s<<1;
    }
    void hostNavigationSelectsScope() {
        QFETCH(QString,route); QFETCH(int,index); auto *main=m_module->mainUi();
        QVERIFY(main->setProperty("currentPage",index+1));
        auto *page=findItem(main,index==0?u"consoleSettingsPage"_s:u"virtualSettingsPage"_s); QVERIFY(page);
        QCOMPARE(page->property("fixedScope").toInt(),index);
        auto *host=page->property("host").value<QObject *>(); QVERIFY(host); QCOMPARE(host->property("scope").toString(),route);
        QVERIFY(!host->property("loaded").toBool()); main->setProperty("currentPage",1);
        const auto warnings=takeMessages(); QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u'\n')));
    }
    void phoneEntryUsesTheSameScopedPage() {
        QQmlComponent component(m_engine.get(),QUrl(u"qrc:/kcm/kcm_farside/main_phone.qml"_s));QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create(QQmlEngine::contextForObject(m_module->mainUi())));QVERIFY2(object,qPrintable(component.errorString()));
        auto *page=qobject_cast<QQuickItem *>(object.data());QVERIFY(page);QCOMPARE(page->objectName(),u"mainPage"_s);
        showPage(page,{640,800});checkReachable(page,{u"consoleHostStatus"_s,u"consoleHostEnabled"_s,u"consoleDetailsToggle"_s});
        QVERIFY(!page->findChild<QObject *>(u"serverSwitch"_s));page->setParentItem(nullptr);
        const auto warnings=takeMessages();QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u'\n')));
    }

};

QTEST_MAIN(KcmUiTest)
#include "KcmUiTest.moc"
