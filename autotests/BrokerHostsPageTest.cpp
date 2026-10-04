// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerhostsettings.h"
#include "ServerCertificate.h"
#include "HostSnapshotFixture.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QScopeGuard>
#include <KLocalizedQmlContext>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <functional>
using namespace Qt::StringLiterals;
class HostPageNavigation : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE void goBack() {}
};
class BrokerHostsPageTest : public QObject {
    Q_OBJECT
    using Scope = BrokerHostSettings::Scope;
    static QObject *find(QQuickItem *parent, const QString &name) {
        if (parent->objectName() == name) return parent;
        if (auto *object = parent->findChild<QObject *>(name)) return object;
        for (auto *child : parent->childItems()) if (auto *object = find(child, name)) return object;
        return nullptr;
    }
    static void writeMode(const QTemporaryDir &dir, const QByteArray &bytes) {
        QFile file(dir.filePath(u"mode"_s)); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(bytes), bytes.size());
    }
    static QStringList arguments(const QTemporaryDir &dir) { return {qEnvironmentVariable("FARSIDE_HOST_TEST_FIXTURE", QString::fromUtf8(HOST_PROTOCOL_FIXTURE)), dir.path()}; }
    static QUrl pageUrl() { return QUrl::fromLocalFile(qEnvironmentVariable("FARSIDE_HOST_TEST_PAGE", QString::fromUtf8(HOST_PAGE))); }
    static void localize(QQmlEngine &engine) {
        auto *context = new KLocalizedQmlContext(&engine); context->setTranslationDomain(u"kcm_farside"_s); engine.rootContext()->setContextObject(context);
    }
    static bool hasUnlockObject(QObject *object) {
        if (object->objectName().startsWith(u"unlock")) return true;
        for (auto *child : object->children()) if (hasUnlockObject(child)) return true;
        if (auto *item = qobject_cast<QQuickItem *>(object)) for (auto *child : item->childItems()) if (hasUnlockObject(child)) return true;
        return false;
    }
