// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerhostsettings.h"
#include "settingfielddefinition.h"
#include "ServerCertificate.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <sys/stat.h>
using namespace Qt::StringLiterals;
class BrokerHostSettingsModelTest : public QObject {
    Q_OBJECT
    using Scope = BrokerHostSettings::Scope;
    static void mode(const QTemporaryDir &directory, const QByteArray &bytes) {
        QFile file(directory.filePath(u"mode"_s)); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(bytes), bytes.size());
    }
    static QStringList arguments(const QTemporaryDir &directory) { return {QString::fromUtf8(HOST_PROTOCOL_FIXTURE), directory.path()}; }
private Q_SLOTS:
    void definitionsDriveTheForm() {
        QTemporaryDir directory;
        BrokerHostSettings console(Scope::Console, u"/usr/bin/python3"_s, arguments(directory), 3000), virtualHost(Scope::Virtual, u"/usr/bin/python3"_s, arguments(directory), 3000),
            session(Scope::VirtualSession, u"/usr/bin/python3"_s, arguments(directory), 3000);
        int total = 0;
        for (auto *model : {&console, &virtualHost, &session}) {
            QVERIFY(model->reload()); QTRY_VERIFY(!model->busy()); QVERIFY(model->loaded());
            QSet<QString> keys; QVariantList bounds;
            for (const auto &row : model->definitions()) {
                const auto definition = row.toMap(); const auto key = definition[u"key"_s].toString(); ++total;
                QVERIFY2(KRdp::SettingFields::definitionProblem(definition).isEmpty(), qPrintable(key + u": "_s + KRdp::SettingFields::definitionProblem(definition)));
                QVERIFY(!keys.contains(key)); keys.insert(key);
                const auto control = definition[u"control"_s].toString();
                if (control == u"spin" || control == u"slider") bounds.append(definition);
            }
            // The advertised bounds are values the model accepts and can save, all numeric fields at once.
            for (const auto &edge : bounds.isEmpty() ? QStringList{} : QStringList{u"min"_s, u"max"_s}) {
                for (const auto &row : bounds) QVERIFY(model->setValue(row.toMap()[u"key"_s].toString(), QString::number(row.toMap()[edge].toInt())));
                QVERIFY2(model->canSave(), qPrintable(edge)); model->discard();
            }
            // Only the Virtual camera bridge carries a reason; the page shows it instead of the control.
            for (const auto &row : model->definitions()) {
                const auto definition = row.toMap();
                QCOMPARE(!definition[u"unavailable"_s].toString().isEmpty(), model == &virtualHost && definition[u"key"_s].toString() == u"CameraLoopbackDevice");
            }
        }
        QCOMPARE(total, 25);
    }
    void allFieldsScopesDefaultsAndDiscard() {
        QTemporaryDir directory;
        BrokerHostSettings console(Scope::Console, u"/usr/bin/python3"_s, arguments(directory), 3000),
            virtualHost(Scope::Virtual, u"/usr/bin/python3"_s, arguments(directory), 3000),
            session(Scope::VirtualSession, u"/usr/bin/python3"_s, arguments(directory), 3000);
        QCOMPARE(console.scope(), u"console"_s); QVERIFY(!console.loaded()); QVERIFY(console.reload());
        QTRY_VERIFY(!console.busy()); QVERIFY2(console.loaded(), qPrintable(console.error()));
        QVERIFY(virtualHost.reload()); QTRY_VERIFY(!virtualHost.busy()); QVERIFY(virtualHost.loaded());
        QVERIFY(session.reload()); QTRY_VERIFY(!session.busy()); QVERIFY(session.loaded());
        const QVariantMap values{{u"Address"_s, u"::1"_s}, {u"Port"_s, u"3401"_s}, {u"Quality"_s, u"99"_s},
            {u"AdaptiveQuality"_s, u"true"_s}, {u"PreferAudioQuality"_s, u"true"_s}, {u"StandardClientMedia"_s, u"false"_s},
            {u"CameraLoopbackDevice"_s, u"/dev/video10"_s}, {u"SoftwareEncoding"_s, u"prefer"_s}, {u"Av1Tiles"_s, u"8"_s},
            {u"Certificate"_s, u"/etc/farside/custom.crt"_s}, {u"CertificateKey"_s, u"/etc/farside/custom.key"_s},
            {u"VaapiDriver"_s, u"iHD"_s}, {u"RenderPci"_s, u"0000:01:00.0"_s}};
        int total = 0;
        for (auto *model : {&console, &virtualHost, &session}) {
            if (model->scope() != u"session") QVERIFY(model->chooseTls(u"existing"_s));
            for (const auto &item : model->definitions()) {
                const auto key = item.toMap()[u"key"_s].toString(); ++total;
                const auto desired = model == &virtualHost && key == u"CameraLoopbackDevice" ? u"none"_s : values[key].toString();
                QVERIFY2(model->setValue(key, desired), qPrintable(key));
            }
            QVERIFY(model->canSave()); QVERIFY(model->save()); QVERIFY(!model->setValue(u"Quality"_s, u"50"_s));
            QTRY_VERIFY(!model->busy()); QVERIFY2(model->error().isEmpty(), qPrintable(model->error()));
            QVERIFY(!model->modified()); QVERIFY(model->applicationRequired());
        }
        QCOMPARE(total, 25); QCOMPARE(console.values()[u"Port"_s].toString(), u"3401"_s);
        QVERIFY(!virtualHost.setValue(u"CameraLoopbackDevice"_s, u"/dev/video1"_s));
        QVERIFY(!session.setValue(u"Port"_s, u"3389"_s)); QVERIFY(!session.chooseTls(u"import"_s));
        QVERIFY(console.setValue(u"Quality"_s, u"101"_s)); QVERIFY(console.modified()); QVERIFY(!console.canSave()); QVERIFY(!console.save());
        console.discard(); QVERIFY(!console.modified()); QCOMPARE(console.values()[u"Quality"_s].toString(), u"99"_s);
        console.defaults(); QCOMPARE(console.values().size(), 2); QCOMPARE(console.tlsMode(), u"keep"_s);
        QCOMPARE(console.values()[u"Certificate"_s].toString(), u"/etc/farside/custom.crt"_s);
        QVERIFY(console.save()); QTRY_VERIFY(!console.busy()); QCOMPARE(console.values().size(), 2);
        QCOMPARE(virtualHost.values()[u"Quality"_s].toString(), u"99"_s); // independent drafts/files
        QVERIFY(session.setValue(u"RenderPci"_s, u""_s)); QVERIFY(session.save()); QTRY_VERIFY(!session.busy());
        QCOMPARE(session.values()[u"RenderPci"_s].toString(), QString());
    }
    void failedRequestsPreservePendingAndSanitizeErrors_data() {
        QTest::addColumn<QByteArray>("failure"); QTest::addColumn<bool>("unknown");
        QTest::newRow("cancel") << QByteArray("cancel") << false;
        QTest::newRow("denied") << QByteArray("denied") << false;
        QTest::newRow("stale") << QByteArray("stale") << false;
        for (const auto &name : {"malformed", "oversized", "wrong-scope", "private-field", "runtime-lie", "wrong-effective", "saved-error", "timeout", "crash-after-save"})
            QTest::newRow(name) << QByteArray(name) << true;
    }
    void failedRequestsPreservePendingAndSanitizeErrors() {
        QFETCH(QByteArray, failure); QFETCH(bool, unknown); QTemporaryDir directory;
        BrokerHostSettings settings(Scope::Console, u"/usr/bin/python3"_s, arguments(directory), failure == "timeout" ? 200 : 3000);
        QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); QVERIFY(settings.loaded()); QVERIFY(settings.setValue(u"Port"_s, u"3501"_s));
        const auto before = settings.values(); mode(directory, failure); QVERIFY(settings.save()); QTRY_VERIFY(!settings.busy());
        QCOMPARE(settings.values(), before); QVERIFY(settings.modified()); QCOMPARE(settings.outcomeUnknown(), unknown);
        QVERIFY(!settings.error().isEmpty()); QVERIFY(!settings.error().contains(u"fixture-private"_s));
        QVERIFY(!QJsonDocument::fromVariant(settings.metadata()).toJson().contains("fixture-private"));
        if (unknown) QVERIFY(!settings.canSave());
        mode(directory, "success"); QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); QVERIFY(!settings.outcomeUnknown()); QVERIFY(!settings.modified());
        if (failure == "crash-after-save") { QVERIFY(settings.applicationRequired()); QCOMPARE(settings.values(), before); }
    }
    void tlsOperationsAndPrivateImportBuffers() {
        QTemporaryDir directory; BrokerHostSettings settings(Scope::Console, u"/usr/bin/python3"_s, arguments(directory), 3000);
        QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy());
        QVERIFY(!settings.setValue(u"Certificate"_s, u"/etc/test.crt"_s));
        QVERIFY(settings.chooseTls(u"existing"_s)); QVERIFY(settings.setValue(u"Certificate"_s, u"/etc/other.crt"_s));
        QVERIFY(settings.chooseTls(u"keep"_s)); QVERIFY(!settings.modified());
        QVERIFY(settings.chooseTls(u"import"_s)); QVERIFY(!settings.canSave());
        KRdp::ServerCertificate::Paths paths{directory.filePath(u"fixture.crt"_s), directory.filePath(u"fixture.key"_s)};
        QString error; QVERIFY(KRdp::ServerCertificate::generate(paths, u"fixture"_s, QDateTime::currentDateTimeUtc(), 10, &error));
        QVERIFY(settings.importTls(QUrl::fromLocalFile(paths.certificate), QUrl::fromLocalFile(paths.key)));
        QCOMPARE(settings.importMetadata()[u"fingerprint"_s], KRdp::ServerCertificate::inspect(paths).sha256Fingerprint);
        QVERIFY(settings.canSave());
        const auto *meta = settings.metaObject();
        for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
            const auto value = meta->property(i).read(&settings);
            QVERIFY(!QJsonDocument::fromVariant(value).toJson().contains("PRIVATE KEY"));
        }
        QVERIFY(settings.save()); QTRY_VERIFY(!settings.busy()); QVERIFY2(settings.error().isEmpty(), qPrintable(settings.error()));
        QVERIFY(QFile::exists(directory.filePath(u"import-seen"_s))); QVERIFY(settings.importMetadata().isEmpty());
        QCOMPARE(settings.values()[u"Certificate"_s].toString(), u"/etc/farside/tls-imports/fixture/certificate.crt"_s);
        QVERIFY(settings.chooseTls(u"standard"_s)); QVERIFY(!settings.values().contains(u"Certificate"_s));
        settings.discard(); QCOMPARE(settings.tlsMode(), u"keep"_s); QVERIFY(!settings.modified());
        QVERIFY(settings.chooseTls(u"import"_s));
        const auto fifo = directory.filePath(u"fifo"_s); QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        QVERIFY(!settings.importTls(QUrl::fromLocalFile(fifo), QUrl::fromLocalFile(paths.key))); QVERIFY(!settings.canSave());
        QVERIFY(!settings.importTls(QUrl(u"https://invalid.example/cert"_s), QUrl::fromLocalFile(paths.key)));
        QVERIFY(!settings.importTls(QUrl::fromLocalFile(u"/dev/zero"_s), QUrl::fromLocalFile(paths.key)));
        QFile oversized(directory.filePath(u"large"_s)); QVERIFY(oversized.open(QIODevice::WriteOnly)); oversized.write(QByteArray(65537, 'x')); oversized.close();
        QVERIFY(!settings.importTls(QUrl::fromLocalFile(oversized.fileName()), QUrl::fromLocalFile(paths.key)));
        QVERIFY(settings.importMetadata().isEmpty());
    }
    void certificateDraftIsolationAndDefaultsPreserveImport() {
        QTemporaryDir directory;
        BrokerHostSettings host(Scope::Console, u"/usr/bin/python3"_s, arguments(directory), 3000);
        QVERIFY(host.reload()); QTRY_VERIFY(!host.busy());
        QVERIFY(host.setValue(u"Port"_s, u"3401"_s)); const auto before = host.values();
        QVERIFY(host.beginCertificateEdit());
        auto *draft = qobject_cast<BrokerHostSettings *>(host.certificateDraft()); QVERIFY(draft);
        QVERIFY(draft->chooseTls(u"standard"_s)); QCOMPARE(host.values(), before); QCOMPARE(host.tlsMode(), u"keep"_s);
        QVERIFY(!draft->save()); QVERIFY(!draft->reload()); QVERIFY(!draft->inspectRuntime());
        host.cancelCertificateEdit(); QCOMPARE(host.values(), before); QCOMPARE(host.tlsMode(), u"keep"_s);
        QVERIFY(host.beginCertificateEdit()); QVERIFY(draft->chooseTls(u"import"_s)); QVERIFY(!draft->canStageCertificate());
        KRdp::ServerCertificate::Paths paths{directory.filePath(u"fixture.crt"_s), directory.filePath(u"fixture.key"_s)};
        QString error; QVERIFY(KRdp::ServerCertificate::generate(paths, u"fixture"_s, QDateTime::currentDateTimeUtc(), 10, &error));
        QVERIFY(draft->importTls(QUrl::fromLocalFile(paths.certificate), QUrl::fromLocalFile(paths.key)));
        QVERIFY(host.stageCertificateEdit()); QCOMPARE(host.tlsMode(), u"import"_s); QCOMPARE(host.values(), before);
        const auto imported = host.importMetadata(); QVERIFY(!imported.isEmpty());
        QVERIFY(host.beginCertificateEdit()); QVERIFY(draft->chooseTls(u"standard"_s)); host.cancelCertificateEdit();
        QCOMPARE(host.tlsMode(), u"import"_s); QCOMPARE(host.importMetadata(), imported);
        host.defaults(); QVERIFY(!host.values().contains(u"Port"_s)); QCOMPARE(host.tlsMode(), u"import"_s); QCOMPARE(host.importMetadata(), imported);
        QVERIFY(host.canSave()); QVERIFY(host.save()); QTRY_VERIFY(!host.busy());
        QVERIFY2(host.error().isEmpty(), qPrintable(host.error())); QVERIFY(QFile::exists(directory.filePath(u"import-seen"_s)));
        QVERIFY(host.importMetadata().isEmpty());
    }
    void stderrIsDrainedAndMissingProgramIsSafe() {
        QTemporaryDir directory; mode(directory, "stderr");
        BrokerHostSettings settings(Scope::VirtualSession, u"/usr/bin/python3"_s, arguments(directory), 3000);
        QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); QVERIFY2(settings.loaded(), qPrintable(settings.error()));
        BrokerHostSettings missing(Scope::Console, u"/nonexistent/farside-test-helper"_s, {}, 500);
        QVERIFY(missing.reload()); QTRY_VERIFY(!missing.busy()); QVERIFY(!missing.loaded()); QVERIFY(!missing.outcomeUnknown());
    }
    void runtimeInspectionPreservesDraftsTlsAndApplicationState() {
        QTemporaryDir dir;
        BrokerHostSettings settings(Scope::Console, u"/usr/bin/python3"_s, arguments(dir), 3000),
            session(Scope::VirtualSession, u"/usr/bin/python3"_s, arguments(dir), 3000);
        QVERIFY(!settings.inspectRuntime()); QVERIFY(session.reload()); QTRY_VERIFY(!session.busy()); QVERIFY(!session.inspectRuntime());
        QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); QVERIFY(settings.setValue(u"Port"_s, u"3501"_s));
        QVERIFY(settings.chooseTls(u"import"_s));
        KRdp::ServerCertificate::Paths paths{dir.filePath(u"fixture.crt"_s), dir.filePath(u"fixture.key"_s)}; QString error;
        QVERIFY(KRdp::ServerCertificate::generate(paths, u"fixture"_s, QDateTime::currentDateTimeUtc(), 10, &error));
        QVERIFY(settings.importTls(QUrl::fromLocalFile(paths.certificate), QUrl::fromLocalFile(paths.key)));
        const auto pending=settings.values(), certificate=settings.importMetadata();
        mode(dir,"runtime-different"); QVERIFY(settings.inspectRuntime()); QVERIFY(!settings.save()); QTRY_VERIFY(!settings.busy());
        QVERIFY2(settings.error().isEmpty(),qPrintable(settings.error())); QCOMPARE(settings.runtime()[u"state"_s],u"different"_s);
        QCOMPARE(settings.runtime()[u"running"_s].toMap()[u"Quality"_s],u"55"_s);
        QCOMPARE(settings.values(),pending); QCOMPARE(settings.importMetadata(),certificate); QCOMPARE(settings.tlsMode(),u"import"_s);
        QVERIFY(settings.modified()); QVERIFY(settings.canSave()); QVERIFY(!settings.applicationRequired()); QVERIFY(!settings.runtimeStale());
        QVERIFY(!settings.runtimeCheckedAt().isEmpty()); mode(dir,"success"); QVERIFY(settings.save()); QTRY_VERIFY(!settings.busy());
        QVERIFY(settings.applicationRequired()); QVERIFY(settings.runtimeStale()); QVERIFY(settings.importMetadata().isEmpty());
        QVERIFY(settings.inspectRuntime()); QTRY_VERIFY(!settings.busy()); QVERIFY(!settings.runtimeStale()); QVERIFY(settings.applicationRequired());
        QVERIFY(settings.setValue(u"Port"_s,u"3502"_s)); mode(dir,"crash-after-save"); QVERIFY(settings.save()); QTRY_VERIFY(!settings.busy());
        QVERIFY(settings.outcomeUnknown()); mode(dir,"success"); QVERIFY(settings.inspectRuntime()); QTRY_VERIFY(!settings.busy());
        QVERIFY(settings.outcomeUnknown()); QVERIFY(!settings.canSave()); QVERIFY(settings.modified()); QVERIFY(settings.runtimeStale());
    }
    void runtimeFailuresAndPartialReplies_data() {
        QTest::addColumn<QByteArray>("modeName");QTest::addColumn<bool>("accepted");QTest::addColumn<bool>("stale");
        for(const auto &name:{"cancel","denied","timeout","malformed","oversized","wrong-scope","private-field","runtime-lie"})
            QTest::newRow(name)<<QByteArray(name)<<false<<true;
        for(const auto &name:{"runtime-partial","runtime-custom","runtime-unavailable","runtime-denied","runtime-reload"})
            QTest::newRow(name)<<QByteArray(name)<<true<<false;
        QTest::newRow("revision")<<QByteArray("runtime-revision")<<true<<true;
        QTest::newRow("changed")<<QByteArray("runtime-stale")<<true<<true;
    }
    void runtimeFailuresAndPartialReplies() {
        QFETCH(QByteArray,modeName);QFETCH(bool,accepted);QFETCH(bool,stale);QTemporaryDir dir;
        BrokerHostSettings settings(Scope::Console,u"/usr/bin/python3"_s,arguments(dir),modeName=="timeout"?200:3000);
        QVERIFY(settings.reload());QTRY_VERIFY(!settings.busy());QVERIFY(settings.inspectRuntime());QTRY_VERIFY(!settings.busy());
        QVERIFY(!settings.runtime().isEmpty());QVERIFY(settings.setValue(u"Quality"_s,u"92"_s)); const auto pending=settings.values();
        mode(dir,modeName);QVERIFY(settings.inspectRuntime());QTRY_VERIFY(!settings.busy());
        QCOMPARE(settings.runtime().isEmpty(),!accepted);QCOMPARE(settings.runtimeStale(),stale);
        QCOMPARE(settings.values(),pending);QVERIFY(settings.modified());QVERIFY(settings.canSave());QVERIFY(!settings.outcomeUnknown());
        QVERIFY(!QJsonDocument::fromVariant(settings.runtime()).toJson().contains("fixture-private"));
        if(!accepted) { QVERIFY(settings.runtimeCheckedAt().isEmpty());QVERIFY(!settings.error().isEmpty()); }
    }
};
QTEST_GUILESS_MAIN(BrokerHostSettingsModelTest)
#include "BrokerHostSettingsModelTest.moc"
