// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionServicePlan.h"
#include <QTest>
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
};
QTEST_GUILESS_MAIN(VirtualSessionServicePlanTest)
#include "VirtualSessionServicePlanTest.moc"
