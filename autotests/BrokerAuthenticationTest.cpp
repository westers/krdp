// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "BrokerAuthentication.h"
#include <Server.h>
#include <RdpConnection.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QFile>
#include <QTest>
#include <sys/stat.h>
#include <unistd.h>

using namespace KRdp;
using namespace KRdp::BrokerAuthentication;
using namespace Qt::StringLiterals;

class BrokerAuthenticationTest : public QObject
{
    Q_OBJECT
    static QJsonObject route(const QString &mode, const QJsonArray &accounts = {}, const QJsonArray &credentials = {})
    {
        return {{u"pam"_s, QJsonObject{{u"mode"_s, mode}, {u"accounts"_s, accounts}}}, {u"credentials"_s, credentials}};
    }
    static QJsonObject policy(const QJsonObject &console, const QJsonObject &virtualDesktop = route(u"disabled"_s))
    {
        return {{u"version"_s, 1}, {u"console"_s, console}, {u"virtual"_s, virtualDesktop}};
    }
    static std::optional<quint32> resolve(const QString &name)
    {
        if (name == u"owner-a"_s) return 1000;
        if (name == u"owner-b"_s) return 1001;
        if (name == u"root"_s) return 0;
        return {};
    }
    static Result parseObject(const QJsonObject &object) { return parse(QJsonDocument(object).toJson(), resolve); }
private Q_SLOTS:
    void canonicalPamIdentityAndRouteSeparation()
    {
        const auto result = parseObject(policy(route(u"allow-list"_s, {u"owner-a"_s}), route(u"allow-list"_s, {u"owner-b"_s})));
        QVERIFY(result.policy);
        QVERIFY(result.policy->console.allowsPam(1000));
        QVERIFY(!result.policy->console.allowsPam(1001));
        QVERIFY(!result.policy->console.allowsPam(0));
        QVERIFY(result.policy->virtualDesktop.allowsPam(1001));
        QVERIFY(!result.policy->virtualDesktop.allowsPam(1000));
        QVERIFY(!result.policy->virtualDesktop.allowsPam(quint32(-1)));
        const auto any = parseObject(policy(route(u"any"_s)));
        QVERIFY(any.policy && any.policy->console.allowsPam(1001));
        QVERIFY(!any.policy->virtualDesktop.allowsPam(1001));
    }
    void customAliasRemainsBoundToOriginalOwner()
    {
        const auto credential = makeCredential(1000, u"test-only-secret"_s);
        QVERIFY(credential);
        const QString verifier = u"pbkdf2-sha256$600000$"_s + QString::fromLatin1(credential->salt.toHex())
            + u"$"_s + QString::fromLatin1(credential->digest.toHex());
        const QJsonObject alias{{u"alias"_s, u"owner-b"_s}, {u"owner"_s, u"owner-a"_s}, {u"verifier"_s, verifier}};
        const auto result = parseObject(policy(route(u"disabled"_s, {}, {alias})));
        QVERIFY(result.policy);
        QCOMPARE(result.policy->console.authenticate(u"owner-b"_s, u"test-only-secret"_s), std::optional<quint32>(1000));
        QVERIFY(!result.policy->console.authenticate(u"owner-a"_s, u"test-only-secret"_s));
        QVERIFY(!result.policy->console.authenticate(u"owner-b"_s, u"wrong"_s));
        QVERIFY(!result.policy->virtualDesktop.authenticate(u"owner-b"_s, u"test-only-secret"_s));
        QVERIFY(!makeCredential(0, u"test-only-secret"_s));
        QVERIFY(!makeCredential(1000, {}));
        QVERIFY(!makeCredential(1000, QString(4097, u'x')));
        Server server;
        server.setUsers({{u"legacy-unscoped"_s, u"test-only-secret"_s}});
        QVERIFY(apply(server, result.policy->console));
        QVERIFY(!server.usePAMAuthentication());
        QVERIFY(!server.matchesConfiguredUser(u"legacy-unscoped"_s, u"test-only-secret"_s));
        QCOMPARE(server.mappedCredentialIdentity(u"owner-b"_s, u"test-only-secret"_s), std::optional<quint32>(1000));
        RdpConnection connection(&server, -1);
        QVERIFY(!connection.authenticatedPamUid());
        QVERIFY(!connection.authenticatedUserUid()); // Verifying another credential does not authorize this connection.
    }
    void malformedPolicyIsAtomic_data()
    {
        QTest::addColumn<QJsonObject>("object");
        QTest::newRow("unknown-mode") << policy(route(u"some"_s));
        QTest::newRow("root") << policy(route(u"allow-list"_s, {u"root"_s}));
        QTest::newRow("unresolved") << policy(route(u"allow-list"_s, {u"missing"_s}));
        QTest::newRow("duplicate-account") << policy(route(u"allow-list"_s, {u"owner-a"_s, u"owner-a"_s}));
        QTest::newRow("disabled-accounts") << policy(route(u"disabled"_s, {u"owner-a"_s}));
        QTest::newRow("bad-other-route") << policy(route(u"allow-list"_s, {u"owner-a"_s}), route(u"any"_s, {u"owner-b"_s}));
        auto unknown = policy(route(u"any"_s)); unknown.insert(u"users"_s, QJsonArray{});
        QTest::newRow("unknown-key") << unknown;
        auto version = policy(route(u"any"_s)); version[u"version"_s] = true;
        QTest::newRow("wrong-version-type") << version;
        const QJsonObject alias{{u"alias"_s, u"guest"_s}, {u"owner"_s, u"owner-a"_s}, {u"verifier"_s, u"plaintext"_s}};
        QTest::newRow("plaintext") << policy(route(u"disabled"_s, {}, {alias}));
        const QString valid = u"pbkdf2-sha256$600000$"_s + QString(32, u'0') + u"$"_s + QString(64, u'0');
        auto malformed = alias; malformed[u"verifier"_s] = valid;
        QTest::newRow("duplicate-alias") << policy(route(u"disabled"_s, {}, {malformed, malformed}));
        malformed[u"owner"_s] = u"root"_s;
        QTest::newRow("root-owner") << policy(route(u"disabled"_s, {}, {malformed}));
        malformed[u"owner"_s] = u"missing"_s;
        QTest::newRow("unresolved-owner") << policy(route(u"disabled"_s, {}, {malformed}));
        malformed[u"owner"_s] = u"owner-a"_s;
        malformed[u"alias"_s] = u"bad\nname"_s;
        QTest::newRow("control-alias") << policy(route(u"disabled"_s, {}, {malformed}));
        malformed[u"alias"_s] = u"guest"_s;
        malformed[u"verifier"_s] = QString(valid).replace(u"600000"_s, u"1"_s);
        QTest::newRow("wrong-work-factor") << policy(route(u"disabled"_s, {}, {malformed}));
    }
    void malformedPolicyIsAtomic()
    {
        QFETCH(QJsonObject, object);
        const auto result = parseObject(object);
        QVERIFY(!result.policy);
        QCOMPARE(result.error, u"invalid authentication policy"_s);
    }
    void snapshotCannotChangeWhileListening()
    {
        Server server;
        const Route initial{PamMode::AllowList, {1000}, {}};
        QVERIFY(apply(server, initial));
        QVERIFY(server.acceptsPamIdentity(1000));
        QVERIFY(!server.acceptsPamIdentity(1001));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0)); // TCP listener only; no peer/daemon/bus/capture.
        QVERIFY(!apply(server, Route{PamMode::Any, {}, {}}));
        QVERIFY(!server.acceptsPamIdentity(1001));
        server.close();
    }
    void boundedRootFileAndExplicitMissingPath()
    {
        QTemporaryDir directory;
        const auto absent = directory.filePath(u"absent"_s);
        const auto inherited = readFile(absent, false);
        QVERIFY(inherited.policy && inherited.policy->console.allowsPam(1000));
        QVERIFY(!readFile(absent, true).policy);
        const auto path = directory.filePath(u"policy"_s);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(policy(route(u"any"_s))).toJson()); file.close();
        chmod(QFile::encodeName(path).constData(), 0600);
        QVERIFY(!readFile(path, true).policy); // Test account is not root.
        const auto link = directory.filePath(u"link"_s);
        QVERIFY(QFile::link(path, link));
        QVERIFY(!readFile(link, true).policy);
        QVERIFY(!parse(QByteArray(MaximumBytes + 1, ' '), resolve).policy);
        QVERIFY(!parse(QByteArray("{}\0", 3), resolve).policy);
    }
};
QTEST_GUILESS_MAIN(BrokerAuthenticationTest)
#include "BrokerAuthenticationTest.moc"