private Q_SLOTS:
    void populatedOnOpenWithoutClicks() {
        // S1: the public-metadata snapshot alone populates every host page, with
        // no helper, no click and no Load/unlock button.
        QTemporaryDir snapshots, helper;
        qputenv("FARSIDE_PUBLIC_SETTINGS_DIR", snapshots.path().toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("FARSIDE_PUBLIC_SETTINGS_DIR"); });
        QVERIFY(HostSnapshotFixture::publishAll(snapshots.path(), "FARSIDE_CONSOLE_PORT=4321\nFARSIDE_VIRTUAL_PORT=4322\nFARSIDE_CONSOLE_QUALITY=71\n"));
        // A helper that cannot run proves nothing was spawned.
        const QStringList unusable{u"/nonexistent"_s};
        BrokerHostSettings console(Scope::Console,u"/nonexistent/helper"_s,unusable,3000),
            virtualHost(Scope::Virtual,u"/nonexistent/helper"_s,unusable,3000),
            session(Scope::VirtualSession,u"/nonexistent/helper"_s,unusable,3000);
        for (auto *model : {&console,&virtualHost,&session}) { QVERIFY(model->refresh()); QVERIFY(model->loaded()); QVERIFY(!model->busy()); QVERIFY(model->error().isEmpty()); }
        QCOMPARE(console.values()[u"Port"_s].toString(), u"4321"_s);
        int scope=0;
        for (auto *model : {&console,&virtualHost}) {
            QQmlEngine engine; localize(engine); QStringList warnings;
            connect(&engine,&QQmlEngine::warnings,this,[&](const auto &errors){for(const auto &error:errors)warnings.append(error.toString());});
            QQmlComponent component(&engine,pageUrl());
            QScopedPointer<QObject> object(component.createWithInitialProperties({{u"fixedScope"_s,scope++},{u"showAdvanced"_s,true},
                {u"consoleSettings"_s,QVariant::fromValue(&console)},{u"virtualSettings"_s,QVariant::fromValue(&virtualHost)},{u"sessionSettings"_s,QVariant::fromValue(&session)}}));
            QVERIFY2(object,qPrintable(component.errorString())); auto *page=qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
            QQuickWindow window; window.resize(900,850); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
            QVERIFY(!hasUnlockObject(page));
            for (const auto &key : {u"Port"_s,u"Quality"_s,u"AdaptiveQuality"_s}) {
                auto *control=find(page,u"host_"_s+key); QVERIFY2(control,qPrintable(key));
                QVERIFY2(qobject_cast<QQuickItem *>(control)->isVisible(),qPrintable(key));
            }
            QVERIFY(!model->modified()); QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u"\n"_s))); page->setParentItem(nullptr);
        }
        // No snapshot: an honest message, never a prompt.
        QTemporaryDir empty; qputenv("FARSIDE_PUBLIC_SETTINGS_DIR", empty.path().toUtf8());
        BrokerHostSettings missing(Scope::Console,u"/nonexistent/helper"_s,unusable,3000);
        QVERIFY(!missing.refresh()); QVERIFY(!missing.loaded()); QVERIFY(missing.error().contains(u"snapshot"));
    }
    void checkedAtLabelIsBlankBeforeInspection() {
        QTemporaryDir dir;
        BrokerHostSettings console(Scope::Console,u"/usr/bin/python3"_s,arguments(dir),3000);
        QQmlEngine engine; localize(engine);
        QQmlComponent component(&engine,pageUrl().resolved(QUrl(u"BrokerServiceDetails.qml"_s))); QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"host"_s,QVariant::fromValue(&console)},
            {u"administration"_s,QVariant::fromValue<QObject *>(nullptr)},{u"route"_s,u"console"_s}}));
        QVERIFY2(object,qPrintable(component.errorString())); auto *page=qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
        QQuickWindow window; window.resize(640,800); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        auto *label=find(page,u"hostRuntimeCheckedAt"_s); QVERIFY(label);
        QCOMPARE(label->property("text").toString(), QString());   // was "Checked at ." (empty timestamp)
        QVERIFY(!label->property("visible").toBool());
        QVERIFY(console.reload()); QTRY_VERIFY(!console.busy());
        QVERIFY(QMetaObject::invokeMethod(find(page,u"inspectHostRuntime"_s),"clicked")); QTRY_VERIFY(!console.busy());
        QVERIFY(!console.runtimeCheckedAt().isEmpty());
        QVERIFY(label->property("text").toString().contains(console.runtimeCheckedAt()));
        QVERIFY(!label->property("text").toString().contains(u"Checked at ."));
        page->setParentItem(nullptr);
    }
    void runtimeInspectionStatesAndWidth_data() {
        QTest::addColumn<QByteArray>("modeName");QTest::addColumn<QString>("summary");
        QTest::newRow("matching")<<QByteArray("success")<<u"agree with stored settings"_s;
        QTest::newRow("custom")<<QByteArray("runtime-custom")<<u"Custom unit"_s;
        QTest::newRow("different")<<QByteArray("runtime-different")<<u"differ from stored"_s;
        QTest::newRow("partial")<<QByteArray("runtime-partial")<<u"incomplete"_s;
        QTest::newRow("reload")<<QByteArray("runtime-reload")<<u"incomplete"_s;
        QTest::newRow("unavailable")<<QByteArray("runtime-unavailable")<<u"unavailable"_s;
        QTest::newRow("stale")<<QByteArray("runtime-stale")<<u"changed during inspection"_s;
        QTest::newRow("denied")<<QByteArray("runtime-denied")<<u"not authorized"_s;
        QTest::newRow("revision")<<QByteArray("runtime-revision")<<u"agree with stored settings"_s;
    }
    void runtimeInspectionStatesAndWidth() {
        QFETCH(QByteArray,modeName);QFETCH(QString,summary);QTemporaryDir dir;
        BrokerHostSettings console(Scope::Console,u"/usr/bin/python3"_s,arguments(dir),3000),
            virtualHost(Scope::Virtual,u"/usr/bin/python3"_s,arguments(dir),3000),
            session(Scope::VirtualSession,u"/usr/bin/python3"_s,arguments(dir),3000);
        QQmlEngine engine;localize(engine);QStringList warnings;
        connect(&engine,&QQmlEngine::warnings,this,[&](const auto &errors){for(const auto &error:errors)warnings.append(error.toString());});
        QQmlComponent component(&engine,pageUrl().resolved(QUrl(u"BrokerServiceDetails.qml"_s)));QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"host"_s,QVariant::fromValue(&console)},
            {u"administration"_s,QVariant::fromValue<QObject *>(nullptr)},{u"route"_s,u"console"_s}}));
        QVERIFY2(object,qPrintable(component.errorString()));auto *page=qobject_cast<QQuickItem *>(object.data());QVERIFY(page);
        QQuickWindow window;window.resize(640,800);page->setParentItem(window.contentItem());page->setSize(window.size());window.show();
        const auto item=[&](const QString &name){return find(page,name);};
        auto *inspect=item(u"inspectHostRuntime"_s);QVERIFY(inspect);QVERIFY(inspect->property("enabled").toBool());
        QVERIFY(console.reload());QTRY_VERIFY(!console.busy());QVERIFY(console.setValue(u"Quality"_s,u"92"_s));
        writeMode(dir,modeName);QVERIFY(QMetaObject::invokeMethod(inspect,"clicked"));QTRY_VERIFY(!console.busy());
        QVERIFY2(console.error().isEmpty(),qPrintable(console.error()));QVERIFY(console.modified());QVERIFY(!console.applicationRequired());
        QTRY_VERIFY(item(u"hostRuntimeSummary"_s)->property("text").toString().contains(summary));
        QCOMPARE(item(u"hostRuntimeStale"_s)->property("visible").toBool(),modeName=="runtime-stale" || modeName=="runtime-revision");
        if(modeName=="runtime-stale" || modeName=="runtime-revision") {
            auto *message=qobject_cast<QQuickItem *>(item(u"hostRuntimeStale"_s));QVERIFY(message);
            std::function<bool(QQuickItem *)> readable=[&](QQuickItem *parent) {
                for(auto *child:parent->childItems()) {
                    if((child->inherits("QQuickText")||child->inherits("QQuickTextEdit"))&&child->property("text")==message->property("text")) {
                        qreal opacity=1;
                        for(auto *ancestor=child;ancestor;ancestor=ancestor->parentItem())opacity*=ancestor->opacity();
                        if(child->isVisible()&&opacity>0.95&&child->height()>0)return true;
                    }
                    if(readable(child))return true;
                }
                return false;
            };
            QTRY_VERIFY(readable(message)); // catch a blank animated warning under a hidden ancestor
        }
        QCOMPARE(item(u"hostRuntimeMissing"_s)->property("visible").toBool(),modeName=="runtime-partial");
        QCOMPARE(item(u"hostRuntimeDifferences"_s)->property("visible").toBool(),modeName=="runtime-different");
        QVERIFY(item(u"showHostRuntimeValues"_s)->setProperty("checked",true));QTRY_VERIFY(item(u"runtime_Port"_s));
        QTest::qWait(100);
        for(const auto &row:console.definitions()) {
            const auto key=row.toMap()[u"key"_s].toString();auto *label=qobject_cast<QQuickItem *>(item(u"runtime_"_s+key));QVERIFY(label);
            const auto rect=label->mapRectToItem(page,QRectF(0,0,label->width(),label->height()));
            QVERIFY2(rect.left()>=-0.5&&rect.right()<=page->width()+0.5,qPrintable(key));
        }
        if(modeName=="runtime-partial")QVERIFY(item(u"runtime_Quality"_s)->property("text").toString().contains(u"not observed"));
        if(modeName=="runtime-different")QVERIFY(item(u"runtime_Quality"_s)->property("text").toString().contains(u"55"));
        const auto screenshots=qEnvironmentVariable("FARSIDE_HOST_SCREENSHOTS");
        if(!screenshots.isEmpty())QVERIFY(window.grabWindow().save(screenshots+u"/runtime-"_s+QString::fromUtf8(modeName)+u".png"_s));
        writeMode(dir,"cancel");QVERIFY(QMetaObject::invokeMethod(inspect,"clicked"));QTRY_VERIFY(!console.busy());
        QVERIFY(console.runtime().isEmpty());QVERIFY(console.modified());QVERIFY(!item(u"hostRuntimeSummary"_s)->property("visible").toBool());
        QVERIFY(!console.modified() || console.values()[u"Quality"_s] == u"92"_s);
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u"\n"_s)));page->setParentItem(nullptr);
    }
    void actualFieldsScopeDraftsDefaultsCancelDiscardAndWidth() {
        QTemporaryDir dir;
        // Reload Saved Settings reads the published public snapshot (no helper, no prompt).
        QTemporaryDir snapshots; qputenv("FARSIDE_PUBLIC_SETTINGS_DIR", snapshots.path().toUtf8());
        const auto restoreEnvironment = qScopeGuard([] { qunsetenv("FARSIDE_PUBLIC_SETTINGS_DIR"); });
        QVERIFY(HostSnapshotFixture::publishAll(snapshots.path()));
        BrokerHostSettings console(Scope::Console,u"/usr/bin/python3"_s,arguments(dir),3000),
            virtualHost(Scope::Virtual,u"/usr/bin/python3"_s,arguments(dir),3000),
            session(Scope::VirtualSession,u"/usr/bin/python3"_s,arguments(dir),3000);
        const QVariantMap desired{{u"Address"_s,u"127.0.0.1"_s},{u"Port"_s,u"3401"_s},{u"Quality"_s,u"93"_s},
            {u"AdaptiveQuality"_s,u"true"_s},{u"PreferAudioQuality"_s,u"true"_s},{u"StandardClientMedia"_s,u"false"_s},
            {u"CameraLoopbackDevice"_s,u"/dev/video10"_s},{u"SoftwareEncoding"_s,u"prefer"_s},{u"Av1Tiles"_s,u"8"_s},
            {u"VaapiDriver"_s,u"off"_s},{u"RenderPci"_s,u"0000:01:00.0"_s}};
        int scope=0;
        for (auto *model : {&console,&virtualHost,&session}) {
            QQmlEngine engine; localize(engine); QStringList warnings;
            connect(&engine,&QQmlEngine::warnings,this,[&](const auto &errors){for(const auto &error:errors)warnings.append(error.toString());});
            QQmlComponent component(&engine,pageUrl());
            QScopedPointer<QObject> object(component.createWithInitialProperties({{u"fixedScope"_s,qMin(scope++,1)},{u"showAdvanced"_s,true},
                {u"consoleSettings"_s,QVariant::fromValue(&console)},{u"virtualSettings"_s,QVariant::fromValue(&virtualHost)},{u"sessionSettings"_s,QVariant::fromValue(&session)}}));
            QVERIFY2(object,qPrintable(component.errorString())); auto *page=qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
            QQuickWindow window; window.resize(900,850); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
            const auto item=[&](const QString &name){return find(page,name);};
            const bool hw=model==&session;   // the session model is edited through the Virtual page's hardware section
            const QString prefix=hw?u"desktop_"_s:u"host_"_s;
            if(hw) QVERIFY(item(u"desktopHardwareSection"_s));   // GPU grants live on the Virtual page; the raw PCI list is under Advanced
            QVERIFY(!model->loaded()); QVERIFY(!item(u"unlockHostSettings"_s)); QVERIFY(!item(u"unlockDesktopHardware"_s)); QVERIFY(model->reload()); QTRY_VERIFY(!model->busy()); QVERIFY(model->loaded());
            for(const auto &definition:model->definitions()) {
                const auto row=definition.toMap();const auto key=row[u"key"_s].toString();
                if(key.startsWith(u"Certificate") || (model==&virtualHost && key==u"CameraLoopbackDevice")) continue;
                auto *control=item(prefix+key); QVERIFY2(control,qPrintable(key));
                if (key==u"Address") {
                    auto *mode=item(prefix+u"AddressMode"_s); QVERIFY(mode); QVERIFY(mode->setProperty("currentIndex",3));
                    QVERIFY(QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,3)));
                }
                const auto choices=row[u"choices"_s].toList();
                if(control->metaObject()->indexOfProperty("from")>=0) {
                    QVERIFY(control->setProperty("value",desired[key].toInt())); QVERIFY(QMetaObject::invokeMethod(control,"valueModified"));
                } else if(choices.isEmpty()) {
                    QVERIFY(control->setProperty("text",desired[key])); QVERIFY(QMetaObject::invokeMethod(control,"textEdited"));
                } else {
                    int selected=-1;for(int i=0;i<choices.size();++i)if(choices[i].toMap()[u"value"_s]==desired[key])selected=i;
                    QVERIFY(selected>=0); QVERIFY(control->setProperty("currentIndex",selected)); QVERIFY(QMetaObject::invokeMethod(control,"activated",Q_ARG(int,selected)));
                }
                QCOMPARE(model->values()[key],desired[key]);
            }
            QVERIFY(model->canSave()); QVERIFY(model->save()); QTRY_VERIFY(!model->busy()); QVERIFY2(model->error().isEmpty(),qPrintable(model->error()));
            QVERIFY(!model->modified()); QVERIFY(model->applicationRequired());
            window.resize(640,700);page->setSize(window.size());QTest::qWait(100);
            auto *flickable=page->property("flickable").value<QQuickItem *>();QVERIFY(flickable);
            auto *content=flickable->property("contentItem").value<QQuickItem *>();QVERIFY(content);
            for(const auto &definition:model->definitions()) {
                const auto key=definition.toMap()[u"key"_s].toString();auto *control=qobject_cast<QQuickItem *>(item(prefix+key));
                if(!control || !control->isVisible())continue;
                const auto rect=control->mapRectToItem(content,QRectF(0,0,control->width(),control->height()));
                QVERIFY2(rect.left()>=-0.5 && rect.right()<=flickable->width()+0.5,qPrintable(key));
            }
            QVERIFY(model->setValue(u"Quality"_s,u"88"_s) || model==&session);
            if(model!=&session) {
                auto *port=item(u"host_Port"_s);QVERIFY(port);QCOMPARE(port->property("to").toInt(),65535);
                QVERIFY(model->setValue(u"Port"_s,u"70000"_s));QVERIFY(!model->canSave());
                QVERIFY(port->setProperty("value",3401));QVERIFY(QMetaObject::invokeMethod(port,"valueModified"));
            }
            model->defaults();QVERIFY(model->values().isEmpty());
            for(const auto &failure:{QByteArray("cancel"),QByteArray("denied"),QByteArray("stale")}) {
                writeMode(dir,failure);QVERIFY(model->save());QTRY_VERIFY(!model->busy());QVERIFY(model->modified());QVERIFY(!model->error().isEmpty());
            }
            writeMode(dir,"success");model->discard();QVERIFY(!model->modified());
            // S3: the page carries no footer and no Save/Revert/Reload/Restore buttons; the standard bar does that.
            for(const auto &old:{u"saveHostSettings"_s,u"discardHostSettings"_s,u"defaultHostSettings"_s,u"loadHostSettings"_s,u"saveDesktopHardware"_s,u"discardDesktopHardware"_s,u"defaultDesktopHardware"_s,u"loadDesktopHardware"_s})
                QVERIFY2(!item(old),qPrintable(old));
            QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u"\n"_s)));page->setParentItem(nullptr);
        }
    }
    void actualImportSelectionPreviewAndSave() {
        QTemporaryDir dir;
        BrokerHostSettings console(Scope::Console, u"/usr/bin/python3"_s, arguments(dir), 3000),
            virtualHost(Scope::Virtual, u"/usr/bin/python3"_s, arguments(dir), 3000), session(Scope::VirtualSession, u"/usr/bin/python3"_s, arguments(dir), 3000);
        QQmlEngine engine; localize(engine); QQmlComponent component(&engine, pageUrl().resolved(QUrl(u"BrokerCertificateSection.qml"_s)));
        QVERIFY(console.reload()); QTRY_VERIFY(!console.busy());
        auto *draft=qobject_cast<BrokerHostSettings *>(console.certificateDraft()); QVERIFY(draft);
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"host"_s,QVariant::fromValue(&console)}}));
        QVERIFY2(object, qPrintable(component.errorString())); auto *page = qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
        QVERIFY(!page->property("open").toBool()); QVERIFY(QMetaObject::invokeMethod(page,"begin")); QVERIFY(page->property("open").toBool());
        QQuickWindow window; window.resize(1000, 900); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        const auto item = [&](const QString &name) { return find(page, name); };
        QVERIFY(console.reload()); QTRY_VERIFY(!console.busy());
        QVERIFY(QMetaObject::invokeMethod(item(u"certificateImport"_s),"clicked")); QCOMPARE(draft->tlsMode(),u"import"_s); QVERIFY(!draft->canStageCertificate());
        KRdp::ServerCertificate::Paths paths{dir.filePath(u"fixture.crt"_s), dir.filePath(u"fixture.key"_s)}; QString error;
        QVERIFY(KRdp::ServerCertificate::generate(paths, u"fixture"_s, QDateTime::currentDateTimeUtc(), 10, &error));
        QVERIFY(page->setProperty("certificateFile", QUrl::fromLocalFile(paths.certificate)));
        QVERIFY(page->setProperty("privateKeyFile", QUrl::fromLocalFile(paths.key)));
        QVERIFY(QMetaObject::invokeMethod(item(u"inspectHostImport"_s), "clicked")); QVERIFY(draft->canStageCertificate());
        QVERIFY(item(u"hostImportPreview"_s)->property("text").toString().contains(draft->importMetadata()[u"fingerprint"_s].toString()));
        QVERIFY(!item(u"hostImportPreview"_s)->property("text").toString().contains(u"PRIVATE KEY"_s));
        auto *keyDialog = item(u"hostPrivateKeyDialog"_s); QVERIFY(keyDialog);
        QVERIFY(keyDialog->setProperty("generation",page->property("selectionGeneration")));
        QVERIFY(keyDialog->setProperty("selectedFile", QUrl::fromLocalFile(paths.key)));
        // A newly accepted file selection invalidates the previously checked
        // pair, even when the selected filename happens to be the same.
        QVERIFY(QMetaObject::invokeMethod(keyDialog, "accepted")); QVERIFY(!draft->canStageCertificate()); QVERIFY(draft->importMetadata().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(item(u"inspectHostImport"_s), "clicked")); QVERIFY(draft->canStageCertificate());
        QVERIFY(QMetaObject::invokeMethod(item(u"stageCertificateEdit"_s),"clicked")); QVERIFY(console.canSave()); QVERIFY(console.save()); QTRY_VERIFY(!console.busy());
        QVERIFY2(console.error().isEmpty(), qPrintable(console.error())); QVERIFY(!console.modified());
        QVERIFY(QFile::exists(dir.filePath(u"import-seen"_s))); QVERIFY(draft->importMetadata().isEmpty());
        page->setParentItem(nullptr);
    }
};
QTEST_MAIN(BrokerHostsPageTest)
#include "BrokerHostsPageTest.moc"
