// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QTest>
#include "VirtualSessionLaunchPlan.h"
using namespace KRdp;
using namespace Qt::StringLiterals;

class VirtualSessionLaunchPlanTest : public QObject
{
    Q_OBJECT
    const QString id = u"a512ca25-048d-42f6-a2ed-a2bb2da22f22"_s;
    const VirtualSessionLaunchPlan::Account account{1000, u"user"_s, u"/home/user"_s};
    const VirtualSessionLaunchPlan::Configuration config{u"/usr/libexec/krdp/launch-virtual-session"_s,
        u"/usr/bin/krdp-console-worker"_s, u"/usr/share/krdp"_s, {u"0000:09:00.0"_s, u"0000:c5:00.0"_s}, {1280, 720}};
private Q_SLOTS:
    void recordedLaunchIdentityIsNotRegenerated()
    {
        const auto launch = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto first = VirtualSessionLaunchPlan::build(1000, account, id, config, nullptr, launch);
        const auto second = VirtualSessionLaunchPlan::build(1000, account, id, config, nullptr, launch);
        QVERIFY(first); QVERIFY(second);
        QCOMPARE(first->runtimeDirectory, QStringLiteral("/run/user/1000/krdp-virtual/") + launch);
        QCOMPARE(first->runtimeDirectory, second->runtimeDirectory);
        QCOMPARE(first->arguments, second->arguments);
        QVERIFY(first->arguments.contains(first->runtimeDirectory));
        for (const auto &bad : {QStringLiteral("../old"), QStringLiteral("00000000-0000-0000-0000-000000000000"),
                QStringLiteral("{12345678-1234-1234-1234-123456789abc}")})
            QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, config, nullptr, bad));
    }
    void persistentProfileSeparateFromRuntime()
    {
        const auto plan = VirtualSessionLaunchPlan::build(1000, account, id, config);
        QVERIFY(plan);
        QVERIFY(plan->runtimeDirectory.startsWith(u"/run/user/1000/krdp-virtual/"_s));
        QCOMPARE(plan->profileDirectory, u"/home/user/.krdp-virtual/sessions/"_s + id);
        QVERIFY(!plan->environment.inheritsFromParent());
        QCOMPARE(plan->environment.keys().size(), 5);
        QVERIFY(!plan->environment.contains(u"DISPLAY"_s));
        QVERIFY(!plan->environment.contains(u"DBUS_SESSION_BUS_ADDRESS"_s));
        QVERIFY(!plan->environment.contains(u"PULSE_SERVER"_s));
        QVERIFY(!plan->arguments.contains(u"timeout"_s));
        QCOMPARE(plan->arguments.count(u"--allow-render-pci"_s), 2);
        const auto second = VirtualSessionLaunchPlan::build(1000, account, id, config);
        QCOMPARE(second->profileDirectory, plan->profileDirectory);
        QVERIFY(second->runtimeDirectory != plan->runtimeDirectory);
        QVERIFY(second->socketPath != plan->socketPath);
    }
    void refusesUntrustedIdentityAndPaths()
    {
        QVERIFY(!VirtualSessionLaunchPlan::build(0, account, id, config));
        QVERIFY(!VirtualSessionLaunchPlan::build(1001, account, id, config));
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, u"../physical"_s, config));
        auto changed = config;
        changed.launcher = u"relative-helper"_s;
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
        changed = config; changed.supportDirectory = u"/usr/share/../krdp"_s;
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
    }
    void gpuAccessMustBeExplicitAndStable()
    {
        auto changed = config;
        changed.allowedRenderPci.clear();
        QString error;
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed, &error));
        QVERIFY(error.contains(u"CPU-only"_s));
        changed.allowedRenderPci = {u"/dev/dri/renderD128"_s};
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
        changed.allowedRenderPci = {u"0000:09:00.0"_s, u"0000:09:00.0"_s};
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
        changed = config; changed.initialSize = QSize(4098, 720);
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
        changed.initialSize = QSize(1279, 720);
        QVERIFY(!VirtualSessionLaunchPlan::build(1000, account, id, changed));
    }
};
QTEST_GUILESS_MAIN(VirtualSessionLaunchPlanTest)
#include "VirtualSessionLaunchPlanTest.moc"
