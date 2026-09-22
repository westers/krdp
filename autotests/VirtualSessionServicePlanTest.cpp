// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionServicePlan.h"
#include "VirtualHostTls.h"
#include <QTest>
#include <QFile>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <unistd.h>

using namespace KRdp;
class VirtualSessionServicePlanTest : public QObject {
    Q_OBJECT
    static QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    const VirtualSessionLaunchPlan::Account account{1000, QStringLiteral("user"), QStringLiteral("/home/user")};
    const VirtualSessionLaunchPlan::Configuration config{QStringLiteral("/usr/share/krdp/virtual-session/launch-virtual-session.sh"),
        QStringLiteral("/usr/bin/krdp-console-worker"), QStringLiteral("/usr/share/krdp/virtual-session/support"),
        {QStringLiteral("0000:09:00.0")}, {1280, 720}};
    const QString device = QStringLiteral("/usr/bin/krdp-virtual-device-entry"), guardian = QStringLiteral("/usr/bin/krdp-virtual-guardian");
private Q_SLOTS:
    void pamKeeperRequiresCleanPrivilegedOwner() {
        QProcess keeper;
        QProcessEnvironment environment;
        environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
        environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
        environment.insert(QStringLiteral("DBUS_SYSTEM_BUS_ADDRESS"), QStringLiteral("unix:path=/not-a-real-bus"));
        keeper.setProcessEnvironment(environment);
        keeper.start(QString::fromLocal8Bit(KRDP_PAM_KEEPER), {});
        QVERIFY(keeper.waitForFinished(3000)); QCOMPARE(keeper.exitCode(), 1);
        QVERIFY(keeper.readAllStandardError().contains("unclean environment"));
        if (!getuid()) return;
        environment.remove(QStringLiteral("DBUS_SYSTEM_BUS_ADDRESS"));
        keeper.setProcessEnvironment(environment);
        keeper.start(QString::fromLocal8Bit(KRDP_PAM_KEEPER), {});
        QVERIFY(keeper.waitForFinished(3000)); QCOMPARE(keeper.exitCode(), 1);
        QVERIFY(keeper.readAllStandardError().contains("explicit root service required"));
    }
    void tlsRequiresMatchingPemAndNeverPrompts() {
        const auto openssl = QStandardPaths::findExecutable(QStringLiteral("openssl"));
        if (openssl.isEmpty()) QSKIP("OpenSSL fixture generator unavailable");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto cert = directory.filePath(QStringLiteral("certificate.pem"));
        const auto key = directory.filePath(QStringLiteral("key.pem"));
        const auto otherCert = directory.filePath(QStringLiteral("other.pem"));
        const auto otherKey = directory.filePath(QStringLiteral("other-key.pem"));
        const auto generate = [&](const QString &c, const QString &k) {
            QProcess process;
            process.start(openssl, {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"),
                QStringLiteral("rsa:2048"), QStringLiteral("-nodes"), QStringLiteral("-days"), QStringLiteral("1"),
                QStringLiteral("-subj"), QStringLiteral("/CN=krdp-test"), QStringLiteral("-out"), c, QStringLiteral("-keyout"), k});
            return process.waitForFinished(10000) && process.exitCode() == 0;
        };
        QVERIFY(generate(cert, key)); QVERIFY(generate(otherCert, otherKey));
        QVERIFY(validVirtualHostTls(cert, key)); QVERIFY(!validVirtualHostTls(cert, otherKey));
        QVERIFY(!validVirtualHostTls(key, cert));
        const auto encrypted = directory.filePath(QStringLiteral("encrypted.pem"));
        QProcess encrypt;
        encrypt.start(openssl, {QStringLiteral("pkey"), QStringLiteral("-in"), key, QStringLiteral("-aes-256-cbc"),
            QStringLiteral("-passout"), QStringLiteral("pass:test-fixture-only"), QStringLiteral("-out"), encrypted});
        QVERIFY(encrypt.waitForFinished(10000)); QCOMPARE(encrypt.exitCode(), 0);
        QVERIFY(!validVirtualHostTls(cert, encrypted));
        QFile bad(otherKey); QVERIFY(bad.open(QIODevice::WriteOnly | QIODevice::Truncate)); bad.close();
        QVERIFY(!validVirtualHostTls(cert, otherKey));
        QVERIFY(!validVirtualHostTls(cert, directory.path()));
        QVERIFY(!validVirtualHostTls(cert, directory.filePath(QStringLiteral("missing.pem"))));
        QVERIFY(bad.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray oversized(1024 * 1024 + 1, 'x');
        QCOMPARE(bad.write(oversized), qint64(oversized.size())); bad.close();
        QVERIFY(!validVirtualHostTls(cert, otherKey));
        QVERIFY(bad.open(QIODevice::WriteOnly)); QCOMPARE(bad.write("not a key"), qint64(9)); bad.close();
        QVERIFY(!validVirtualHostTls(cert, otherKey));
    }
    void recordedIdentityAndFdOnlyCredential() {
        const VirtualSessionJournal::Record r{1000, id(), id(), id(), id(), QByteArray(32, 's')};
        const auto p = VirtualSessionServicePlan::build(r, r.boot, account, config, device, guardian);
        QVERIFY(p); QCOMPARE(p->program, device);
        QCOMPARE(p->credential, r.token);
        QCOMPARE(p->runtime + QStringLiteral("/worker.sock"), r.workerSocket());
        const auto args = p->arguments;
        QVERIFY(args.contains(r.session)); QVERIFY(args.contains(r.incarnation)); QVERIFY(args.contains(r.launch));
        QCOMPARE(args[args.indexOf(QStringLiteral("--token-fd")) + 1], QStringLiteral("0"));
        QVERIFY(!args.join(QLatin1Char(' ')).contains(QString::fromLatin1(r.token)));
        QVERIFY(!args.join(QLatin1Char(' ')).contains(QString::fromLatin1(r.token.toHex())));
        QCOMPARE(args.count(QStringLiteral("--allow-render-pci")), 2);
        QVERIFY(!args.contains(QStringLiteral("timeout")));
    }
    void refusesStaleBootWrongOwnerAndMissingGpuPolicy() {
        const VirtualSessionJournal::Record r{1000, id(), id(), id(), id(), QByteArray(32, 's')};
        QVERIFY(!VirtualSessionServicePlan::build(r, id(), account, config, device, guardian));
        auto other = account; other.uid = 1001;
        QVERIFY(!VirtualSessionServicePlan::build(r, r.boot, other, config, device, guardian));
        auto policy = config; policy.allowedRenderPci.clear();
        QVERIFY(!VirtualSessionServicePlan::build(r, r.boot, account, policy, device, guardian));
        QVERIFY(!VirtualSessionServicePlan::build(r, r.boot, account, config, QStringLiteral("../helper"), guardian));
        auto broken = r; broken.token.clear();
        QVERIFY(!VirtualSessionServicePlan::build(broken, r.boot, account, config, device, guardian));
    }
    void executableRefusesUnprivilegedInvocation() {
        if (!getuid()) QSKIP("Nonroot refusal fixture");
        QProcess entry;
        entry.start(QString::fromLocal8Bit(KRDP_SESSION_ENTRY), {QStringLiteral("--session"), id()});
        QVERIFY(entry.waitForFinished(3000));
        QCOMPARE(entry.exitStatus(), QProcess::NormalExit); QCOMPARE(entry.exitCode(), 1);
        QVERIFY(entry.readAllStandardError().contains("explicit root service required"));
    }
    void brokerRefusesUnprivilegedInvocation() {
        if (!getuid()) QSKIP("Nonroot refusal fixture");
        QProcess host;
        host.start(QString::fromLocal8Bit(KRDP_VIRTUAL_HOST), {});
        QVERIFY(host.waitForFinished(3000));
        QCOMPARE(host.exitStatus(), QProcess::NormalExit); QCOMPARE(host.exitCode(), 1);
        QVERIFY(host.readAllStandardError().contains("explicit root service invocation"));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionServicePlanTest)
#include "VirtualSessionServicePlanTest.moc"
