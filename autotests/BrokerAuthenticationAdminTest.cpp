// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerAuthenticationAdmin.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace KRdp;
using namespace Qt::StringLiterals;
class BrokerAuthenticationAdminTest : public QObject
{
    Q_OBJECT
    BrokerAuthentication::Result current;
    static std::optional<quint32> resolve(const QString &name)
    {
        if (name == u"owner-a"_s || name == u"owner-a-alt"_s) return 1000;
        if (name == u"owner-b"_s) return 1001;
        if (name == u"root"_s) return 0;
        return {};
    }
    static std::optional<QString> name(quint32 uid)
    {
        return uid == 1000 ? std::optional(u"owner-a"_s) : uid == 1001 ? std::optional(u"owner-b"_s) : std::nullopt;
    }
    QJsonObject request() const { return BrokerAuthenticationAdmin::view(current, name).value; }
    static void alias(QJsonObject &request, const QString &route, const QJsonObject &value)
    {
        auto object = request.value(route).toObject(); object.insert(u"credentials"_s, QJsonArray{value}); request.insert(route, object);
    }
private Q_SLOTS:
    void initTestCase()
    {
        const auto verifier = BrokerAuthentication::makeCredential(1000, u"fixture-only-password"_s);
        QVERIFY(verifier);
        const QString encoded = u"pbkdf2-sha256$600000$"_s + QString::fromLatin1(verifier->salt.toHex()) + u"$"_s + QString::fromLatin1(verifier->digest.toHex());
        const auto route = [&](const QString &owner) {
            return QJsonObject{{u"pam"_s, QJsonObject{{u"mode"_s, u"allow-list"_s}, {u"accounts"_s, QJsonArray{owner}}}},
                {u"credentials"_s, QJsonArray{QJsonObject{{u"alias"_s, u"guest"_s}, {u"owner"_s, owner}, {u"verifier"_s, encoded}}}}};
        };
        const auto bytes = QJsonDocument(QJsonObject{{u"version"_s, 1}, {u"console"_s, route(u"owner-a"_s)}, {u"virtual"_s, route(u"owner-b"_s)}}).toJson();
        current = BrokerAuthentication::parse(bytes, resolve); QVERIFY(current.policy); current.document = bytes;
    }
    void publicSnapshotHasNoSecrets()
    {
        const auto snapshot = request();
        QCOMPARE(snapshot.value(u"revision"_s).toString(), BrokerAuthenticationAdmin::revision(current));
        const auto bytes = QJsonDocument(snapshot).toJson();
        for (const auto &secret : {QByteArray("verifier"), QByteArray("password"), QByteArray("pbkdf2"), current.policy->console.credentials.value(u"guest"_s).salt.toHex(),
             current.policy->console.credentials.value(u"guest"_s).digest.toHex()}) QVERIFY(!bytes.contains(secret));
        QCOMPARE(snapshot.value(u"console"_s).toObject().value(u"credentials"_s).toArray().first().toObject().value(u"owner"_s).toString(), u"owner-a"_s);
        QCOMPARE(snapshot.value(u"virtual"_s).toObject().value(u"credentials"_s).toArray().first().toObject().value(u"owner"_s).toString(), u"owner-b"_s);
    }
    void unchangedAliasesPreserveVerifiersAndScopes()
    {
        const auto update = BrokerAuthenticationAdmin::prepare(current, request(), resolve);
        QVERIFY2(update.error.isEmpty(), qPrintable(update.error));
        const auto policy = BrokerAuthentication::parse(update.document, resolve); QVERIFY(policy.policy);
        QCOMPARE(policy.policy->console.credentials.value(u"guest"_s).digest, current.policy->console.credentials.value(u"guest"_s).digest);
        QCOMPARE(policy.policy->virtualDesktop.credentials.value(u"guest"_s).salt, current.policy->virtualDesktop.credentials.value(u"guest"_s).salt);
        QCOMPARE(policy.policy->console.authenticate(u"guest"_s, u"fixture-only-password"_s), std::optional<quint32>(1000));
        QCOMPARE(policy.policy->virtualDesktop.authenticate(u"guest"_s, u"fixture-only-password"_s), std::optional<quint32>(1001));
        QVERIFY(!policy.policy->console.allowsPam(1001)); QVERIFY(!policy.policy->virtualDesktop.allowsPam(1000));
    }
    void passwordRotationAndOwnerChangeAreExplicit()
    {
        auto desired = request();
        alias(desired, u"console"_s, {{u"alias"_s, u"guest"_s}, {u"owner"_s, u"owner-b"_s}, {u"password"_s, u"rotated-fixture-password"_s}});
        const auto update = BrokerAuthenticationAdmin::prepare(current, desired, resolve); QVERIFY(update.error.isEmpty());
        QVERIFY(!update.document.contains("rotated-fixture-password"));
        const auto policy = BrokerAuthentication::parse(update.document, resolve); QVERIFY(policy.policy);
        QVERIFY(!policy.policy->console.authenticate(u"guest"_s, u"fixture-only-password"_s));
        QCOMPARE(policy.policy->console.authenticate(u"guest"_s, u"rotated-fixture-password"_s), std::optional<quint32>(1001));
        QCOMPARE(policy.policy->virtualDesktop.credentials.value(u"guest"_s).digest, current.policy->virtualDesktop.credentials.value(u"guest"_s).digest);
    }
    void invalidUpdates_data()
    {
        QTest::addColumn<QJsonObject>("desired");
        auto value = request(); value.insert(u"revision"_s, u"stale"_s); QTest::newRow("stale") << value;
        value = request(); value.insert(u"version"_s, true); QTest::newRow("version-type") << value;
        value = request(); value.insert(u"path"_s, u"/etc/other"_s); QTest::newRow("caller-path") << value;
        value = request(); value.insert(u"console"_s, false); QTest::newRow("route-type") << value;
        for (const auto &owner : {u"owner-b"_s, u"root"_s, u"missing"_s}) {
            value = request(); alias(value, u"console"_s, {{u"alias"_s, u"guest"_s}, {u"owner"_s, owner}});
            QTest::newRow(qPrintable(owner)) << value;
        }
        value = request(); alias(value, u"console"_s, {{u"alias"_s, u"new-guest"_s}, {u"owner"_s, u"owner-a"_s}}); QTest::newRow("rename-needs-password") << value;
        value = request(); alias(value, u"console"_s, {{u"alias"_s, u"guest"_s}, {u"owner"_s, u"owner-a"_s}, {u"verifier"_s, u"injected"_s}}); QTest::newRow("verifier-injection") << value;
        value = request(); alias(value, u"console"_s, {{u"alias"_s, u"guest"_s}, {u"owner"_s, u"owner-a"_s}, {u"password"_s, QString()}}); QTest::newRow("empty-password") << value;
        value = request(); alias(value, u"console"_s, {{u"alias"_s, u"guest"_s}, {u"owner"_s, u"owner-a"_s}, {u"password"_s, true}}); QTest::newRow("password-type") << value;
        value = request(); auto route = value.value(u"console"_s).toObject(); auto aliases = route.value(u"credentials"_s).toArray(); aliases.append(aliases.first()); route.insert(u"credentials"_s, aliases); value.insert(u"console"_s, route); QTest::newRow("duplicate-alias") << value;
        value = request(); route = value.value(u"console"_s).toObject(); route.insert(u"pam"_s, QJsonObject{{u"mode"_s, u"allow-list"_s}, {u"accounts"_s, QJsonArray{u"owner-a"_s, u"owner-a-alt"_s}}}); value.insert(u"console"_s, route); QTest::newRow("duplicate-pam-uid") << value;
    }
    void invalidUpdates()
    {
        QFETCH(QJsonObject, desired);
        const auto update = BrokerAuthenticationAdmin::prepare(current, desired, resolve);
        QVERIFY(update.document.isEmpty()); QVERIFY(!update.error.isEmpty());
        QVERIFY(!update.error.contains(u"fixture-only-password"_s));
        QCOMPARE(current.policy->console.credentials.value(u"guest"_s).ownerUid, quint32(1000));
    }
    void removalAndPamChangesPreserveOtherRoute()
    {
        auto desired = request();
        desired.insert(u"console"_s, QJsonObject{{u"pam"_s, QJsonObject{{u"mode"_s, u"disabled"_s}, {u"accounts"_s, QJsonArray{}}}}, {u"credentials"_s, QJsonArray{}}});
        const auto update = BrokerAuthenticationAdmin::prepare(current, desired, resolve); QVERIFY(update.error.isEmpty());
        const auto policy = BrokerAuthentication::parse(update.document, resolve); QVERIFY(policy.policy);
        QVERIFY(!policy.policy->console.allowsPam(1000)); QVERIFY(policy.policy->console.credentials.isEmpty());
        QVERIFY(policy.policy->virtualDesktop.allowsPam(1001)); QCOMPARE(policy.policy->virtualDesktop.credentials.size(), 1);
    }
    void missingPolicyRetainsBrokerDefaults()
    {
        QTemporaryDir directory;
        const auto absent = BrokerAuthentication::readFile(directory.filePath(u"absent.json"_s), false);
        QVERIFY(absent.policy); QVERIFY(absent.document.isEmpty());
        const auto snapshot = BrokerAuthenticationAdmin::view(absent, name); QVERIFY(snapshot.error.isEmpty());
        const auto update = BrokerAuthenticationAdmin::prepare(absent, snapshot.value, resolve); QVERIFY(update.error.isEmpty());
        const auto policy = BrokerAuthentication::parse(update.document, resolve); QVERIFY(policy.policy);
        QVERIFY(policy.policy->console.allowsPam(1000)); QVERIFY(policy.policy->virtualDesktop.allowsPam(1001));
        auto existing = policy; existing.document = update.document;
        QVERIFY(BrokerAuthenticationAdmin::revision(existing) != BrokerAuthenticationAdmin::revision(absent));
    }
};
QTEST_GUILESS_MAIN(BrokerAuthenticationAdminTest)
#include "BrokerAuthenticationAdminTest.moc"
