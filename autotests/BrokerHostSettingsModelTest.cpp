// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "brokerhostsettings.h"
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
        console.defaults(); QVERIFY(console.values().isEmpty()); QCOMPARE(console.tlsMode(), u"standard"_s);
        QVERIFY(console.save()); QTRY_VERIFY(!console.busy()); QVERIFY(console.values().isEmpty());
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
    void stderrIsDrainedAndMissingProgramIsSafe() {
        QTemporaryDir directory; mode(directory, "stderr");
        BrokerHostSettings settings(Scope::VirtualSession, u"/usr/bin/python3"_s, arguments(directory), 3000);
        QVERIFY(settings.reload()); QTRY_VERIFY(!settings.busy()); QVERIFY2(settings.loaded(), qPrintable(settings.error()));
        BrokerHostSettings missing(Scope::Console, u"/nonexistent/farside-test-helper"_s, {}, 500);
        QVERIFY(missing.reload()); QTRY_VERIFY(!missing.busy()); QVERIFY(!missing.loaded()); QVERIFY(!missing.outcomeUnknown());
    }
};
QTEST_GUILESS_MAIN(BrokerHostSettingsModelTest)
#include "BrokerHostSettingsModelTest.moc"
