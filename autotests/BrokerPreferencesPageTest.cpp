// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerpreferences.h"
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
using namespace KRdp;
class BrokerPreferencesPageTest : public QObject {
    Q_OBJECT
    static QObject *find(QQuickItem *parent,const QString &name) {
        if(parent->objectName()==name) return parent;
        if(auto *object=parent->findChild<QObject *>(name)) return object;
        for(auto *child:parent->childItems()) if(auto *object=find(child,name)) return object;
        return nullptr;
    }
    static void write(const QString &path,const QByteArray &bytes) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate)); QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)); QCOMPARE(file.write(bytes),bytes.size()); }
    static QByteArray read(const QString &path) { QFile file(path); if(!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
private Q_SLOTS:
    void actualBindingsSaveReloadAndDefaultInheritance() {
        QTemporaryDir dir; const auto path=dir.filePath(u"farsideserverrc"_s);
        write(path,"[General]\nQuality=42\nAv1Tiles[$i]=8\nCertificate=/root/fixture.pem\nPassword=fixture-secret-only\n");
        BrokerPreferences preferences(dir.path()); QQmlEngine engine; QStringList warnings;
        connect(&engine,&QQmlEngine::warnings,this,[&](const auto &errors) { for(const auto &error:errors) warnings.append(error.toString()); });
        auto *localized=new KLocalizedQmlContext(&engine); localized->setTranslationDomain(u"kcm_farside"_s); engine.rootContext()->setContextObject(localized);
        QQmlComponent component(&engine,QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_PREFERENCES_TEST_PAGE",QString::fromUtf8(PREFERENCES_PAGE))));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"showAdvanced"_s,true},{u"preferences"_s,QVariant::fromValue(&preferences)}}));
        QVERIFY2(object,qPrintable(component.errorString())); auto *page=qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
        QQuickWindow window; window.resize(1000,900); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        const auto item=[&](const QString &name) { return find(page,name); };
        QVERIFY(!preferences.loaded()); QVERIFY(preferences.reload()); QVERIFY(preferences.loaded());
        for(const auto &old:{u"saveBrokerPreferences"_s,u"discardBrokerPreferences"_s,u"defaultBrokerPreferences"_s,u"loadBrokerPreferences"_s}) QVERIFY2(!item(old),qPrintable(old)); // S3: the standard bar does this
        QTRY_VERIFY(item(u"preference_Quality"_s)); auto *quality=qobject_cast<QQuickItem *>(item(u"preference_Quality"_s)); QVERIFY(quality);
        quality->forceActiveFocus(); QTest::keyClick(&window,Qt::Key_A,Qt::ControlModifier); QTest::keyClick(&window,Qt::Key_9); QTest::keyClick(&window,Qt::Key_5); QTest::keyClick(&window,Qt::Key_Return);
        QTRY_COMPARE(preferences.values()[u"Quality"_s].toString(),u"95"_s);
        QVERIFY(!item(u"preference_Av1Tiles"_s)->property("enabled").toBool());
        // With the mode inherited the display section holds only that one choice, so the page says when more appear.
        QVERIFY(qobject_cast<QQuickItem *>(item(u"displayModeHint"_s))->isVisible());
        const QVariantMap desired{{u"Quality"_s,u"91"_s},{u"AdaptiveQuality"_s,u"true"_s},{u"PreferAudioQuality"_s,u"true"_s},{u"Codec"_s,u"avc444"_s},
            {u"SoftwareEncoding"_s,u"prefer"_s},{u"Av1Tiles"_s,u"8"_s},{u"Avc444MotionGapMs"_s,u"50"_s},{u"Avc444RestMs"_s,u"75"_s},{u"Avc444MaxGapMs"_s,u"1000"_s},
            {u"MonitorMode"_s,u"specific"_s},{u"MonitorIndex"_s,u"2"_s},{u"VirtualMonitorPolicy"_s,u"extend"_s},{u"VirtualMonitorLayout"_s,u"physical"_s},
            {u"VirtualMonitorFallbackSize"_s,u"2560x1440"_s},{u"WakeDisplayOnConnect"_s,u"false"_s},{u"StandardClientMedia"_s,u"false"_s},{u"VirtualStockClientPolicy"_s,u"refuse"_s}};
        for(const auto &definition:preferences.definitions()) {
            const auto row=definition.toMap(); const auto key=row[u"key"_s].toString(); auto *control=item(key==u"VirtualMonitorFallbackSize" ? u"inherit_"_s+key : u"preference_"_s+key); QVERIFY2(control,qPrintable(key));
            if(preferences.lockedKeys().contains(key)) continue;
            const auto choices=row[u"choices"_s].toList();
            if (key == u"VirtualMonitorFallbackSize") {
                QVERIFY(item(u"inherit_"_s+key)->setProperty("currentIndex",1));
                QVERIFY(QMetaObject::invokeMethod(item(u"inherit_"_s+key),"activated",Q_ARG(int,1)));
                for (const auto &dimension : {QString(u"Width"_s), QString(u"Height"_s)}) {
                    auto *spin=item(u"preference_Fallback"_s+dimension); QVERIFY(spin);
                    QVERIFY(spin->setProperty("value",dimension==u"Width" ? 2560 : 1440));
                    QVERIFY(QMetaObject::invokeMethod(spin,"valueModified"));
                }
            } else if(control->metaObject()->indexOfProperty("from")>=0) {
                auto *mode=item(u"inherit_"_s+key); QVERIFY(mode);
                QVERIFY(mode->setProperty("currentIndex",1)); QVERIFY(QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,1)));
                QVERIFY(control->setProperty("value",desired[key].toInt()));
                QVERIFY(QMetaObject::invokeMethod(control,"valueModified"));
            } else {
                int index=-1; for(int i=0;i<choices.size();++i) if(choices[i].toMap()[u"value"_s]==desired[key]) index=i;
                QVERIFY(index>=0); QVERIFY(control->setProperty("currentIndex",index)); QVERIFY(QMetaObject::invokeMethod(control,"activated",Q_ARG(int,index)));
            }
            QCOMPARE(preferences.values()[key],desired[key]);
        }
        QCOMPARE(preferences.values(),desired);
        QTRY_VERIFY(!qobject_cast<QQuickItem *>(item(u"displayModeHint"_s))->isVisible());   // "One display" is chosen: its own options are showing
        QCOMPARE(quality->property("to").toInt(),100); QVERIFY(preferences.setValue(u"Quality"_s,u"101"_s)); QVERIFY(!preferences.canSave());
        QVERIFY(preferences.error().contains(u"Quality"_s)); QVERIFY(quality->setProperty("value",91)); QVERIFY(QMetaObject::invokeMethod(quality,"valueModified"));
        QVERIFY(preferences.save()); QVERIFY(!preferences.modified()); QVERIFY(preferences.reconnectRequired());
        QCOMPARE(BrokerUserSettings::parse(read(path)).preferences.quality,std::optional<quint8>(91)); QVERIFY(read(path).contains("Password=fixture-secret-only\n"));
        auto *flickable=page->property("flickable").value<QQuickItem *>(); QVERIFY(flickable); QTest::qWait(400);
        const auto screenshots=qEnvironmentVariable("FARSIDE_PREFERENCES_SCREENSHOTS");
        if(!screenshots.isEmpty()) {
            const auto maximum=qMax<qreal>(0,flickable->property("contentHeight").toReal()-flickable->height());
            const auto step=qMax<qreal>(1,flickable->height()*0.8); int shot=0;
            for(qreal y=0;;y=qMin(maximum,y+step)) {
                flickable->setProperty("contentY",y); QTest::qWait(200);
                QVERIFY(window.grabWindow().save(screenshots+u"/preferences-"_s+QString::number(shot++)+u".png"_s));
                if(y>=maximum) break;
            }
        }
        window.resize(640,700); page->setSize(window.size()); QTest::qWait(200);
        auto *content=flickable->property("contentItem").value<QQuickItem *>(); QVERIFY(content);
        for(const auto &key:BrokerUserSettings::preferenceKeys()) {
            auto *control=qobject_cast<QQuickItem *>(item(key==u"VirtualMonitorFallbackSize" ? u"inherit_"_s+key : u"preference_"_s+key)); QVERIFY(control);
            const auto bounds=control->mapRectToItem(content,QRectF(0,0,control->width(),control->height()));
            QVERIFY2(bounds.left()>=-0.5 && bounds.right()<=flickable->width()+0.5,qPrintable(key+u" needs horizontal scrolling at width640"_s));
        }
        preferences.defaults(); QCOMPARE(preferences.values(),QVariantMap({{u"Av1Tiles"_s,u"8"_s}}));
        QTRY_VERIFY(item(u"inherit_Quality"_s)->property("currentIndex").toInt()==0); QVERIFY(preferences.save());
        QVERIFY(!BrokerUserSettings::parse(read(path)).preferences.quality); QVERIFY(read(path).contains("Certificate=/root/fixture.pem\n"));
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u"\n"_s))); page->setParentItem(nullptr);
    }
    void choosingCustomShowsNoErrorForAnyInheritField() {
        QTemporaryDir dir; write(dir.filePath(u"farsideserverrc"_s),"[General]\nHost=original\n");
        BrokerPreferences preferences(dir.path()); QQmlEngine engine;
        auto *localized=new KLocalizedQmlContext(&engine); localized->setTranslationDomain(u"kcm_farside"_s); engine.rootContext()->setContextObject(localized);
        QQmlComponent component(&engine,QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_PREFERENCES_TEST_PAGE",QString::fromUtf8(PREFERENCES_PAGE))));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"showAdvanced"_s,true},{u"preferences"_s,QVariant::fromValue(&preferences)}}));
        QVERIFY2(object,qPrintable(component.errorString())); auto *page=qobject_cast<QQuickItem *>(object.data());
        QQuickWindow window; window.resize(1000,900); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        const auto item=[&](const QString &name) { return find(page,name); };
        QVERIFY(preferences.reload()); QTRY_VERIFY(item(u"inherit_Quality"_s));
        // MonitorIndex only shows for the "One display" mode.
        QVERIFY(preferences.setValue(u"MonitorMode"_s,u"specific"_s)); preferences.discard(); QVERIFY(preferences.setValue(u"MonitorMode"_s,u"specific"_s));
        int visited=0;
        for(const auto &definition:preferences.definitions()) {
            const auto row=definition.toMap(); const auto key=row[u"key"_s].toString(); const auto control=row[u"control"_s].toString();
            if(control!=u"spin" && control!=u"slider" && control!=u"size") continue;
            QTRY_VERIFY2(item(u"inherit_"_s+key),qPrintable(key)); ++visited;
            auto *mode=item(u"inherit_"_s+key); QVERIFY(mode->setProperty("currentIndex",1)); QVERIFY(QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,1)));
            QCOMPARE(preferences.values()[key].toString(),row[u"customSeed"_s].toString());
            QVERIFY2(preferences.error().isEmpty(),qPrintable(key+u": "_s+preferences.error()));
            auto *error=qobject_cast<QQuickItem *>(item(u"brokerPreferenceError"_s)); QVERIFY(error); QVERIFY2(!error->isVisible(),qPrintable(key));
            if(control!=u"size") { auto *spin=item(u"preference_"_s+key); QVERIFY(spin); QTRY_VERIFY2(!spin->property("contentItem").value<QQuickItem *>()->property("text").toString().isEmpty(),qPrintable(key)); }
        }
        QCOMPARE(visited,6); QVERIFY(preferences.canSave());
        // Back to the host setting removes the value again.
        QVERIFY(QMetaObject::invokeMethod(item(u"inherit_Quality"_s),"activated",Q_ARG(int,0))); QVERIFY(!preferences.values().contains(u"Quality"_s));
        page->setParentItem(nullptr);
    }
    void staleSaveAndDiscardCancellationKeepPendingValues() {
        QTemporaryDir dir; const auto path=dir.filePath(u"farsideserverrc"_s); write(path,"[General]\nQuality=42\n");
        BrokerPreferences preferences(dir.path()); QQmlEngine engine;
        auto *localized=new KLocalizedQmlContext(&engine); localized->setTranslationDomain(u"kcm_farside"_s); engine.rootContext()->setContextObject(localized);
        QQmlComponent component(&engine,QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_PREFERENCES_TEST_PAGE",QString::fromUtf8(PREFERENCES_PAGE))));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"showAdvanced"_s,true},{u"preferences"_s,QVariant::fromValue(&preferences)}}));
        QVERIFY2(object,qPrintable(component.errorString())); auto *page=qobject_cast<QQuickItem *>(object.data()); QQuickWindow window;
        window.resize(1000,900); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        const auto item=[&](const QString &name) { return find(page,name); };
        QVERIFY(preferences.reload()); QTRY_VERIFY(item(u"preference_Quality"_s));
        QVERIFY(preferences.setValue(u"Quality"_s,u"88"_s)); const QByteArray changed("[General]\nQuality=51\n# external\n"); write(path,changed);
        QVERIFY(!preferences.save()); QCOMPARE(read(path),changed); QVERIFY(preferences.error().contains(u"changed"_s));
        // Reset (the standard bar) discards the pending edit and re-reads the file the other writer changed.
        preferences.discard(); QVERIFY(preferences.reload());
        QCOMPARE(preferences.values()[u"Quality"_s].toString(),u"51"_s); QVERIFY(!preferences.modified()); page->setParentItem(nullptr);
    }
};
QTEST_MAIN(BrokerPreferencesPageTest)
#include "BrokerPreferencesPageTest.moc"
