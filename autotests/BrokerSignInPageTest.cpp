// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerauthenticationsettings.h"
#include <KLocalizedQmlContext>
#include <QFile>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
using namespace Qt::StringLiterals;
class BrokerSignInPageTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void actualPageLoadsStagesAliasAndHandlesCancel()
    {
        QTemporaryDir directory;
        BrokerAuthenticationSettings administration(u"/usr/bin/python3"_s,
            {qEnvironmentVariable("FARSIDE_AUTH_TEST_FIXTURE", QString::fromUtf8(AUTH_FIXTURE)), directory.path()}, nullptr);
        QQmlEngine engine;
        auto *localized = new KLocalizedQmlContext(&engine);
        localized->setTranslationDomain(u"kcm_farside"_s);
        engine.rootContext()->setContextObject(localized);
        QQmlComponent component(&engine, QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_AUTH_TEST_PAGE", QString::fromUtf8(AUTH_PAGE))));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"administration"_s, QVariant::fromValue(&administration)}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *page = qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
        QQuickWindow window; window.resize(1000, 900); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        const auto visualItem = [](auto &&self, QQuickItem *parent, const QString &name) -> QObject * {
            if (parent->objectName() == name) return parent;
            if (auto *child = parent->findChild<QObject *>(name)) return child;
            for (auto *child : parent->childItems()) if (auto *found = self(self, child, name)) return found;
            return nullptr;
        };
        const auto item = [&](const QString &name) { return visualItem(visualItem, page, name); };
        for(const auto &old:{u"unlockBrokerAuthentication"_s,u"saveBrokerAuthentication"_s,u"discardBrokerAuthentication"_s,u"loadBrokerAuthentication"_s}) QVERIFY2(!item(old),qPrintable(old)); // S3: the standard bar does this
         QVERIFY(!administration.loaded());
        auto *expand = item(u"expandWhoCanSignIn"_s); QVERIFY(expand); QVERIFY(QMetaObject::invokeMethod(expand, "clicked"));
        QTRY_VERIFY(administration.loaded() && !administration.busy());
        QTRY_VERIFY(item(u"consolePamMode"_s)); QTRY_VERIFY(item(u"virtualPamMode"_s));
        auto *add = item(u"consoleAddAlias"_s); QVERIFY(add); QVERIFY(QMetaObject::invokeMethod(add, "clicked"));
        auto *dialog = item(u"brokerAliasDialog"_s); QVERIFY(dialog); QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(item(u"brokerAliasName"_s)->setProperty("text", u"guest"_s));
        QVERIFY(item(u"brokerAliasOwner"_s)->setProperty("text", u"westers"_s));
        QVERIFY(item(u"brokerAliasPassword"_s)->setProperty("text", u"fixture-only-password"_s));
        QCOMPARE(item(u"brokerAliasPassword"_s)->property("echoMode").toInt(), 2); // TextInput.Password
        QVERIFY(QMetaObject::invokeMethod(item(u"stageBrokerAlias"_s), "triggered"));
        QTRY_VERIFY(!dialog->property("visible").toBool()); QTRY_COMPARE(item(u"brokerAliasPassword"_s)->property("text").toString(), QString());
        QVERIFY(administration.modified()); QVERIFY(!QJsonDocument::fromVariant(administration.policy()).toJson().contains("password"));
        QFile mode(directory.filePath(u"mode"_s)); QVERIFY(mode.open(QIODevice::WriteOnly)); mode.write("cancel"); mode.close();
        QVERIFY(administration.save());
        QTRY_VERIFY(!administration.busy()); QVERIFY(administration.modified()); QVERIFY(administration.error().contains(u"unchanged"_s));
        QVERIFY(mode.open(QIODevice::WriteOnly | QIODevice::Truncate)); mode.write("success"); mode.close();
        QVERIFY(administration.save());
        QTRY_VERIFY(!administration.busy()); QVERIFY2(administration.error().isEmpty(), qPrintable(administration.error()));
        QVERIFY(!administration.modified()); QVERIFY(administration.lastSaveRequiresRestart());
        QTest::qWait(400);
        const auto screenshot = qEnvironmentVariable("FARSIDE_AUTH_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(window.grabWindow().save(screenshot));
        page->setParentItem(nullptr);
    }
};
QTEST_MAIN(BrokerSignInPageTest)
#include "BrokerSignInPageTest.moc"
