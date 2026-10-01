// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerHostAdmin.h"
#include "ServerCertificate.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
using namespace Qt::StringLiterals;
using namespace KRdp;
using Scope = BrokerHostSettings::Scope;
class BrokerHostAdminTest : public QObject {
    Q_OBJECT
    static QJsonObject request(Scope scope, bool exists, const QByteArray &bytes, const QVariantMap &values)
    {
        return {{u"version"_s, 1}, {u"operation"_s, u"save"_s}, {u"scope"_s, BrokerHostAdmin::scopeName(scope)},
            {u"revision"_s, BrokerHostAdmin::revision(scope, exists, bytes)}, {u"values"_s, QJsonObject::fromVariantMap(values)}};
    }
    static QByteArray read(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
private Q_SLOTS:
    void scopeAndPresenceBoundRevisions()
    {
        const QByteArray bytes("# keep\nUNRELATED=fixture-only-secret\nFARSIDE_CONSOLE_PORT=4321\n");
        QCOMPARE(BrokerHostAdmin::scope(u"console"_s), std::optional<Scope>(Scope::Console));
        QVERIFY(!BrokerHostAdmin::scope(u"../console"_s));
        QVERIFY(!BrokerHostAdmin::scope(u"virtual-session.conf"_s));
        const auto revision = BrokerHostAdmin::revision(Scope::Console, true, bytes);
        QCOMPARE(revision.size(), 64);
        QVERIFY(revision != BrokerHostAdmin::revision(Scope::Virtual, true, bytes));
        QVERIFY(revision != BrokerHostAdmin::revision(Scope::Console, false, bytes));
        QVERIFY(revision != BrokerHostAdmin::revision(Scope::Console, true, bytes + "# other admin edit\n"));
        const auto view = BrokerHostAdmin::view(Scope::Console, true, bytes);
        QVERIFY(view.error.isEmpty());
        QCOMPARE(view.value[u"revision"_s].toString(), revision);
        QCOMPARE(view.value[u"values"_s].toObject().size(), 1);
        QVERIFY(!QJsonDocument(view.value).toJson().contains("fixture-only-secret"));
        const auto desired = request(Scope::Console, true, bytes, {{u"Port"_s, u"5432"_s}});
        const auto update = BrokerHostAdmin::prepare(Scope::Console, true, bytes, desired);
        QVERIFY2(update.error.isEmpty(), qPrintable(update.error));
        QVERIFY(update.document.startsWith("# keep\nUNRELATED=fixture-only-secret\n"));
        QCOMPARE(update.effective[u"Port"_s].toString(), u"5432"_s);
        QVERIFY(!BrokerHostAdmin::prepare(Scope::Console, true, bytes + "# admin edit\n", desired).error.isEmpty());
        QVERIFY(!BrokerHostAdmin::prepare(Scope::Console, false, bytes, desired).error.isEmpty());
        QVERIFY(!BrokerHostAdmin::prepare(Scope::Virtual, true, bytes, desired).error.isEmpty());
    }
    void typedRequestsAndPreservedFailure()
    {
        const QByteArray bytes("FARSIDE_CONSOLE_PORT=4321\n");
        const auto valid = request(Scope::Console, true, bytes, {{u"Port"_s, u"5432"_s}});
        for (const auto &mutation : QList<QJsonObject>{{{u"path"_s, u"/root/arbitrary"_s}}, {{u"version"_s, 2}},
                {{u"revision"_s, u"bad"_s}}, {{u"values"_s, true}}, {{u"tls"_s, true}},
                {{u"values"_s, QJsonObject{{u"Port"_s, 5432}}}}, {{u"values"_s, QJsonObject{{u"Shell"_s, u"fixture-secret"_s}}}},
                {{u"values"_s, QJsonObject{{u"Port"_s, u"5432\nINJECTION=fixture-secret"_s}}}},
                {{u"tls"_s, QJsonObject{{u"mode"_s, u"keep"_s}, {u"path"_s, u"/root/arbitrary"_s}}}}}) {
            auto desired = valid;
            for (auto it = mutation.begin(); it != mutation.end(); ++it) desired.insert(it.key(), it.value());
            const auto refused = BrokerHostAdmin::prepare(Scope::Console, true, bytes, desired);
            QVERIFY(!refused.error.isEmpty()); QVERIFY(refused.document.isEmpty());
            QVERIFY(!refused.error.contains(u"fixture-secret"_s));
        }
        auto desired = valid;
        desired[u"values"_s] = QJsonObject{{u"Certificate"_s, u"/etc/farside/custom.crt"_s}, {u"CertificateKey"_s, u"/etc/farside/custom.key"_s}};
        QVERIFY(!BrokerHostAdmin::prepare(Scope::Console, true, bytes, desired).error.isEmpty());
        desired[u"tls"_s] = QJsonObject{{u"mode"_s, u"existing"_s}};
        QVERIFY(BrokerHostAdmin::prepare(Scope::Console, true, bytes, desired).error.isEmpty()); // still needs privileged file checks
        desired[u"values"_s] = QJsonObject{{u"Certificate"_s, u"/etc/farside/custom.crt"_s}};
        QVERIFY(!BrokerHostAdmin::prepare(Scope::Console, true, bytes, desired).error.isEmpty());
        auto session = request(Scope::VirtualSession, false, {}, {});
        session[u"tls"_s] = QJsonObject{{u"mode"_s, u"keep"_s}};
        QVERIFY(!BrokerHostAdmin::prepare(Scope::VirtualSession, false, {}, session).error.isEmpty());
    }
    void standardPathsPreserveMaterialAndUnknownBytes()
    {
        const QByteArray original("# retained\nCUSTOM=unchanged\nFARSIDE_VIRTUAL_CERTIFICATE=/etc/farside/custom.crt\nFARSIDE_VIRTUAL_CERTIFICATE_KEY=/etc/farside/custom.key\nFARSIDE_VIRTUAL_PORT=4567\n");
        auto desired = request(Scope::Virtual, true, original, {{u"Port"_s, u"5678"_s}});
        desired[u"tls"_s] = QJsonObject{{u"mode"_s, u"standard"_s}};
        const auto updated = BrokerHostAdmin::prepare(Scope::Virtual, true, original, desired);
        QVERIFY2(updated.error.isEmpty(), qPrintable(updated.error));
        QCOMPARE(updated.effective[u"Certificate"_s].toString(), u"/etc/farside/virtual-host.crt"_s);
        QCOMPARE(updated.effective[u"CertificateKey"_s].toString(), u"/etc/farside/virtual-host.key"_s);
        QVERIFY(updated.document.startsWith("# retained\nCUSTOM=unchanged\n"));
        QVERIFY(updated.certificatePem.isEmpty()); QVERIFY(updated.privateKeyPem.isEmpty());
        desired[u"values"_s] = QJsonObject{{u"Certificate"_s, u"/etc/farside/custom.crt"_s}};
        QVERIFY(!BrokerHostAdmin::prepare(Scope::Virtual, true, original, desired).error.isEmpty());
    }
    void importedPairUsesSharedBoundedNoninteractiveInspection()
    {
        QTemporaryDir dir;
        const ServerCertificate::Paths paths{dir.filePath(u"first.crt"_s), dir.filePath(u"first.key"_s)};
        const auto now = QDateTime::currentDateTimeUtc();
        QString error;
        QVERIFY(ServerCertificate::generate(paths, u"fixture-only"_s, now, 365, &error));
        const auto certificate = read(paths.certificate), key = read(paths.key);
        const auto info = ServerCertificate::inspectPem(certificate, key);
        QVERIFY(info.usable());
        QCOMPARE(info.sha256Fingerprint, ServerCertificate::inspect(paths).sha256Fingerprint);
        QVERIFY(!ServerCertificate::inspectPem(certificate, "not a key").usable());
        QVERIFY(!ServerCertificate::inspectPem(QByteArray(65537, 'x'), key).usable());
        auto desired = request(Scope::Console, false, {}, {{u"Port"_s, u"4321"_s}});
        const QJsonObject tls{{u"mode"_s, u"import"_s}, {u"certificatePem"_s, QString::fromUtf8(certificate)}, {u"privateKeyPem"_s, QString::fromUtf8(key)}};
        desired[u"tls"_s] = tls;
        const auto update = BrokerHostAdmin::prepare(Scope::Console, false, {}, desired);
        QVERIFY2(update.error.isEmpty(), qPrintable(update.error));
        QCOMPARE(update.certificatePem, certificate); QCOMPARE(update.privateKeyPem, key);
        QCOMPARE(update.tls, BrokerHostAdmin::TlsMode::Import);
        QVERIFY(!update.document.contains("BEGIN"));
        for (const auto &field : {u"certificatePem"_s, u"privateKeyPem"_s}) {
            auto invalid = tls; invalid[field] = QString(65537, u'x'); desired[u"tls"_s] = invalid;
            QVERIFY(!BrokerHostAdmin::prepare(Scope::Console, false, {}, desired).error.isEmpty());
        }
        const ServerCertificate::Paths old{dir.filePath(u"old.crt"_s), dir.filePath(u"old.key"_s)};
        QVERIFY(ServerCertificate::generate(old, u"fixture-only"_s, now.addYears(-2), 1, &error));
        auto invalid = tls; invalid[u"certificatePem"_s] = QString::fromUtf8(read(old.certificate));
        invalid[u"privateKeyPem"_s] = QString::fromUtf8(read(old.key)); desired[u"tls"_s] = invalid;
        QVERIFY(!BrokerHostAdmin::prepare(Scope::Console, false, {}, desired).error.isEmpty());
        invalid = tls; invalid[u"privateKeyPem"_s] = QString::fromUtf8(read(old.key)); desired[u"tls"_s] = invalid;
        QVERIFY(!BrokerHostAdmin::prepare(Scope::Console, false, {}, desired).error.isEmpty());
        const ServerCertificate::Paths future{dir.filePath(u"future.crt"_s), dir.filePath(u"future.key"_s)};
        QVERIFY(ServerCertificate::generate(future, u"fixture-only"_s, now.addDays(2), 365, &error));
        invalid = tls; invalid[u"certificatePem"_s] = QString::fromUtf8(read(future.certificate));
        invalid[u"privateKeyPem"_s] = QString::fromUtf8(read(future.key)); desired[u"tls"_s] = invalid;
        QVERIFY(!BrokerHostAdmin::prepare(Scope::Console, false, {}, desired).error.isEmpty());
        const auto view = BrokerHostAdmin::view(Scope::Console, true, update.document);
        const auto publicBytes = QJsonDocument(view.value).toJson();
        QVERIFY(!publicBytes.contains("PRIVATE KEY")); QVERIFY(!publicBytes.contains(key));
    }
};
QTEST_GUILESS_MAIN(BrokerHostAdminTest)
#include "BrokerHostAdminTest.moc"
