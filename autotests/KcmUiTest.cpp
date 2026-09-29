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
}

class KcmUiTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;
    std::shared_ptr<QQmlEngine> m_engine;
    KQuickConfigModule *m_module = nullptr;
    QQuickWindow *m_window = nullptr;

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
            auto *item = page->findChild<QQuickItem *>(name);
            QVERIFY2(item, qPrintable(u"missing item "_s + name));
            QVERIFY2(item->isVisible(), qPrintable(name + u" is hidden"_s));
            QVERIFY2(item->width() > 0 && item->height() > 0, qPrintable(name + u" has no size"_s));
            QVERIFY2(content->isAncestorOf(item), qPrintable(name + u" is not in the scrolling content"_s));

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

        // The server's settings for this run: one extra user, a port nothing
        // listens on, so the page shows its ordinary state.
        const QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
        QVERIFY2(config.startsWith(QDir::tempPath()) || config.contains(u"/build"_s), "run through ctest: it sets a throwaway XDG_CONFIG_HOME");
        QDir().mkpath(config);
        QFile rc(config + u"/farsideserverrc"_s);
        QVERIFY(rc.open(QIODevice::WriteOnly | QIODevice::Truncate));
        rc.write("[General]\nUsers=buzz\nSystemUserEnabled=true\nListenPort=1\n");
        rc.close();

        m_engine = std::make_shared<QQmlEngine>();
        const KPluginMetaData metaData(QStringLiteral(KCM_PLUGIN_PATH), KPluginMetaData::AllowEmptyMetaData);
        QVERIFY2(metaData.isValid(), KCM_PLUGIN_PATH);
        const auto result = KQuickConfigModuleLoader::loadModule(metaData, this, {}, m_engine);
        QVERIFY2(result.plugin, qPrintable(result.errorText));
        m_module = result.plugin;
        m_module->load();

        m_window = new QQuickWindow;
        m_window->resize(1280, 800);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanupTestCase()
    {
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
        // (Autostart is hidden here: there is no systemd on this test's bus.)
        QVERIFY(page->findChild<QQuickItem *>(u"autostartCheck"_s));
        checkReachable(page, {u"statusRow"_s, u"connectRow"_s, u"fingerprintRow"_s, u"signInRow"_s, u"screensCombo"_s, u"qualityRow"_s, u"pageButtons"_s});
        if (window.height() < 500) {
            // Too small for everything: the page must scroll (so the checks
            // above really scrolled), not squeeze or stack its parts.
            auto *flickable = flickableOf(page);
            QVERIFY2(flickable->property("contentHeight").toReal() > flickable->height(),
                     qPrintable(u"content %1, view %2, page %3"_s.arg(flickable->property("contentHeight").toReal()).arg(flickable->height()).arg(page->height())));
        }
        // All four ways into the sub-pages.
        for (const auto &button : {u"usersPageButton"_s, u"screensPageButton"_s, u"videoAudioPageButton"_s, u"advancedPageButton"_s}) {
            auto *item = page->findChild<QQuickItem *>(button);
            QVERIFY(item && item->isVisible() && item->width() > 0);
        }
        const auto warnings = takeMessages();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
    }

    void subPagesLoadWithoutWarnings_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("objectName");
        QTest::addColumn<QString>("title");
        QTest::addColumn<QStringList>("keyItems");
        QTest::newRow("users") << u"UsersPage.qml"_s << u"usersPage"_s << u"Users and Security"_s
                               << QStringList{u"systemUserCheck"_s, u"usersFrame"_s, u"certificateStateRow"_s, u"ownCertificateCheck"_s};
        QTest::newRow("screens") << u"ScreensPage.qml"_s << u"screensPage"_s << u"Screens and Displays"_s << QStringList{u"shareColumn"_s, u"wakeCheck"_s};
        QTest::newRow("video and audio") << u"VideoAudioPage.qml"_s << u"videoAudioPage"_s << u"Video and Audio"_s
                                         << QStringList{u"colorDetailCombo"_s, u"busyNetworkColumn"_s, u"standardMediaCheck"_s};
        QTest::newRow("advanced") << u"AdvancedPage.qml"_s << u"advancedPage"_s << u"Advanced"_s
                                  << QStringList{u"listenAddressCombo"_s, u"portField"_s, u"vaapiCombo"_s, u"softwareEncodingCombo"_s, u"av1TilesCombo"_s, u"fallbackSizeField"_s,
                                                 u"cameraCombo"_s, u"developerToggle"_s};
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

    void av1TilesComboFollowsTheSetting()
    {
        // AV1-Q: Advanced > Encoding > "AV1 tiles": Automatic (recommended), 1, 2, 4, 8, 16,
        // bound to krdpserverrc Av1Tiles ("auto" or the count).
        auto *settings = m_module->mainUi()->property("settings").value<QObject *>();
        QVERIFY(settings);
        QCOMPARE(settings->property("av1Tiles").toString(), u"auto"_s);

        m_module->push(u"AdvancedPage.qml"_s);
        auto *page = m_module->subPage(m_module->depth() - 2);
        QVERIFY(page);
        showPage(page, {1280, 800});
        auto *combo = page->findChild<QQuickItem *>(u"av1TilesCombo"_s);
        QVERIFY(combo);
        QCOMPARE(combo->property("count").toInt(), 6);
        QStringList texts;
        QStringList values;
        for (int i = 0; i < 6; ++i) {
            QString text;
            QVariant value;
            QVERIFY(QMetaObject::invokeMethod(combo, "textAt", Q_RETURN_ARG(QString, text), Q_ARG(int, i)));
            QVERIFY(QMetaObject::invokeMethod(combo, "valueAt", Q_RETURN_ARG(QVariant, value), Q_ARG(int, i)));
            texts << text;
            values << value.toString();
        }
        QCOMPARE(texts, QStringList({u"Automatic (recommended)"_s, u"1"_s, u"2"_s, u"4"_s, u"8"_s, u"16"_s}));
        QCOMPARE(values, QStringList({u"auto"_s, u"1"_s, u"2"_s, u"4"_s, u"8"_s, u"16"_s}));
        QCOMPARE(combo->property("currentIndex").toInt(), 0);
        auto *note = page->findChild<QQuickItem *>(u"av1TilesNote"_s);
        QVERIFY(note && note->isVisible());
        QCOMPARE(note->property("text").toString(), u"More tiles let slower computers decode AV1 faster, at a small size cost."_s);

        // The user picks 8: the setting follows.
        combo->setProperty("currentIndex", 4);
        QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 4)));
        QCOMPARE(settings->property("av1Tiles").toString(), u"8"_s);
        // The setting changes (Defaults, a reload): the combo follows.
        settings->setProperty("av1Tiles", u"16"_s);
        QCoreApplication::processEvents();
        QCOMPARE(combo->property("currentIndex").toInt(), 5);
        settings->setProperty("av1Tiles", u"auto"_s);
        QCoreApplication::processEvents();
        QCOMPARE(combo->property("currentIndex").toInt(), 0);
        m_module->pop();
        const auto warnings = takeMessages();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
    }

    void usersPageShowsAPlaceholderWhenEmpty()
    {
        auto *settings = m_module->mainUi()->property("settings").value<QObject *>();
        QVERIFY(settings);
        const auto users = settings->property("users");
        settings->setProperty("users", QStringList());

        m_module->push(u"UsersPage.qml"_s);
        auto *page = m_module->subPage(m_module->depth() - 2);
        QVERIFY(page);
        showPage(page, {1280, 800});
        auto *placeholder = page->findChild<QQuickItem *>(u"usersPlaceholder"_s);
        QVERIFY(placeholder);
        QVERIFY(placeholder->isVisible());
        QCOMPARE(placeholder->property("text").toString(), u"No other users"_s);

        settings->setProperty("systemUserEnabled", false);
        QCoreApplication::processEvents();
        QCOMPARE(placeholder->property("text").toString(), u"No one can sign in yet"_s);
        checkReachable(page, {u"systemUserCheck"_s, u"usersPlaceholder"_s, u"ownCertificateCheck"_s});

        settings->setProperty("users", users);
        settings->setProperty("systemUserEnabled", true);
        QCoreApplication::processEvents();
        QVERIFY(!placeholder->isVisible());
        m_module->pop();
        const auto warnings = takeMessages();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
    }
};

QTEST_MAIN(KcmUiTest)
#include "KcmUiTest.moc"
