// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerhostsettings.h"
#include "ServerCertificate.h"
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
private Q_SLOTS:
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
        QQmlComponent component(&engine,pageUrl());QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"consoleSettings"_s,QVariant::fromValue(&console)},
            {u"virtualSettings"_s,QVariant::fromValue(&virtualHost)},{u"sessionSettings"_s,QVariant::fromValue(&session)}}));
        QVERIFY2(object,qPrintable(component.errorString()));auto *page=qobject_cast<QQuickItem *>(object.data());QVERIFY(page);
        QQuickWindow window;window.resize(640,800);page->setParentItem(window.contentItem());page->setSize(window.size());window.show();
        const auto item=[&](const QString &name){return find(page,name);};
        auto *inspect=item(u"inspectHostRuntime"_s);QVERIFY(inspect);QVERIFY(!inspect->property("enabled").toBool());
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
        auto *flickable=page->property("flickable").value<QQuickItem *>();QVERIFY(flickable);QTest::qWait(100);
        auto *content=flickable->property("contentItem").value<QQuickItem *>();QVERIFY(content);
        for(const auto &row:console.definitions()) {
            const auto key=row.toMap()[u"key"_s].toString();auto *label=qobject_cast<QQuickItem *>(item(u"runtime_"_s+key));QVERIFY(label);
            const auto rect=label->mapRectToItem(content,QRectF(0,0,label->width(),label->height()));
            QVERIFY2(rect.left()>=-0.5&&rect.right()<=flickable->width()+0.5,qPrintable(key));
        }
        if(modeName=="runtime-partial")QVERIFY(item(u"runtime_Quality"_s)->property("text").toString().contains(u"not observed"));
        if(modeName=="runtime-different")QVERIFY(item(u"runtime_Quality"_s)->property("text").toString().contains(u"55"));
        const auto screenshots=qEnvironmentVariable("FARSIDE_HOST_SCREENSHOTS");
        if(!screenshots.isEmpty())QVERIFY(window.grabWindow().save(screenshots+u"/runtime-"_s+QString::fromUtf8(modeName)+u".png"_s));
        writeMode(dir,"cancel");QVERIFY(QMetaObject::invokeMethod(inspect,"clicked"));QTRY_VERIFY(!console.busy());
        QVERIFY(console.runtime().isEmpty());QVERIFY(console.modified());QVERIFY(!item(u"hostRuntimeSummary"_s)->property("visible").toBool());
        auto *selector=item(u"hostScope"_s);QVERIFY(selector->setProperty("currentIndex",2));QVERIFY(!inspect->property("visible").toBool());
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join(u"\n"_s)));page->setParentItem(nullptr);
    }
    void actualFieldsScopeDraftsDefaultsCancelDiscardAndWidth() {
        QTemporaryDir dir;
        BrokerHostSettings console(Scope::Console, u"/usr/bin/python3"_s, arguments(dir), 3000),
            virtualHost(Scope::Virtual, u"/usr/bin/python3"_s, arguments(dir), 3000),
            session(Scope::VirtualSession, u"/usr/bin/python3"_s, arguments(dir), 3000);
        QQmlEngine engine; localize(engine); QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const auto &errors) { for (const auto &error : errors) warnings.append(error.toString()); });
        QQmlComponent component(&engine, pageUrl()); QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"showAdvanced"_s, true}, {u"consoleSettings"_s, QVariant::fromValue(&console)},
            {u"virtualSettings"_s, QVariant::fromValue(&virtualHost)}, {u"sessionSettings"_s, QVariant::fromValue(&session)}}));
        QVERIFY2(object, qPrintable(component.errorString())); auto *page = qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
        QQuickWindow window; window.resize(1000, 900); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        const auto item = [&](const QString &name) { return find(page, name); };
        const auto click = [&](const QString &name) { auto *button = item(name); return button && QMetaObject::invokeMethod(button, "clicked"); };
        auto *selector = item(u"hostScope"_s); QVERIFY(selector); QVERIFY(!console.loaded()); QVERIFY(!virtualHost.loaded()); QVERIFY(!session.loaded());
        const QVariantMap desired{{u"Address"_s, u"127.0.0.1"_s}, {u"Port"_s, u"3401"_s}, {u"Quality"_s, u"93"_s},
            {u"AdaptiveQuality"_s, u"true"_s}, {u"PreferAudioQuality"_s, u"true"_s}, {u"StandardClientMedia"_s, u"false"_s},
            {u"CameraLoopbackDevice"_s, u"/dev/video10"_s}, {u"SoftwareEncoding"_s, u"prefer"_s}, {u"Av1Tiles"_s, u"8"_s},
            {u"Certificate"_s, u"/etc/farside/custom.crt"_s}, {u"CertificateKey"_s, u"/etc/farside/custom.key"_s},
            {u"VaapiDriver"_s, u"off"_s}, {u"RenderPci"_s, u"0000:01:00.0"_s}};
        int scope = 0, fields = 0;
        for (auto *model : {&console, &virtualHost, &session}) {
            QVERIFY(selector->setProperty("currentIndex", scope++)); QVERIFY(click(u"loadHostSettings"_s));
            QTRY_VERIFY(!model->busy()); QVERIFY2(model->loaded(), qPrintable(model->error()));
            if (model != &session) {
                auto *tls = item(u"hostTlsOperation"_s); QVERIFY(tls); QVERIFY(tls->setProperty("currentIndex", 1));
                QVERIFY(QMetaObject::invokeMethod(tls, "activated", Q_ARG(int, 1))); QCOMPARE(model->tlsMode(), u"existing"_s);
            }
            for (const auto &definition : model->definitions()) {
                const auto row = definition.toMap(); const auto key = row[u"key"_s].toString();
                QTRY_VERIFY(item(u"host_"_s + key)); auto *control = item(u"host_"_s + key); ++fields;
                QTRY_VERIFY2(control->property("visible").toBool(),qPrintable(key + u" hidden in its scope"_s));
                if (model == &virtualHost && key == u"CameraLoopbackDevice") { QVERIFY(!control->property("enabled").toBool()); continue; }
                const auto choices = row[u"choices"_s].toList();
                if (control->metaObject()->indexOfProperty("checkState") >= 0) {
                    QVERIFY(control->setProperty("checkState", int(desired[key].toString() == u"true"_s ? Qt::Checked : Qt::Unchecked)));
                    QVERIFY(QMetaObject::invokeMethod(control, "clicked"));
                } else if (control->metaObject()->indexOfProperty("from") >= 0) {
                    QVERIFY(control->setProperty("value", desired[key].toInt()));
                    QVERIFY(QMetaObject::invokeMethod(control, "valueModified"));
                } else if (choices.isEmpty()) { QVERIFY(control->setProperty("text", desired[key])); QVERIFY(QMetaObject::invokeMethod(control, "textEdited")); }
                else {
                    int selected = -1; for (int i = 0; i < choices.size(); ++i) if (choices[i].toMap()[u"value"_s] == desired[key]) selected = i;
                    QVERIFY(selected >= 0); QVERIFY(control->setProperty("currentIndex", selected)); QVERIFY(QMetaObject::invokeMethod(control, "activated", Q_ARG(int, selected)));
                }
                QCOMPARE(model->values()[key], desired[key]);
            }
            QVERIFY(model->canSave()); QVERIFY(click(u"saveHostSettings"_s)); QTRY_VERIFY(!model->busy());
            QVERIFY2(model->error().isEmpty(), qPrintable(model->error())); QVERIFY(model->applicationRequired()); QVERIFY(!model->modified());
            auto *flickable = page->property("flickable").value<QQuickItem *>(); QVERIFY(flickable); QTest::qWait(100);
            const auto screenshots = qEnvironmentVariable("FARSIDE_HOST_SCREENSHOTS");
            if (!screenshots.isEmpty()) {
                const qreal maximum = qMax<qreal>(0, flickable->property("contentHeight").toReal() - flickable->height());
                const qreal step = qMax<qreal>(1, flickable->height() * 0.8); int shot = 0;
                for (qreal y = 0;; y = qMin(maximum, y + step)) {
                    flickable->setProperty("contentY", y); QTest::qWait(150);
                    QVERIFY(window.grabWindow().save(screenshots + u"/hosts-"_s + model->scope() + u"-"_s + QString::number(shot++) + u".png"_s));
                    if (y >= maximum) break;
                }
            }
            window.resize(640, 700); page->setSize(window.size()); QTest::qWait(100);
            auto *content = flickable->property("contentItem").value<QQuickItem *>(); QVERIFY(content);
            for (const auto &definition : model->definitions()) {
                const auto key = definition.toMap()[u"key"_s].toString(); auto *control = qobject_cast<QQuickItem *>(item(u"host_"_s + key)); QVERIFY(control);
                const auto bounds = control->mapRectToItem(content, QRectF(0, 0, control->width(), control->height()));
                QVERIFY2(bounds.left() >= -0.5 && bounds.right() <= flickable->width() + 0.5, qPrintable(key + u" exceeds width640"_s));
            }
            window.resize(1000, 900); page->setSize(window.size());
        }
        QCOMPARE(fields, 25);
        QVERIFY(selector->setProperty("currentIndex", 0)); QTRY_VERIFY(item(u"host_Quality"_s));
        auto *quality = qobject_cast<QQuickItem *>(item(u"host_Quality"_s)); QVERIFY(quality); quality->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_A, Qt::ControlModifier); QTest::keyClick(&window, Qt::Key_8); QTest::keyClick(&window, Qt::Key_8); QTest::keyClick(&window, Qt::Key_Return);
        QTRY_COMPARE(console.values()[u"Quality"_s].toString(), u"88"_s);
        auto *port = item(u"host_Port"_s); QVERIFY(port); QCOMPARE(port->property("to").toInt(),65535); QVERIFY(console.setValue(u"Port"_s, u"70000"_s));
        QVERIFY(!item(u"saveHostSettings"_s)->property("enabled").toBool()); QVERIFY(console.modified());
        QVERIFY(port->setProperty("value", 3401)); QVERIFY(QMetaObject::invokeMethod(port, "valueModified"));
        QVERIFY(selector->setProperty("currentIndex", 1)); QVERIFY(click(u"defaultHostSettings"_s)); QVERIFY(virtualHost.values().isEmpty());
        QCOMPARE(console.values()[u"Quality"_s].toString(), u"88"_s);
        for (const auto &failure : {QByteArray("cancel"), QByteArray("denied"), QByteArray("stale")}) {
            writeMode(dir, failure); QVERIFY(click(u"saveHostSettings"_s)); QTRY_VERIFY(!virtualHost.busy());
            QVERIFY(virtualHost.modified()); QVERIFY(virtualHost.values().isEmpty()); QVERIFY(!virtualHost.error().isEmpty());
        }
        writeMode(dir, "success"); QVERIFY(click(u"loadHostSettings"_s)); auto *dialog = item(u"reloadHostConfirmation"_s); QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool()); QVERIFY(QMetaObject::invokeMethod(dialog, "reject")); QVERIFY(virtualHost.modified());
        QVERIFY(click(u"loadHostSettings"_s)); QVERIFY(QMetaObject::invokeMethod(dialog, "accept")); QTRY_VERIFY(!virtualHost.busy());
        QVERIFY(!virtualHost.modified()); QCOMPARE(virtualHost.values()[u"Quality"_s].toString(), u"93"_s);
        QVERIFY(selector->setProperty("currentIndex", 0)); QTRY_COMPARE(item(u"host_Quality"_s)->property("value").toInt(), 88);
        QVERIFY(click(u"discardHostSettings"_s)); QVERIFY(!console.modified());
        QVERIFY(click(u"defaultHostSettings"_s)); QCOMPARE(console.tlsMode(), u"standard"_s); QVERIFY(console.values().isEmpty());
        QVERIFY(click(u"saveHostSettings"_s)); QTRY_VERIFY(!console.busy()); QVERIFY(console.values().isEmpty());
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u"\n"_s))); page->setParentItem(nullptr);
    }
    void actualImportSelectionPreviewAndSave() {
        QTemporaryDir dir;
        BrokerHostSettings console(Scope::Console, u"/usr/bin/python3"_s, arguments(dir), 3000),
            virtualHost(Scope::Virtual, u"/usr/bin/python3"_s, arguments(dir), 3000), session(Scope::VirtualSession, u"/usr/bin/python3"_s, arguments(dir), 3000);
        QQmlEngine engine; localize(engine); QQmlComponent component(&engine, pageUrl());
        QScopedPointer<QObject> object(component.createWithInitialProperties({{u"showAdvanced"_s, true}, {u"consoleSettings"_s, QVariant::fromValue(&console)},
            {u"virtualSettings"_s, QVariant::fromValue(&virtualHost)}, {u"sessionSettings"_s, QVariant::fromValue(&session)}}));
        QVERIFY2(object, qPrintable(component.errorString())); auto *page = qobject_cast<QQuickItem *>(object.data()); QVERIFY(page);
        QQuickWindow window; window.resize(1000, 900); page->setParentItem(window.contentItem()); page->setSize(window.size()); window.show();
        const auto item = [&](const QString &name) { return find(page, name); };
        QVERIFY(console.reload()); QTRY_VERIFY(!console.busy());
        auto *tls = item(u"hostTlsOperation"_s); QVERIFY(tls); QVERIFY(tls->setProperty("currentIndex", 3));
        QVERIFY(QMetaObject::invokeMethod(tls, "activated", Q_ARG(int, 3))); QCOMPARE(console.tlsMode(), u"import"_s); QVERIFY(!console.canSave());
        KRdp::ServerCertificate::Paths paths{dir.filePath(u"fixture.crt"_s), dir.filePath(u"fixture.key"_s)}; QString error;
        QVERIFY(KRdp::ServerCertificate::generate(paths, u"fixture"_s, QDateTime::currentDateTimeUtc(), 10, &error));
        QVERIFY(page->setProperty("certificateFile", QUrl::fromLocalFile(paths.certificate)));
        QVERIFY(page->setProperty("privateKeyFile", QUrl::fromLocalFile(paths.key)));
        QVERIFY(QMetaObject::invokeMethod(item(u"inspectHostImport"_s), "clicked")); QVERIFY(console.canSave());
        QVERIFY(item(u"hostImportPreview"_s)->property("text").toString().contains(console.importMetadata()[u"fingerprint"_s].toString()));
        QVERIFY(!item(u"hostImportPreview"_s)->property("text").toString().contains(u"PRIVATE KEY"_s));
        auto *keyDialog = item(u"hostPrivateKeyDialog"_s); QVERIFY(keyDialog);
        QVERIFY(keyDialog->setProperty("target", QVariant::fromValue(&console)));
        QVERIFY(keyDialog->setProperty("selectedFile", QUrl::fromLocalFile(paths.key)));
        // A newly accepted file selection invalidates the previously checked
        // pair, even when the selected filename happens to be the same.
        QVERIFY(QMetaObject::invokeMethod(keyDialog, "accepted")); QVERIFY(!console.canSave()); QVERIFY(console.importMetadata().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(item(u"inspectHostImport"_s), "clicked")); QVERIFY(console.canSave());
        QVERIFY(QMetaObject::invokeMethod(item(u"saveHostSettings"_s), "clicked")); QTRY_VERIFY(!console.busy());
        QVERIFY2(console.error().isEmpty(), qPrintable(console.error())); QVERIFY(!console.modified());
        QVERIFY(QFile::exists(dir.filePath(u"import-seen"_s))); QVERIFY(console.importMetadata().isEmpty());
        page->setParentItem(nullptr);
    }
};
QTEST_MAIN(BrokerHostsPageTest)
#include "BrokerHostsPageTest.moc"
